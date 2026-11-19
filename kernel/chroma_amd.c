// SPDX-License-Identifier: GPL-2.0

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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("prism-amd Contributors");
MODULE_DESCRIPTION("Atomic DRM/KMS hardware color calibration for AMDGPU");
MODULE_VERSION(PRISMA_VERSION);

#ifdef PRISMA_DRM_ATOMIC_COMMIT
#define chroma_atomic_state drm_atomic_commit
#define chroma_atomic_alloc drm_atomic_commit_alloc
#define chroma_atomic_put drm_atomic_commit_put
#else
#define chroma_atomic_state drm_atomic_state
#define chroma_atomic_alloc drm_atomic_state_alloc
#define chroma_atomic_put drm_atomic_state_put
#endif

/* Rec.709 integer luminance weights scaled by 10000 */
#define REC709_RW 2126LL
#define REC709_GW 7152LL
#define REC709_BW 722LL

static u64 to_s31_32(s64 num, u64 den)
{
	u64 sign = 0;
	u64 abs_num;
	u64 mag;

	if (num < 0) {
		sign = 1ULL << 63;
		abs_num = (u64)(-num);
	} else {
		abs_num = (u64)num;
	}

	/* Scale magnitude by 2^32 with safe intermediate 128-bit division (no FPU) */
	mag = mul_u64_u64_div_u64(abs_num, 1ULL << 32, den);

	/* Safe clamp to S31.32 maximum magnitude (bits 0..62) */
	if (mag > ((1ULL << 63) - 1))
		mag = (1ULL << 63) - 1;

	return sign | mag;
}

/*
 * Computes the 3x3 Rec.709 CTM matrix incorporating saturation, contrast,
 * and linear brightness gain using strictly 64-bit integer arithmetic.
 *
 * Brightness (-100 to 100) is applied as a proportional linear gain factor:
 * - brightness = -100 -> gain_q16 = 16384  (0.25x)
 * - brightness =    0 -> gain_q16 = 65536  (1.00x)
 * - brightness = +100 -> gain_q16 = 114688 (1.75x)
 * Formula: gain_q16 = 65536 + div_s64(brightness * 49152, 100)
 */
static void chroma_compute_ctm(struct drm_color_ctm *ctm, int sat_val, int con_val, int bri_val)
{
	s64 sat = (s64)sat_val;
	s64 contrast = (s64)con_val;
	s64 brightness = (s64)bri_val;
	s64 one_minus_s = 100LL - sat;
	s64 weights[3] = { REC709_RW, REC709_GW, REC709_BW };
	s64 gain_q16;
	s64 den = 100000000LL * 65536LL; /* 100 (sat) * 10000 (luma) * 100 (contrast) * 65536 (gain_q16) */
	int r, c;

	gain_q16 = 65536LL + div_s64(brightness * 49152LL, 100LL);
	if (gain_q16 < 0)
		gain_q16 = 0;

	for (r = 0; r < 3; ++r) {
		for (c = 0; c < 3; ++c) {
			s64 w = weights[c];
			s64 val;

			if (r == c)
				val = (one_minus_s * w + sat * 10000LL) * contrast;
			else
				val = (one_minus_s * w) * contrast;

			val = val * gain_q16;

			ctm->matrix[r * 3 + c] = to_s31_32(val, den);
		}
	}
}

static DEFINE_MUTEX(config_lock);
static struct prisma_ioctl_state config = {
	.abi_version = PRISMA_ABI_VERSION,
	.enabled = 1,
	.saturation = 100,
	.contrast = 100,
};

static struct prisma_ioctl_state chroma_snapshot(void)
{
	struct prisma_ioctl_state state;

	mutex_lock(&config_lock);
	state = config;
	mutex_unlock(&config_lock);
	return state;
}

struct chroma_gpu {
	struct list_head link;
	struct drm_device *dev;
	struct module *owner;
	unsigned int num_crtcs;
	struct drm_property_blob *owned[];
};

/* Only simultaneous discoveries are bounded. Tracked devices and CRTCs are dynamic. */
#define CHROMA_PENDING_GPUS 32
#define CHROMA_RECHECK (2 * HZ)

static DEFINE_RAW_SPINLOCK(discovery_lock);
static LIST_HEAD(gpus);
static struct drm_device *pending[CHROMA_PENDING_GPUS];
static bool paused;
static bool refresh_queued;
static struct workqueue_struct *chroma_wq;
static struct work_struct refresh_work;

static void chroma_schedule(void)
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

static DEFINE_IRQ_WORK(discovery_irq, chroma_discovery_ready);

static void chroma_recheck(struct timer_list *timer)
{
	chroma_schedule();
}

static DEFINE_TIMER(recheck_timer, chroma_recheck);

static bool chroma_has_ctm(struct drm_crtc *crtc)
{
	struct drm_property *ctm = crtc->dev->mode_config.ctm_property;
	struct drm_object_properties *props = &crtc->properties;
	int i;

	for (i = 0; i < props->count; ++i) {
		if (props->properties[i] == ctm)
			return true;
	}
	return false;
}

static bool chroma_is_identity_ctm(const struct drm_color_ctm *ctm)
{
	int i;

	if (!ctm)
		return true;

	for (i = 0; i < 9; ++i) {
		u64 expected = (i == 0 || i == 4 || i == 8) ? (1ULL << 32) : 0ULL;

		if (ctm->matrix[i] != expected)
			return false;
	}
	return true;
}

static int chroma_update_crtc(struct drm_crtc *crtc,
			      struct drm_property_blob **owned,
			      const struct drm_color_ctm *target)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_property_blob *candidate;
	struct drm_crtc_state *new_state;
	struct chroma_atomic_state *state;
	bool allow_modeset = false;
	bool is_transition;
	int ret;

	drm_modeset_acquire_init(&ctx, 0);
	for (;;) {
		ret = drm_modeset_lock(&crtc->mutex, &ctx);
		if (ret)
			goto retry;

		/* Ownership belongs to the exact blob installed by a successful commit. */
		if (*owned && crtc->state->ctm != *owned)
			drm_property_replace_blob(owned, NULL);
		if (target) {
			if (!crtc->state->active) {
				pr_debug("chroma_amd: %s CRTC %u is not active, skipping\n",
					 dev_name(crtc->dev->dev), crtc->base.id);
				break;
			}
			if (crtc->state->ctm && !*owned) {
				const struct drm_color_ctm *ext_ctm;

				if (crtc->state->ctm->length < sizeof(struct drm_color_ctm)) {
					pr_debug("chroma_amd: %s CRTC %u has malformed external CTM blob (length %zu), skipping\n",
						 dev_name(crtc->dev->dev), crtc->base.id,
						 crtc->state->ctm->length);
					break;
				}
				ext_ctm = crtc->state->ctm->data;
				if (!chroma_is_identity_ctm(ext_ctm)) {
					pr_debug("chroma_amd: %s CRTC %u has active non-identity external CTM, skipping\n",
						 dev_name(crtc->dev->dev), crtc->base.id);
					break;
				}
				pr_debug("chroma_amd: %s CRTC %u has identity CTM, adopting CRTC\n",
					 dev_name(crtc->dev->dev), crtc->base.id);
			}
			if (*owned && !memcmp((*owned)->data, target, sizeof(*target)))
				break;
		} else if (!*owned) {
			break;
		}

		is_transition = (!*owned || !target);

commit_state:
		state = chroma_atomic_alloc(crtc->dev);
		if (!state) {
			ret = -ENOMEM;
			break;
		}
		state->acquire_ctx = &ctx;
		state->allow_modeset = allow_modeset;
		candidate = NULL;
		new_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(new_state)) {
			ret = PTR_ERR(new_state);
			goto put_state;
		}
		if (target) {
			candidate = drm_property_create_blob(crtc->dev, sizeof(*target), target);
			if (IS_ERR(candidate)) {
				ret = PTR_ERR(candidate);
				goto put_state;
			}
		}
		drm_property_replace_blob(&new_state->ctm, candidate);
		new_state->color_mgmt_changed = true;
		ret = drm_atomic_commit(state);
		if (!ret) {
			drm_property_replace_blob(owned, candidate);
			if (allow_modeset) {
				pr_debug("chroma_amd: %s CRTC %u CTM blob %u (fallback allow_modeset=true confirmed)\n",
					 dev_name(crtc->dev->dev), crtc->base.id,
					 candidate ? candidate->base.id : 0);
			} else {
				pr_debug("chroma_amd: %s CRTC %u CTM blob %u\n",
					 dev_name(crtc->dev->dev), crtc->base.id,
					 candidate ? candidate->base.id : 0);
			}
		} else if (ret == -EINVAL && !allow_modeset && is_transition) {
			pr_debug("chroma_amd: %s CRTC %u fast commit failed (-EINVAL), retrying with allow_modeset=true\n",
				 dev_name(crtc->dev->dev), crtc->base.id);
			drm_property_blob_put(candidate);
			chroma_atomic_put(state);
			allow_modeset = true;
			goto commit_state;
		} else if (ret == -EBUSY) {
			pr_debug("chroma_amd: %s CRTC %u atomic commit returned -EBUSY (flip in flight)\n",
				 dev_name(crtc->dev->dev), crtc->base.id);
		}
		drm_property_blob_put(candidate);
put_state:
		chroma_atomic_put(state);
retry:
		if (ret != -EDEADLK)
			break;
		ret = drm_modeset_backoff(&ctx);
		if (ret)
			break;
	}
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
}

/* The DRM reference keeps the mode-config storage alive. SRCU guards hardware
 * and driver state until amdgpu_pci_remove's drm_dev_unplug() can proceed.
 */
static int chroma_update_gpu(struct chroma_gpu *gpu, const struct drm_color_ctm *target)
{
	struct drm_device *dev = gpu->dev;
	struct drm_crtc *crtc;
	unsigned int i;
	int idx, pm, ret = 0;

	if (!target) {
		for (i = 0; i < gpu->num_crtcs && !gpu->owned[i]; ++i)
			;
		if (i == gpu->num_crtcs) {
			pr_debug("chroma_amd: %s: no owned CRTCs to reset\n", dev_name(dev->dev));
			return 0;
		}
	}
	if (!drm_dev_enter(dev, &idx)) {
		pr_debug("chroma_amd: %s: drm_dev_enter failed (device unplugged?)\n", dev_name(dev->dev));
		return -ENODEV;
	}
	pm = pm_runtime_get_if_active(dev->dev);
	if (pm != 0) {
		drm_for_each_crtc(crtc, dev) {
			struct drm_property_blob **owned;
			int err;

			i = drm_crtc_index(crtc);
			if (i >= gpu->num_crtcs)
				continue;
			owned = &gpu->owned[i];
			if (!chroma_has_ctm(crtc)) {
				pr_debug("chroma_amd: %s CRTC %u: no CTM property\n",
					 dev_name(dev->dev), crtc->base.id);
				continue;
			}
			if (!target && !*owned)
				continue;
			err = chroma_update_crtc(crtc, owned, target);
			if (err) {
				ret = err;
				if (err != -EBUSY)
					pr_warn_ratelimited("chroma_amd: %s CRTC %u: CTM commit failed: %pe\n",
							    dev_name(dev->dev), crtc->base.id, ERR_PTR(err));
			}
		}
		if (pm > 0)
			__pm_runtime_put_autosuspend(dev->dev);
	} else {
		pr_debug("chroma_amd: %s: pm_runtime_get_if_active returned 0 (device suspended/idle)\n",
			 dev_name(dev->dev));
		ret = -EAGAIN;
	}
	drm_dev_exit(idx);
	return ret;
}

static void chroma_put_gpu(struct drm_device *dev, struct module *owner)
{
	drm_dev_put(dev);
	module_put(owner);
}

static void chroma_forget_gpu(struct chroma_gpu *gpu)
{
	unsigned long flags;
	unsigned int i;

	raw_spin_lock_irqsave(&discovery_lock, flags);
	list_del(&gpu->link);
	raw_spin_unlock_irqrestore(&discovery_lock, flags);

	for (i = 0; i < gpu->num_crtcs; ++i)
		drm_property_blob_put(gpu->owned[i]);
	chroma_put_gpu(gpu->dev, gpu->owner);
	kfree(gpu);
}

static bool chroma_try_enqueue_locked(struct drm_device *dev)
{
	struct chroma_gpu *gpu;
	unsigned int i, slot = ARRAY_SIZE(pending);

	list_for_each_entry(gpu, &gpus, link) {
		if (gpu->dev == dev)
			return false;
	}
	for (i = 0; i < ARRAY_SIZE(pending); ++i) {
		if (pending[i] == dev)
			return false;
		if (!pending[i] && slot == ARRAY_SIZE(pending))
			slot = i;
	}
	if (slot == ARRAY_SIZE(pending)) {
		pr_debug("chroma_amd: pending slots full, cannot enqueue %s\n", dev_name(dev->dev));
		return false;
	}
	if (!try_module_get(dev->driver->fops->owner)) {
		pr_debug("chroma_amd: try_module_get failed for %s owner\n", dev_name(dev->dev));
		return false;
	}
	drm_dev_get(dev);
	pending[slot] = dev;
	return true;
}

static void chroma_collect_gpus(void)
{
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pending); ++i) {
		struct drm_device *dev;
		struct chroma_gpu *gpu;
		struct module *owner;
		unsigned int num_crtcs;

		raw_spin_lock_irqsave(&discovery_lock, flags);
		dev = pending[i];
		raw_spin_unlock_irqrestore(&discovery_lock, flags);
		if (!dev)
			continue;

		owner = dev->driver->fops->owner;
		num_crtcs = dev->mode_config.num_crtc;
		gpu = kzalloc(struct_size(gpu, owned, num_crtcs), GFP_KERNEL);
		if (gpu) {
			gpu->dev = dev;
			gpu->owner = owner;
			gpu->num_crtcs = num_crtcs;
		} else {
			pr_warn_ratelimited("chroma_amd: could not track %s\n", dev_name(dev->dev));
		}

		raw_spin_lock_irqsave(&discovery_lock, flags);
		pending[i] = NULL;
		if (gpu)
			list_add_tail(&gpu->link, &gpus);
		raw_spin_unlock_irqrestore(&discovery_lock, flags);

		if (!gpu)
			chroma_put_gpu(dev, owner);
	}
}

static void chroma_scan_pci(void)
{
	struct pci_dev *pdev = NULL;
	bool enqueued = false;

	while ((pdev = pci_get_device(PCI_VENDOR_ID_ATI, PCI_ANY_ID, pdev)) != NULL) {
		struct drm_device *dev = NULL;
		unsigned long flags;

		if ((pdev->class >> 16) != PCI_BASE_CLASS_DISPLAY)
			continue;

		device_lock(&pdev->dev);
		if (pdev->driver && pdev->driver->name && !strcmp(pdev->driver->name, "amdgpu")) {
			dev = pci_get_drvdata(pdev);
			if (dev && READ_ONCE(dev->registered) && dev->mode_config.ctm_property) {
				raw_spin_lock_irqsave(&discovery_lock, flags);
				if (chroma_try_enqueue_locked(dev)) {
					enqueued = true;
					pr_debug("chroma_amd: proactive PCI scan enqueued %s (%s)\n",
						 dev_name(dev->dev), pci_name(pdev));
				}
				raw_spin_unlock_irqrestore(&discovery_lock, flags);
			}
		}
		device_unlock(&pdev->dev);
	}

	if (enqueued)
		chroma_collect_gpus();
}

#define CHROMA_MAX_BUSY_RETRIES 5
static unsigned int busy_retry_count;

static void chroma_refresh(struct work_struct *work)
{
	struct prisma_ioctl_state snapshot;
	struct chroma_gpu *gpu, *next, *gpu_count;
	struct drm_color_ctm ctm;
	struct drm_color_ctm *target = NULL;
	unsigned int num_gpus = 0, num_pending = 0;
	unsigned int p_idx;
	unsigned long flags;
	bool had_ebusy = false;

	raw_spin_lock_irqsave(&discovery_lock, flags);
	refresh_queued = false;
	if (paused) {
		raw_spin_unlock_irqrestore(&discovery_lock, flags);
		return;
	}
	list_for_each_entry(gpu_count, &gpus, link)
		num_gpus++;
	for (p_idx = 0; p_idx < ARRAY_SIZE(pending); ++p_idx) {
		if (pending[p_idx])
			num_pending++;
	}
	raw_spin_unlock_irqrestore(&discovery_lock, flags);

	pr_debug("chroma_amd: refresh: %u tracked GPUs, %u pending\n", num_gpus, num_pending);

	if (list_empty(&gpus)) {
		pr_debug("chroma_amd: no tracked GPUs, initiating proactive PCI scan\n");
		chroma_scan_pci();
	}

	snapshot = chroma_snapshot();
	if (snapshot.enabled &&
	    (snapshot.saturation != 100 || snapshot.contrast != 100 || snapshot.brightness)) {
		chroma_compute_ctm(&ctm, snapshot.saturation, snapshot.contrast, snapshot.brightness);
		target = &ctm;
	}
	chroma_collect_gpus();
	list_for_each_entry_safe(gpu, next, &gpus, link) {
		int err = chroma_update_gpu(gpu, target);

		if (err == -EBUSY)
			had_ebusy = true;
		if (drm_dev_is_unplugged(gpu->dev))
			chroma_forget_gpu(gpu);
	}

	if (had_ebusy) {
		if (busy_retry_count < CHROMA_MAX_BUSY_RETRIES) {
			busy_retry_count++;
			pr_debug("chroma_amd: rescheduling refresh due to -EBUSY (retry %u/%u)\n",
				 busy_retry_count, CHROMA_MAX_BUSY_RETRIES);
			raw_spin_lock_irqsave(&discovery_lock, flags);
			if (!paused && !refresh_queued) {
				refresh_queued = true;
				queue_work(chroma_wq, &refresh_work);
			}
			raw_spin_unlock_irqrestore(&discovery_lock, flags);
		} else {
			pr_warn_ratelimited("chroma_amd: exceeded maximum -EBUSY retries (%u)\n",
					    CHROMA_MAX_BUSY_RETRIES);
			busy_retry_count = 0;
		}
	} else {
		busy_retry_count = 0;
	}

	raw_spin_lock_irqsave(&discovery_lock, flags);
	if (!paused && !list_empty(&gpus) && !timer_pending(&recheck_timer))
		mod_timer(&recheck_timer, jiffies + CHROMA_RECHECK);
	raw_spin_unlock_irqrestore(&discovery_lock, flags);
}

/* Retain only the device. The commit caller already owns a DRM reference,
 * so drm_dev_get() only increments the refcount in the probe.
 */
static int chroma_observe(struct kprobe *probe, struct pt_regs *regs)
{
	struct drm_device *dev = (void *)regs_get_kernel_argument(regs, 0);
	unsigned long flags;
	bool discovered = false;

	if (!dev) {
		pr_debug("chroma_amd: observe: dev is NULL\n");
		return 0;
	}
	if (!dev->driver || !dev->driver->name) {
		pr_debug("chroma_amd: observe: device has no driver/name\n");
		return 0;
	}
	if (strcmp(dev->driver->name, "amdgpu")) {
		pr_debug("chroma_amd: observe: driver '%s' is not amdgpu\n", dev->driver->name);
		return 0;
	}
	if (!READ_ONCE(dev->registered)) {
		pr_debug("chroma_amd: observe: %s is not registered\n", dev_name(dev->dev));
		return 0;
	}
	if (!dev->mode_config.ctm_property) {
		pr_debug("chroma_amd: observe: %s has no ctm_property\n", dev_name(dev->dev));
		return 0;
	}

	/* An INT3 probe can enter with NMI accounting. Defer on contention. */
	if (!raw_spin_trylock_irqsave(&discovery_lock, flags)) {
		pr_debug("chroma_amd: observe: discovery_lock contended\n");
		return 0;
	}
	discovered = chroma_try_enqueue_locked(dev);
	raw_spin_unlock_irqrestore(&discovery_lock, flags);

	if (discovered) {
		pr_debug("chroma_amd: observe: enqueued %s via kprobe\n", dev_name(dev->dev));
		irq_work_queue(&discovery_irq);
	}
	return 0;
}
NOKPROBE_SYMBOL(chroma_observe);

static struct kprobe chroma_kp = {
	.pre_handler = chroma_observe,
};

static void chroma_pause(void)
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
		int ret = chroma_update_gpu(gpu, NULL);

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

static int chroma_parse_config(char *input, struct prisma_ioctl_state *next)
{
	char *token, *key;
	int value;

	while ((token = strsep(&input, " \t\r\n"))) {
		if (!*token)
			continue;
		key = strsep(&token, "=");
		if (!token || kstrtoint(token, 10, &value))
			return -EINVAL;

		if (!strcmp(key, "saturation")) {
			if (value < 0 || value > 400)
				return -EINVAL;
			next->saturation = value;
		} else if (!strcmp(key, "contrast")) {
			if (value < 10 || value > 300)
				return -EINVAL;
			next->contrast = value;
		} else if (!strcmp(key, "brightness")) {
			if (value < -100 || value > 100)
				return -EINVAL;
			next->brightness = value;
		} else if (!strcmp(key, "enabled")) {
			if (value != 0 && value != 1)
				return -EINVAL;
			next->enabled = value;
		} else {
			return -EINVAL;
		}
	}
	return 0;
}

static ssize_t chroma_dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct prisma_ioctl_state state = chroma_snapshot();
	char msg[160];
	int len;

	len = scnprintf(msg, sizeof(msg),
			"abi_version=%u\nenabled=%d\nsaturation=%d\ncontrast=%d\nbrightness=%d\n",
			state.abi_version, state.enabled, state.saturation,
			state.contrast, state.brightness);
	return simple_read_from_buffer(buf, count, ppos, msg, len);
}

static ssize_t chroma_dev_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct prisma_ioctl_state next;
	char input[256];
	int ret;

	if (!count)
		return 0;
	if (count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count))
		return -EFAULT;
	if (memchr(input, '\0', count))
		return -EINVAL;
	input[count] = '\0';

	mutex_lock(&config_lock);
	next = config;
	ret = chroma_parse_config(input, &next);
	if (!ret)
		config = next;
	mutex_unlock(&config_lock);

	if (ret)
		return ret;
	chroma_schedule();
	return count;
}

static long chroma_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *buf = (void __user *)arg;
	struct prisma_ioctl_state state;
	u32 abi = PRISMA_ABI_VERSION;

	switch (cmd) {
	case PRISMA_IOC_GET_ABI:
		return copy_to_user(buf, &abi, sizeof(abi)) ? -EFAULT : 0;
	case PRISMA_IOC_GET_STATE:
		state = chroma_snapshot();
		return copy_to_user(buf, &state, sizeof(state)) ? -EFAULT : 0;
	case PRISMA_IOC_SET_STATE:
		if (copy_from_user(&state, buf, sizeof(state)))
			return -EFAULT;
		if (state.abi_version != PRISMA_ABI_VERSION)
			return -EPROTO;
		if (state.saturation < 0 || state.saturation > 400 ||
		    state.contrast < 10 || state.contrast > 300 ||
		    state.brightness < -100 || state.brightness > 100 ||
		    (state.enabled != 0 && state.enabled != 1))
			return -EINVAL;

		mutex_lock(&config_lock);
		config = state;
		mutex_unlock(&config_lock);
		chroma_schedule();
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations chroma_fops = {
	.owner = THIS_MODULE,
	.read = chroma_dev_read,
	.write = chroma_dev_write,
	.unlocked_ioctl = chroma_dev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = default_llseek,
};

static struct miscdevice chroma_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "chroma_amd",
	.fops = &chroma_fops,
	.mode = 0660,
};

#define CHROMA_ATTR(field, format) \
static ssize_t field##_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
{ \
	return sysfs_emit(buf, format "\n", chroma_snapshot().field); \
} \
static struct kobj_attribute field##_attr = __ATTR_RO(field)

CHROMA_ATTR(abi_version, "%u");
CHROMA_ATTR(saturation, "%d");
CHROMA_ATTR(contrast, "%d");
CHROMA_ATTR(brightness, "%d");
CHROMA_ATTR(enabled, "%d");

static struct attribute *chroma_attrs[] = {
	&abi_version_attr.attr,
	&saturation_attr.attr,
	&contrast_attr.attr,
	&brightness_attr.attr,
	&enabled_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(chroma);

static struct kobject *chroma_kobj;

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

	chroma_kobj = kobject_create_and_add("chroma_amd", kernel_kobj);
	if (!chroma_kobj) {
		ret = -ENOMEM;
		goto stop;
	}
	ret = sysfs_create_groups(chroma_kobj, chroma_groups);
	if (ret)
		goto put_kobj;
	ret = misc_register(&chroma_misc_dev);
	if (!ret) {
		chroma_schedule();
		return 0;
	}

	sysfs_remove_groups(chroma_kobj, chroma_groups);
put_kobj:
	kobject_put(chroma_kobj);
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
	misc_deregister(&chroma_misc_dev);
	sysfs_remove_groups(chroma_kobj, chroma_groups);
	kobject_put(chroma_kobj);
	chroma_stop();
}

module_init(chroma_init);
module_exit(chroma_exit);
