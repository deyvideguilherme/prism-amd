// SPDX-License-Identifier: GPL-2.0
#include "chroma_internal.h"

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

int chroma_update_gpu(struct chroma_gpu *gpu, const struct drm_color_ctm *target,
		      bool *had_ebusy)
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
				if (err == -EBUSY) {
					if (had_ebusy)
						*had_ebusy = true;
					if (!ret)
						ret = -EBUSY;
				} else {
					ret = err;
					pr_warn_ratelimited("chroma_amd: %s CRTC %u: CTM commit failed: %pe\n",
							    dev_name(dev->dev), crtc->base.id, ERR_PTR(err));
				}
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

void chroma_put_gpu(struct drm_device *dev, struct module *owner)
{
	drm_dev_put(dev);
	module_put(owner);
}

void chroma_forget_gpu(struct chroma_gpu *gpu)
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

enum chroma_enqueue_result chroma_try_enqueue_locked(struct drm_device *dev)
{
	struct chroma_gpu *gpu;
	unsigned int i, slot = ARRAY_SIZE(pending);

	if (!dev || !dev->driver || !dev->driver->fops)
		return CHROMA_ENQUEUE_INVALID;

	list_for_each_entry(gpu, &gpus, link) {
		if (gpu->dev == dev)
			return CHROMA_ENQUEUE_ALREADY_TRACKED;
	}
	for (i = 0; i < ARRAY_SIZE(pending); ++i) {
		if (pending[i] == dev)
			return CHROMA_ENQUEUE_ALREADY_TRACKED;
		if (!pending[i] && slot == ARRAY_SIZE(pending))
			slot = i;
	}
	if (slot == ARRAY_SIZE(pending))
		return CHROMA_ENQUEUE_FULL;

	if (!try_module_get(dev->driver->fops->owner))
		return CHROMA_ENQUEUE_MODULE_FAIL;

	drm_dev_get(dev);
	pending[slot] = dev;
	return CHROMA_ENQUEUE_OK;
}

void chroma_collect_gpus(void)
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

void chroma_scan_pci(void)
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
				enum chroma_enqueue_result enq_status;

				raw_spin_lock_irqsave(&discovery_lock, flags);
				enq_status = chroma_try_enqueue_locked(dev);
				raw_spin_unlock_irqrestore(&discovery_lock, flags);
				if (enq_status == CHROMA_ENQUEUE_OK) {
					enqueued = true;
					pr_debug("chroma_amd: proactive PCI scan enqueued %s (%s)\n",
						 dev_name(dev->dev), pci_name(pdev));
				} else if (enq_status == CHROMA_ENQUEUE_FULL) {
					pr_debug("chroma_amd: proactive PCI scan: pending slots full, cannot enqueue %s\n",
						 dev_name(dev->dev));
				} else if (enq_status == CHROMA_ENQUEUE_MODULE_FAIL) {
					pr_debug("chroma_amd: proactive PCI scan: try_module_get failed for %s owner\n",
						 dev_name(dev->dev));
				}
			}
		}
		device_unlock(&pdev->dev);
	}

	if (enqueued)
		chroma_collect_gpus();
}

/* Retain only the device. The commit caller already owns a DRM reference,
 * so drm_dev_get() only increments the refcount in the probe.
 */
int chroma_observe(struct kprobe *probe, struct pt_regs *regs)
{
	struct drm_device *dev = (void *)regs_get_kernel_argument(regs, 0);
	unsigned long flags;
	bool discovered = false;

	if (!dev || !dev->driver || !dev->driver->name || strcmp(dev->driver->name, "amdgpu") ||
	    !READ_ONCE(dev->registered) || !dev->mode_config.ctm_property)
		return 0;

	/* An INT3 probe executes in atomic/non-preemptible context. Defer on contention. */
	if (!raw_spin_trylock_irqsave(&discovery_lock, flags))
		return 0;
	switch (chroma_try_enqueue_locked(dev)) {
	case CHROMA_ENQUEUE_OK:
		discovered = true;
		break;
	case CHROMA_ENQUEUE_FULL:
		kprobe_drops_full++;
		break;
	case CHROMA_ENQUEUE_MODULE_FAIL:
		kprobe_drops_module++;
		break;
	default:
		break;
	}
	raw_spin_unlock_irqrestore(&discovery_lock, flags);

	if (discovered)
		irq_work_queue(&discovery_irq);
	return 0;
}
NOKPROBE_SYMBOL(chroma_observe);
