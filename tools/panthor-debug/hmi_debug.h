/* HMI debug (round 3): force-included in panthor, drm_gpuvm and drm_exec.
 * Every dma_resv_unlock() checks that the calling task owns the lock. On a
 * non-debug kernel unlocking somebody else's mutex silently releases it:
 * the VM reservation was seen unlocked right after panthor took it.
 */
#ifndef HMI_DEBUG_H
#define HMI_DEBUG_H
#include <linux/dma-resv.h>
#include <linux/sched.h>
#include <linux/printk.h>

static inline void hmi_dma_resv_unlock(struct dma_resv *r, const char *fn, int line)
{
	unsigned long owner = (unsigned long)atomic_long_read(&r->lock.base.owner) & ~0x07UL;

	if (owner != (unsigned long)current) {
		static atomic_t hmi_reports = ATOMIC_INIT(0);

		if (atomic_inc_return(&hmi_reports) <= 8) {
			pr_err("panthor-debug: dma_resv_unlock(%px) at %s:%d by task %px (%s) but owner is %lx\n",
			       r, fn, line, current, current->comm, owner);
			dump_stack();
		}
	}
	dma_resv_reset_max_fences(r);
	ww_mutex_unlock(&r->lock);
}
#define dma_resv_unlock(r) hmi_dma_resv_unlock((r), __func__, __LINE__)
#endif
