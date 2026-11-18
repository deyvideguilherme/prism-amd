// SPDX-License-Identifier: GPL-2.0
/*
 * chroma_amd.c - Linux Kernel Module for AMDGPU Hardware Color Acceleration (prism-amd)
 *
 * Injects Rec.709 Color Transformation Matrices (CTM) directly into the AMD Display
 * Core Next (DCN) hardware pipeline by intercepting DRM atomic commits via kprobe.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/version.h>
#include <linux/math64.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_property.h>
#include <drm/drm_mode.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("prism-amd Contributors");
MODULE_DESCRIPTION("Hardware Color Transformation Matrix (CTM) Interceptor for AMDGPU DRM/KMS");
MODULE_VERSION("1.0.0");

/*
 * Kernel compatibility:
 * In Linux 6.13+ / 7.x, struct drm_atomic_state was renamed to struct drm_atomic_commit.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#ifndef drm_atomic_state
#define drm_atomic_state drm_atomic_commit
#endif
#endif

/* Rec.709 integer luminance weights scaled by 10000 */
#define REC709_RW 2126LL
#define REC709_GW 7152LL
#define REC709_BW 722LL

#define MAX_CRTCS 32

/* Sysfs tuning parameters (/sys/kernel/chroma_amd/) */
static int g_saturation = 100; // 0 to 400 (default 100 = 100%)
static int g_contrast = 100;   // 10 to 300 (default 100 = 100%)
static int g_brightness = 0;   // -100 to 100 (default 0 = 0%)
static int g_enabled = 1;      // 0 or 1 (default 1)

static atomic_t config_generation = ATOMIC_INIT(1);
static int g_crtc_applied_gen[MAX_CRTCS];

static DEFINE_SPINLOCK(g_ctm_lock);
static struct drm_color_ctm g_current_ctm;

/*
 * Converts rational value (num / den) into 64-bit S31.32 sign-magnitude fixed-point
 * format required by Linux DRM kernel (struct drm_color_ctm):
 * - Bit 63: sign bit (1 = negative, 0 = positive)
 * - Bits 0..62: absolute magnitude scaled by 2^32
 */
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

	/* Scale magnitude by 2^32 with 128-bit safe intermediate division (no FPU) */
	mag = mul_u64_u64_div_u64(abs_num, 1ULL << 32, den);
	mag &= ((1ULL << 63) - 1);

	return sign | mag;
}

/*
 * Recalculates the 3x3 Rec.709 CTM matrix using strictly 64-bit integer arithmetic
 */
static void chroma_recalc_ctm(void)
{
	s64 sat = (s64)g_saturation;
	s64 contrast = (s64)g_contrast;
	s64 one_minus_s = 100LL - sat;
	s64 weights[3] = { REC709_RW, REC709_GW, REC709_BW };
	s64 den = 100000000LL; /* 100 (sat) * 10000 (luma) * 100 (contrast) */
	struct drm_color_ctm new_ctm;
	int r, c;
	unsigned long flags;

	for (r = 0; r < 3; ++r) {
		for (c = 0; c < 3; ++c) {
			s64 w = weights[c];
			s64 val;

			if (r == c)
				val = (one_minus_s * w + sat * 10000LL) * contrast;
			else
				val = (one_minus_s * w) * contrast;

			new_ctm.matrix[r * 3 + c] = to_s31_32(val, den);
		}
	}

	spin_lock_irqsave(&g_ctm_lock, flags);
	memcpy(&g_current_ctm, &new_ctm, sizeof(new_ctm));
	spin_unlock_irqrestore(&g_ctm_lock, flags);
}

/*
 * Sysfs Callbacks (/sys/kernel/chroma_amd/)
 */
static ssize_t saturation_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_saturation);
}

static ssize_t saturation_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;
	if (val < 0 || val > 400)
		return -EINVAL;

	g_saturation = val;
	chroma_recalc_ctm();
	atomic_inc(&config_generation);

	return count;
}

static struct kobj_attribute saturation_attr = __ATTR(saturation, 0664, saturation_show, saturation_store);

static ssize_t contrast_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_contrast);
}

static ssize_t contrast_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;
	if (val < 10 || val > 300)
		return -EINVAL;

	g_contrast = val;
	chroma_recalc_ctm();
	atomic_inc(&config_generation);

	return count;
}

static struct kobj_attribute contrast_attr = __ATTR(contrast, 0664, contrast_show, contrast_store);

static ssize_t brightness_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_brightness);
}

static ssize_t brightness_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;
	if (val < -100 || val > 100)
		return -EINVAL;

	g_brightness = val;
	chroma_recalc_ctm();
	atomic_inc(&config_generation);

	return count;
}

static struct kobj_attribute brightness_attr = __ATTR(brightness, 0664, brightness_show, brightness_store);

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_enabled);
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) < 0)
		return -EINVAL;
	if (val != 0 && val != 1)
		return -EINVAL;

	g_enabled = val;
	atomic_inc(&config_generation);

	return count;
}

static struct kobj_attribute enabled_attr = __ATTR(enabled, 0664, enabled_show, enabled_store);

static struct attribute *chroma_attrs[] = {
	&saturation_attr.attr,
	&contrast_attr.attr,
	&brightness_attr.attr,
	&enabled_attr.attr,
	NULL,
};

static const struct attribute_group chroma_attr_group = {
	.attrs = chroma_attrs,
};

static struct kobject *chroma_kobj;

/*
 * Kprobe handler intercepting drm_atomic_helper_commit
 */
static int chroma_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	/* On x86_64: 1st argument in %rdi, 2nd argument in %rsi */
	struct drm_device *dev = (struct drm_device *)regs->di;
	struct drm_atomic_state *state = (struct drm_atomic_state *)regs->si;
	struct drm_crtc *crtc;
	struct drm_crtc_state *new_crtc_state;
	int i;
	int cur_gen;
	int enabled;
	struct drm_color_ctm ctm_copy;
	unsigned long flags;

	if (!dev || !state)
		return 0;

	if (!dev->driver || !dev->driver->name)
		return 0;

	/* Verify target is AMDGPU driver */
	if (strcmp(dev->driver->name, "amdgpu") != 0)
		return 0;

	enabled = g_enabled;
	cur_gen = atomic_read(&config_generation);

	if (enabled == 1) {
		spin_lock_irqsave(&g_ctm_lock, flags);
		memcpy(&ctm_copy, &g_current_ctm, sizeof(ctm_copy));
		spin_unlock_irqrestore(&g_ctm_lock, flags);

		for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
			unsigned int idx;
			struct drm_property_blob *new_blob;

			if (!new_crtc_state || !new_crtc_state->active)
				continue;

			idx = drm_crtc_index(crtc);
			if (idx >= MAX_CRTCS)
				continue;

			/* Skip re-allocation if already applied with same config generation */
			if (g_crtc_applied_gen[idx] == cur_gen && new_crtc_state->ctm)
				continue;

			/* Create a new kernel property blob with updated CTM matrix */
			new_blob = drm_property_create_blob(dev, sizeof(struct drm_color_ctm), &ctm_copy);
			if (IS_ERR(new_blob))
				continue;

			/* Replace CTM blob in new_crtc_state and drop local reference */
			drm_property_replace_blob(&new_crtc_state->ctm, new_blob);
			drm_property_blob_put(new_blob);
			new_crtc_state->color_mgmt_changed = true;

			g_crtc_applied_gen[idx] = cur_gen;
		}
	} else {
		/* enabled == 0: clean up custom CTM if active */
		for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
			unsigned int idx;

			if (!new_crtc_state)
				continue;

			idx = drm_crtc_index(crtc);
			if (idx >= MAX_CRTCS)
				continue;

			if (new_crtc_state->ctm && g_crtc_applied_gen[idx] != 0) {
				drm_property_replace_blob(&new_crtc_state->ctm, NULL);
				new_crtc_state->color_mgmt_changed = true;
				g_crtc_applied_gen[idx] = 0;
			}
		}
	}

	return 0;
}

static struct kprobe chroma_kp = {
	.symbol_name = "drm_atomic_helper_commit",
	.pre_handler = chroma_pre_handler,
};

static int __init chroma_init(void)
{
	int ret;

	pr_info("chroma_amd: Initializing Linux kernel module for AMDGPU hardware color acceleration (prism-amd)...\n");

	/* Pre-calculate initial CTM (100% saturation, 100% contrast) */
	chroma_recalc_ctm();

	/* Create sysfs interface /sys/kernel/chroma_amd/ */
	chroma_kobj = kobject_create_and_add("chroma_amd", kernel_kobj);
	if (!chroma_kobj) {
		pr_err("chroma_amd: Failed to create kobject in kernel_kobj\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(chroma_kobj, &chroma_attr_group);
	if (ret) {
		pr_err("chroma_amd: Failed to create sysfs attribute group: %d\n", ret);
		kobject_put(chroma_kobj);
		return ret;
	}

	/* Register kprobe on drm_atomic_helper_commit */
	ret = register_kprobe(&chroma_kp);
	if (ret < 0) {
		pr_err("chroma_amd: Failed to register kprobe on drm_atomic_helper_commit: %d\n", ret);
		sysfs_remove_group(chroma_kobj, &chroma_attr_group);
		kobject_put(chroma_kobj);
		return ret;
	}

	pr_info("chroma_amd: Kprobe registered successfully on %s (addr: %p)\n",
		chroma_kp.symbol_name, chroma_kp.addr);
	pr_info("chroma_amd: Interface /sys/kernel/chroma_amd/ ready for real-time control!\n");

	return 0;
}

static void __exit chroma_exit(void)
{
	pr_info("chroma_amd: Unloading module...\n");

	unregister_kprobe(&chroma_kp);

	if (chroma_kobj) {
		sysfs_remove_group(chroma_kobj, &chroma_attr_group);
		kobject_put(chroma_kobj);
	}

	pr_info("chroma_amd: Module unloaded successfully.\n");
}

module_init(chroma_init);
module_exit(chroma_exit);
