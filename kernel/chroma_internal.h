/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CHROMA_INTERNAL_H
#define CHROMA_INTERNAL_H

#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/irq_work.h>
#include <linux/kallsyms.h>
#include <linux/kobject.h>
#include <linux/kprobes.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_property.h>

#include "prisma_abi.h"

#ifdef PRISMA_DRM_ATOMIC_COMMIT
#define chroma_atomic_state drm_atomic_commit
#define chroma_atomic_alloc drm_atomic_commit_alloc
#define chroma_atomic_put drm_atomic_commit_put
#else
#define chroma_atomic_state drm_atomic_state
#define chroma_atomic_alloc drm_atomic_state_alloc
#define chroma_atomic_put drm_atomic_state_put
#endif

/* Only simultaneous discoveries are bounded. Tracked devices and CRTCs are dynamic. */
#define CHROMA_PENDING_GPUS 32
#define CHROMA_RECHECK (2 * HZ)
#define CHROMA_EBUSY_BACKOFF msecs_to_jiffies(20)
#define CHROMA_MAX_BUSY_RETRIES 5

enum chroma_enqueue_result {
	CHROMA_ENQUEUE_OK = 0,
	CHROMA_ENQUEUE_ALREADY_TRACKED,
	CHROMA_ENQUEUE_FULL,
	CHROMA_ENQUEUE_MODULE_FAIL,
	CHROMA_ENQUEUE_INVALID,
};

struct chroma_gpu {
	struct list_head link;
	struct drm_device *dev;
	struct module *owner;
	unsigned int num_crtcs;
	struct drm_property_blob *owned[];
};

/* --- Global State & Synchronization (defined in chroma_main.c) --- */
extern struct mutex config_lock;
extern struct prisma_ioctl_state config;

extern raw_spinlock_t discovery_lock;
extern struct list_head gpus;
extern struct drm_device *pending[CHROMA_PENDING_GPUS];
extern bool paused;
extern bool refresh_queued;
extern struct workqueue_struct *chroma_wq;
extern struct work_struct refresh_work;
extern struct timer_list recheck_timer;
extern struct irq_work discovery_irq;
extern struct kprobe chroma_kp;

extern unsigned int kprobe_drops_full;
extern unsigned int kprobe_drops_module;

/* --- Main / Scheduler / State (chroma_main.c) --- */
void chroma_schedule(void);
void chroma_pause(void);
struct prisma_ioctl_state chroma_snapshot(void);

/* --- CTM Computations (chroma_ctm.c) --- */
u64 to_s31_32(s64 num, u64 den);
void chroma_compute_ctm(struct drm_color_ctm *ctm, int sat_val, int con_val, int bri_val);
bool chroma_is_identity_ctm(const struct drm_color_ctm *ctm);

/* --- DRM & GPU Operations (chroma_drm.c) --- */
int chroma_observe(struct kprobe *probe, struct pt_regs *regs);
void chroma_scan_pci(void);
void chroma_collect_gpus(void);
int chroma_update_gpu(struct chroma_gpu *gpu, const struct drm_color_ctm *target, bool *had_ebusy);
void chroma_forget_gpu(struct chroma_gpu *gpu);
void chroma_put_gpu(struct drm_device *dev, struct module *owner);
enum chroma_enqueue_result chroma_try_enqueue_locked(struct drm_device *dev);

/* --- Device & Sysfs Interface (chroma_iface.c) --- */
int chroma_iface_init(void);
void chroma_iface_exit(void);

#endif /* CHROMA_INTERNAL_H */
