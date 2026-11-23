// SPDX-License-Identifier: GPL-2.0
#include "chroma_internal.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("prism-amd Contributors");
MODULE_DESCRIPTION("Atomic DRM/KMS hardware color calibration for AMDGPU");
MODULE_VERSION(PRISMA_VERSION);

DEFINE_MUTEX(config_lock);
struct prisma_ioctl_state config = {
	.abi_version = PRISMA_ABI_VERSION,
	.enabled = 1,
	.saturation = 100,
	.contrast = 100,
};

struct prisma_ioctl_state chroma_snapshot(void)
{
	struct prisma_ioctl_state state;

	mutex_lock(&config_lock);
	state = config;
	mutex_unlock(&config_lock);
	return state;
}

unsigned int kprobe_drops_full;
unsigned int kprobe_drops_module;

DEFINE_RAW_SPINLOCK(discovery_lock);
LIST_HEAD(gpus);
struct drm_device *pending[CHROMA_PENDING_GPUS];
bool paused;
bool refresh_queued;
struct workqueue_struct *chroma_wq;
struct work_struct refresh_work;

static void chroma_refresh(struct work_struct *work);

void chroma_schedule(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&discovery_lock, flags);
	if (!paused && !refresh_queued) {
		refresh_queued = true;
		queue_work(chroma_wq, &refresh_work);
	}
	raw_spin_unlock_irqrestore(&discovery_lock, flags);
}

static void chroma_discovery_ready(struct irq_work *work)
{
	chroma_schedule();
}

DEFINE_IRQ_WORK(discovery_irq, chroma_discovery_ready);

static void chroma_recheck(struct timer_list *timer)
{
	chroma_schedule();
}

DEFINE_TIMER(recheck_timer, chroma_recheck);

static unsigned int busy_retry_count;

static void chroma_refresh(struct work_struct *work)
{
	struct prisma_ioctl_state snapshot;
	struct chroma_gpu *gpu, *next, *gpu_count;
	struct drm_color_ctm ctm;
	struct drm_color_ctm *target = NULL;
	unsigned int num_gpus = 0, num_pending = 0;
	unsigned int drops_full = 0, drops_module = 0;
	unsigned int p_idx;
	unsigned long flags;
	bool had_ebusy = false;
	bool has_owned = false;
	bool retry_pending = false;

	raw_spin_lock_irqsave(&discovery_lock, flags);
	refresh_queued = false;
	if (paused) {
		raw_spin_unlock_irqrestore(&discovery_lock, flags);
		return;
	}
	drops_full = kprobe_drops_full;
	kprobe_drops_full = 0;
	drops_module = kprobe_drops_module;
	kprobe_drops_module = 0;
	list_for_each_entry(gpu_count, &gpus, link)
		num_gpus++;
	for (p_idx = 0; p_idx < ARRAY_SIZE(pending); ++p_idx) {
		if (pending[p_idx])
			num_pending++;
	}
	raw_spin_unlock_irqrestore(&discovery_lock, flags);

	if (drops_full)
		pr_debug("chroma_amd: kprobe dropped %u device discoveries (pending slots full)\n",
			 drops_full);
	if (drops_module)
		pr_debug("chroma_amd: kprobe dropped %u device discoveries (try_module_get failed)\n",
			 drops_module);

	pr_debug("chroma_amd: refresh: %u tracked GPUs, %u pending\n", num_gpus, num_pending);

	if (list_empty(&gpus)) {
		pr_debug("chroma_amd: no tracked GPUs, initiating proactive PCI scan\n");
		chroma_scan_pci();
	}

	snapshot = chroma_snapshot();
	if (snapshot.enabled && snapshot.saturation != 100) {
		chroma_compute_ctm(&ctm, snapshot.saturation);
		target = &ctm;
	}
	bool need_gamma = snapshot.enabled && (snapshot.contrast != 100 || snapshot.brightness != 0);

	chroma_collect_gpus();
	list_for_each_entry_safe(gpu, next, &gpus, link) {
		unsigned int c;

		chroma_update_gpu(gpu, target, snapshot.contrast, snapshot.brightness,
				  need_gamma, &had_ebusy);

		for (c = 0; c < gpu->num_crtcs; ++c) {
			if (gpu->owned[c].ctm || gpu->owned[c].gamma_lut) {
				has_owned = true;
				break;
			}
		}
		if (drm_dev_is_unplugged(gpu->dev))
			chroma_forget_gpu(gpu);
	}

	if (had_ebusy) {
		if (busy_retry_count < CHROMA_MAX_BUSY_RETRIES) {
			busy_retry_count++;
			retry_pending = true;
			pr_debug("chroma_amd: rescheduling refresh due to -EBUSY in 20ms (retry %u/%u)\n",
				 busy_retry_count, CHROMA_MAX_BUSY_RETRIES);
			raw_spin_lock_irqsave(&discovery_lock, flags);
			if (!paused)
				mod_timer(&recheck_timer, jiffies + CHROMA_EBUSY_BACKOFF);
			raw_spin_unlock_irqrestore(&discovery_lock, flags);
		} else {
			pr_warn_ratelimited("chroma_amd: exceeded maximum -EBUSY retries (%u)\n",
					    CHROMA_MAX_BUSY_RETRIES);
			busy_retry_count = 0;
		}
	} else {
		busy_retry_count = 0;
	}

	if (!retry_pending) {
		bool needs_periodic = (target != NULL) || need_gamma || has_owned;

		raw_spin_lock_irqsave(&discovery_lock, flags);
		if (!paused && !list_empty(&gpus) && needs_periodic && !timer_pending(&recheck_timer))
			mod_timer(&recheck_timer, jiffies + CHROMA_RECHECK);
		else if (!needs_periodic)
			timer_delete(&recheck_timer);
		raw_spin_unlock_irqrestore(&discovery_lock, flags);
	}
}

struct kprobe chroma_kp = {
	.pre_handler = chroma_observe,
};

void chroma_pause(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&discovery_lock, flags);
	paused = true;
	raw_spin_unlock_irqrestore(&discovery_lock, flags);
	timer_delete_sync(&recheck_timer);
	flush_work(&refresh_work);
}

static int chroma_pm_event(struct notifier_block *notifier, unsigned long event, void *unused)
{
	unsigned long flags;

	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		chroma_pause();
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		raw_spin_lock_irqsave(&discovery_lock, flags);
		paused = false;
		raw_spin_unlock_irqrestore(&discovery_lock, flags);
		chroma_schedule();
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block chroma_pm_notifier = {
	.notifier_call = chroma_pm_event,
};

static void chroma_stop(void)
{
	struct chroma_gpu *gpu, *next;
	unsigned int i;

	unregister_kprobe(&chroma_kp);
	irq_work_sync(&discovery_irq);
	unregister_pm_notifier(&chroma_pm_notifier);
	chroma_pause();
	list_for_each_entry_safe(gpu, next, &gpus, link) {
		int ret = chroma_update_gpu(gpu, NULL, 100, 0, false, NULL);

		if (ret && ret != -ENODEV)
			pr_warn("chroma_amd: %s: display color restoration at unload failed: %pe\n",
				dev_name(gpu->dev->dev), ERR_PTR(ret));
		chroma_forget_gpu(gpu);
	}
	for (i = 0; i < ARRAY_SIZE(pending); ++i) {
		struct drm_device *dev = pending[i];

		if (dev)
			chroma_put_gpu(dev, dev->driver->fops->owner);
	}
	destroy_workqueue(chroma_wq);
}

static int __init chroma_init(void)
{
	int ret;

	chroma_wq = alloc_ordered_workqueue("chroma_amd", WQ_FREEZABLE);
	if (!chroma_wq)
		return -ENOMEM;
	INIT_WORK(&refresh_work, chroma_refresh);
	ret = register_pm_notifier(&chroma_pm_notifier);
	if (ret)
		goto destroy_queue;
	/* A symbol reference lets modprobe load drm_kms_helper when it is modular. */
	chroma_kp.addr = dereference_symbol_descriptor(drm_atomic_helper_commit);
	ret = register_kprobe(&chroma_kp);
	if (ret) {
		pr_err("chroma_amd: could not register the DRM discovery probe: %pe\n",
		       ERR_PTR(ret));
		goto remove_notifier;
	}

	ret = chroma_iface_init();
	if (ret)
		goto stop;

	chroma_schedule();
	return 0;

stop:
	chroma_stop();
	return ret;
remove_notifier:
	unregister_pm_notifier(&chroma_pm_notifier);
	chroma_pause();
destroy_queue:
	destroy_workqueue(chroma_wq);
	return ret;
}

static void __exit chroma_exit(void)
{
	chroma_iface_exit();
	chroma_stop();
}

module_init(chroma_init);
module_exit(chroma_exit);
