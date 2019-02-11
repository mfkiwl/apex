#include <arch.h>

#include <cpu.h>
#include <debug.h>
#include <kernel.h>
#include <sections.h>
#include <sig.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <task.h>
#include <thread.h>
#include <vm.h>

__fast_bss size_t fixed;	    /* number of fixed regions */
__fast_bss size_t stack;	    /* number of stack regions */
__fast_bss size_t victim;	    /* next victim to evict */
__fast_bss const void *fault_addr;  /* last fault address */
const struct thread *mapped_thread; /* currently mapped thread */

static void
clear_dynamic(void)
{
	const size_t regions = MPU->TYPE.DREGION;
	for (size_t i = fixed + stack; i < regions; ++i) {
		write32(&MPU->RNR, i);
		write32(&MPU->RASR, 0);
	}
	fault_addr = 0;
}

static void
static_region(const struct mmumap *map, size_t i)
{
	if (!is_pow2(map->size))
		panic("region must be power-of-2 sized");
	if ((uintptr_t)map->paddr & (map->size - 1))
		panic("region must be aligned on size boundary");
	write32(&MPU->RBAR, (union mpu_rbar){
		.REGION = i,
		.VALID = 1,
		.ADDR = (uintptr_t)map->paddr >> 5,
	}.r);
	write32(&MPU->RASR, map->flags | (union mpu_rasr){
		.SIZE = floor_log2(map->size) - 1,
		.ENABLE = 1,
	}.r);
}

__fast_text static uint32_t
prot_to_rasr(int prot)
{
	switch (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
	case PROT_READ:
		return RASR_USER_R_WBWA;
	case PROT_READ | PROT_EXEC:
		return RASR_USER_RX_WBWA;
	case PROT_READ | PROT_WRITE:
		return RASR_USER_RW_WBWA;
	case PROT_READ | PROT_WRITE | PROT_EXEC:
		return RASR_USER_RWX_WBWA;
	default:
		panic("bad prot");
	}
}

/*
 * mpu_init - initialise memory protection unit
 */
void
mpu_init(const struct mmumap *map, size_t count, int flags)
{
	const size_t regions = MPU->TYPE.DREGION;

	if (!regions)
		panic("MPU not implemented");
	if (regions > 16)
		panic("MPU not supported"); /* RBAR.REGION supports 0 to 15 */
	if (count >= regions - 2)
		panic("invalid");

	/* all regions must be initialised before enabling */
	for (size_t i = 0; i < count; ++i)
		static_region(map + i, i);
	fixed = victim = count;
	clear_dynamic();

	write32(&MPU->CTRL, (union mpu_ctrl){
		.PRIVDEFENA = !!(flags & MPU_ENABLE_DEFAULT_MAP),
		.HFNMIENA = 1,
		.ENABLE = 1,
	}.r);

	dbg("PMSAv7 MPU initialised, %d dynamic regions\n", regions - fixed);
}

/*
 * mpu_switch - switch mpu to new address space
 */
void
mpu_switch(const struct as *as)
{
	stack = 0;
	mapped_thread = NULL;
	clear_dynamic();
}

/*
 * mpu_thread_switch - switch mpu to new thread
 */
__attribute__((used)) void
mpu_thread_switch(struct thread *prev, struct thread *t)
{
	if (t->task == &kern_task)
		return;
	if (t == mapped_thread)
		return;
	if (mapped_thread && mapped_thread->task != t->task)
		mpu_switch(t->task->as);

	/* map stack */
	const struct seg *seg = as_find_seg(t->task->as, (void *)t->ctx.kregs.psp);
	if (!seg || seg_prot(seg) == PROT_NONE) {
		sig_thread(t, SIGSEGV);
		return;
	}
	stack = 0;
	const uint32_t rasr_prot = prot_to_rasr(seg_prot(seg));
	for (void *a = seg_begin(seg); a < seg_end(seg);) {
		const size_t size = seg_end(seg) - a;
		size_t o = MIN(__builtin_ctz((uintptr_t)a), floor_log2(size));
		write32(&MPU->RBAR, (union mpu_rbar){
			.REGION = fixed + stack,
			.VALID = 1,
			.ADDR = (uintptr_t)a >> 5,
		}.r);
		write32(&MPU->RASR, rasr_prot | (union mpu_rasr){
			.SIZE = o - 1,
			.ENABLE = 1,
		}.r);
		a += 1 << o;
		++stack;
	}

	mapped_thread = t;
	if (victim < fixed + stack)
		victim = fixed + stack;
}

/*
 * mpu_unmap - unmap region from currently active address space
 */
void
mpu_unmap(const void *addr, size_t len)
{
	clear_dynamic();
}

/*
 * mpu_map - map region into currently active address space
 */
void
mpu_map(const void *addr, size_t len, int prot)
{
	/* nothing to do, rely on fault handler */
}

/*
 * mpu_protect - change protection flags on address range in currently active
 *		 address space
 */
void
mpu_protect(const void *addr, size_t len, int prot)
{
	clear_dynamic();
}

/*
 * mpu_fault - handle mpu fault
 */
__fast_text void
mpu_fault(const void *addr, size_t len)
{
	const struct seg *seg = as_find_seg(task_cur()->as, addr);

	/* double fault at the same address means that last time we faulted in
	   a region it didn't satisfy the MPU */
	if (!seg || seg_prot(seg) == PROT_NONE || addr == fault_addr) {
		sig_thread(thread_cur(), SIGSEGV);
		return;
	}
	fault_addr = addr;

again:;
	/* find largest power-of-2 sized region containing addr within seg */
	const size_t order = MIN(
	    floor_log2((uintptr_t)addr ^ (uintptr_t)seg_end(seg)),
	    floor_log2((-(uintptr_t)addr - 1) ^ -(uintptr_t)seg_begin(seg)));

	/* configure MPU */
	const uintptr_t region_base = (uintptr_t)addr & -(1UL << order);
	write32(&MPU->RBAR, (union mpu_rbar){
		.REGION = victim,
		.VALID = 1,
		.ADDR = region_base >> 5,
	}.r);
	write32(&MPU->RASR, prot_to_rasr(seg_prot(seg)) | (union mpu_rasr){
		.SIZE = order - 1,
		.ENABLE = 1,
	}.r);

	if (++victim == MPU->TYPE.DREGION)
		victim = fixed + stack;

	if (len) {
		addr += len;
		const void *region_end = (void *)region_base + (1UL << order);
		if (addr < seg_end(seg) && addr >= region_end) {
			len = 0;
			goto again;
		}
	}
}

/*
 * mpu_dump - dump mpu state
 */
void
mpu_dump(void)
{
#if defined(CONFIG_DEBUG)
	dbg("*** MPU dump ***\n");
	dbg("fixed:%x stack:%x victim:%x fault_addr:%8p\n",
	    fixed, stack, victim, fault_addr);

	const union mpu_type type = MPU->TYPE;
	dbg("MPU_TYPE %08x: SEPARATE:%d IREGION:%d DREGION:%d\n",
	    type.r, type.SEPARATE, type.IREGION, type.DREGION);

	const union mpu_ctrl ctrl = MPU->CTRL;
	dbg("MPU_CTRL %08x: ENABLE:%d HFNMIENA:%d PRIVDEFENA:%d\n",
	    ctrl.r, ctrl.ENABLE, ctrl.HFNMIENA, ctrl.PRIVDEFENA);

	for (size_t i = 0; i < MPU->TYPE.DREGION; ++i) {
		write32(&MPU->RNR, i);

		const union mpu_rbar rbar = MPU->RBAR;
		const union mpu_rasr rasr = MPU->RASR;

		assert(rbar.REGION == i);

		if (rasr.ENABLE)
			dbg("Region %x: ADDR:%08x SIZE:%08x SRD:%02x TEX:%x C:%d B:%d S:%d AP:%x XN:%d\n",
			    i, rbar.ADDR << 5, 1 << (rasr.SIZE + 1), rasr.SRD, rasr.TEX, rasr.C, rasr.B, rasr.S, rasr.AP, rasr.XN);
		else
			dbg("Region %x: disabled\n", i);
	}
#endif
}
