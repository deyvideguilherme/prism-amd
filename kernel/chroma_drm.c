// SPDX-License-Identifier: GPL-2.0
#include "chroma_internal.h"

static bool chroma_has_property(struct drm_crtc *crtc, struct drm_property *prop)
{
	struct drm_object_properties *props = &crtc->properties;
	int i;

	if (!prop)
		return false;

	for (i = 0; i < props->count; ++i) {
		if (props->properties[i] == prop)
			return true;
	}
	return false;
}

static size_t chroma_get_gamma_lut_size(struct drm_crtc *crtc)
{
	struct drm_property *size_prop = crtc->dev->mode_config.gamma_lut_size_property;
	uint64_t val = 0;

	if (size_prop && drm_object_property_get_value(&crtc->base, size_prop, &val) == 0 && val >= 2)
		return (size_t)val;

	if (crtc->gamma_size >= 2)
		return crtc->gamma_size;

	return 0;
}

static int chroma_update_crtc(struct drm_crtc *crtc,
			      struct chroma_crtc_owned *owned,
			      const struct drm_color_ctm *target_ctm,
			      int con_val, int bri_val, bool need_gamma)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_property_blob *candidate_ctm = NULL;
	struct drm_property_blob *candidate_gamma = NULL;
	struct drm_color_lut *lut_buf = NULL;
	struct drm_crtc_state *new_state;
	struct chroma_atomic_state *state;
	bool has_ctm = chroma_has_property(crtc, crtc->dev->mode_config.ctm_property);
	bool has_gamma = chroma_has_property(crtc, crtc->dev->mode_config.gamma_lut_property);
	size_t lut_size = has_gamma ? chroma_get_gamma_lut_size(crtc) : 0;
	bool allow_modeset = false;
	int ret = 0;

	if (lut_size < 2)
		has_gamma = false;

	if (!has_ctm && !has_gamma)
		return 0;

	if (has_gamma && need_gamma) {
		lut_buf = kvmalloc_array(lut_size, sizeof(struct drm_color_lut), GFP_KERNEL);
		if (!lut_buf)
			return -ENOMEM;
		chroma_compute_gamma_lut(lut_buf, lut_size, con_val, bri_val);
	}

	drm_modeset_acquire_init(&ctx, 0);
	for (;;) {
		bool apply_ctm = false;
		bool ctm_needs_change = false;
		bool apply_gamma = false;
		bool gamma_needs_change = false;
		bool is_transition = false;

		ret = drm_modeset_lock(&crtc->mutex, &ctx);
		if (ret)
			goto retry;

		/* Ownership belongs to the exact blob installed by a successful commit. */
		if (owned->ctm && crtc->state->ctm != owned->ctm)
			drm_property_replace_blob(&owned->ctm, NULL);
		if (owned->gamma_lut && crtc->state->gamma_lut != owned->gamma_lut)
			drm_property_replace_blob(&owned->gamma_lut, NULL);

		if (!crtc->state->active) {
			pr_debug("chroma_amd: %s CRTC %u is not active, skipping\n",
				 dev_name(crtc->dev->dev), crtc->base.id);
			break;
		}

		/* Evaluate CTM (Saturation) */
		if (has_ctm) {
			if (target_ctm) {
				if (crtc->state->ctm && !owned->ctm) {
					if (crtc->state->ctm->length < sizeof(struct drm_color_ctm)) {
						pr_debug("chroma_amd: %s CRTC %u malformed external CTM blob, skipping CTM\n",
							 dev_name(crtc->dev->dev), crtc->base.id);
					} else {
						const struct drm_color_ctm *ext = crtc->state->ctm->data;

						if (chroma_is_identity_ctm(ext)) {
							pr_debug("chroma_amd: %s CRTC %u adopting identity external CTM\n",
								 dev_name(crtc->dev->dev), crtc->base.id);
							apply_ctm = true;
						} else {
							pr_debug("chroma_amd: %s CRTC %u active non-identity external CTM, yielding CTM\n",
								 dev_name(crtc->dev->dev), crtc->base.id);
						}
					}
				} else {
					apply_ctm = true;
				}

				if (apply_ctm) {
					if (!owned->ctm ||
					    memcmp(owned->ctm->data, target_ctm, sizeof(*target_ctm)) != 0)
						ctm_needs_change = true;
				}
			} else {
				if (owned->ctm)
					ctm_needs_change = true;
			}
		}

		/* Evaluate GAMMA_LUT (Contrast & Brightness) */
		if (has_gamma) {
			if (need_gamma && lut_buf) {
				if (crtc->state->gamma_lut && !owned->gamma_lut) {
					size_t lut_bytes = sizeof(struct drm_color_lut);
					size_t ext_len = crtc->state->gamma_lut->length;

					if (ext_len < 2 * lut_bytes || (ext_len % lut_bytes) != 0) {
						pr_debug("chroma_amd: %s CRTC %u malformed external GAMMA_LUT blob (length %zu), skipping GAMMA_LUT\n",
							 dev_name(crtc->dev->dev), crtc->base.id, ext_len);
					} else {
						size_t count = ext_len / lut_bytes;
						const struct drm_color_lut *ext = crtc->state->gamma_lut->data;

						if (chroma_is_identity_gamma_lut(ext, count)) {
							pr_debug("chroma_amd: %s CRTC %u adopting identity external GAMMA_LUT\n",
								 dev_name(crtc->dev->dev), crtc->base.id);
							apply_gamma = true;
						} else {
							pr_debug("chroma_amd: %s CRTC %u active non-identity external GAMMA_LUT, yielding GAMMA_LUT\n",
								 dev_name(crtc->dev->dev), crtc->base.id);
						}
					}
				} else {
					apply_gamma = true;
				}

				if (apply_gamma) {
					size_t exp_len = lut_size * sizeof(struct drm_color_lut);

					if (!owned->gamma_lut ||
					    owned->gamma_lut->length != exp_len ||
					    memcmp(owned->gamma_lut->data, lut_buf, exp_len) != 0)
						gamma_needs_change = true;
				}
			} else {
				if (owned->gamma_lut)
					gamma_needs_change = true;
			}
		}

		if (!ctm_needs_change && !gamma_needs_change)
			break;

		if (ctm_needs_change && (!owned->ctm || !apply_ctm))
			is_transition = true;
		if (gamma_needs_change && (!owned->gamma_lut || !apply_gamma))
			is_transition = true;

commit_state:
		state = chroma_atomic_alloc(crtc->dev);
		if (!state) {
			ret = -ENOMEM;
			break;
		}
		state->acquire_ctx = &ctx;
		state->allow_modeset = allow_modeset;
		candidate_ctm = NULL;
		candidate_gamma = NULL;

		new_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(new_state)) {
			ret = PTR_ERR(new_state);
			goto put_state;
		}

		if (ctm_needs_change) {
			if (apply_ctm) {
				candidate_ctm = drm_property_create_blob(crtc->dev, sizeof(*target_ctm), target_ctm);
				if (IS_ERR(candidate_ctm)) {
					ret = PTR_ERR(candidate_ctm);
					candidate_ctm = NULL;
					goto put_state;
				}
			}
			drm_property_replace_blob(&new_state->ctm, candidate_ctm);
		}

		if (gamma_needs_change) {
			if (apply_gamma) {
				candidate_gamma = drm_property_create_blob(crtc->dev,
									   lut_size * sizeof(struct drm_color_lut),
									   lut_buf);
				if (IS_ERR(candidate_gamma)) {
					ret = PTR_ERR(candidate_gamma);
					candidate_gamma = NULL;
					goto put_state;
				}
			}
			drm_property_replace_blob(&new_state->gamma_lut, candidate_gamma);
		}

		new_state->color_mgmt_changed = true;
		ret = drm_atomic_commit(state);
		if (!ret) {
			if (ctm_needs_change)
				drm_property_replace_blob(&owned->ctm, candidate_ctm);
			if (gamma_needs_change)
				drm_property_replace_blob(&owned->gamma_lut, candidate_gamma);

			pr_debug("chroma_amd: %s CRTC %u commit ok (ctm_blob=%u gamma_blob=%u allow_modeset=%d)\n",
				 dev_name(crtc->dev->dev), crtc->base.id,
				 candidate_ctm ? candidate_ctm->base.id : 0,
				 candidate_gamma ? candidate_gamma->base.id : 0,
				 allow_modeset);
		} else if (ret == -EINVAL && !allow_modeset && is_transition) {
			pr_debug("chroma_amd: %s CRTC %u fast commit failed (-EINVAL), retrying with allow_modeset=true\n",
				 dev_name(crtc->dev->dev), crtc->base.id);
			if (candidate_ctm) {
				drm_property_blob_put(candidate_ctm);
				candidate_ctm = NULL;
			}
			if (candidate_gamma) {
				drm_property_blob_put(candidate_gamma);
				candidate_gamma = NULL;
			}
			chroma_atomic_put(state);
			allow_modeset = true;
			goto commit_state;
		} else if (ret == -EBUSY) {
			pr_debug("chroma_amd: %s CRTC %u atomic commit returned -EBUSY (flip in flight)\n",
				 dev_name(crtc->dev->dev), crtc->base.id);
		}

		if (candidate_ctm)
			drm_property_blob_put(candidate_ctm);
		if (candidate_gamma)
			drm_property_blob_put(candidate_gamma);

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
	kvfree(lut_buf);
	return ret;
}

int chroma_update_gpu(struct chroma_gpu *gpu,
		      const struct drm_color_ctm *target_ctm,
		      int con_val, int bri_val, bool need_gamma,
		      bool *had_ebusy)
{
	struct drm_device *dev = gpu->dev;
	struct drm_crtc *crtc;
	unsigned int i;
	int idx, pm, ret = 0;

	if (!target_ctm && !need_gamma) {
		for (i = 0; i < gpu->num_crtcs; ++i) {
			if (gpu->owned[i].ctm || gpu->owned[i].gamma_lut)
				break;
		}
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
			struct chroma_crtc_owned *owned;
			int err;

			i = drm_crtc_index(crtc);
			if (i >= gpu->num_crtcs)
				continue;
			owned = &gpu->owned[i];
			if (!target_ctm && !need_gamma && !owned->ctm && !owned->gamma_lut)
				continue;
			err = chroma_update_crtc(crtc, owned, target_ctm, con_val, bri_val, need_gamma);
			if (err) {
				if (err == -EBUSY) {
					if (had_ebusy)
						*had_ebusy = true;
					if (!ret)
						ret = -EBUSY;
				} else {
					ret = err;
					pr_warn_ratelimited("chroma_amd: %s CRTC %u: color commit failed: %pe\n",
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

	for (i = 0; i < gpu->num_crtcs; ++i) {
		if (gpu->owned[i].ctm)
			drm_property_blob_put(gpu->owned[i].ctm);
		if (gpu->owned[i].gamma_lut)
			drm_property_blob_put(gpu->owned[i].gamma_lut);
	}
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
			if (dev && READ_ONCE(dev->registered) &&
			    (dev->mode_config.ctm_property || dev->mode_config.gamma_lut_property)) {
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
	    !READ_ONCE(dev->registered) ||
	    (!dev->mode_config.ctm_property && !dev->mode_config.gamma_lut_property))
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
