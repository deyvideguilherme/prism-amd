// SPDX-License-Identifier: GPL-2.0
/*
 * chroma_amd.c - Linux Kernel Module for AMDGPU Hardware Color Acceleration (prism-amd)
 *
 * Injects Rec.709 Color Transformation Matrices (CTM) directly into
 * the AMD Display Core Next (DCN) hardware pipeline by intercepting DRM atomic commits via kprobe.
 *
 * Device node: /dev/chroma_amd (character miscdevice, mode 0660, group video)
 * Telemetry:   /sys/kernel/chroma_amd/ (read-only mode 0444)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
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
MODULE_DESCRIPTION("Hardware Linear CTM Interceptor for AMDGPU DRM/KMS");
MODULE_VERSION("1.3.0");

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

/* State tuning parameters */
static int g_saturation = 100; // 0 to 400 (default 100 = 100%)
static int g_contrast = 100;   // 10 to 300 (default 100 = 100%)
static int g_brightness = 0;   // -100 to 100 (default 0 = 0%)
static int g_enabled = 1;      // 0 or 1 (default 1)

static atomic_t config_generation = ATOMIC_INIT(1);
static int g_crtc_applied_ctm_gen[MAX_CRTCS];

static DEFINE_SPINLOCK(g_ctm_lock);
static struct drm_color_ctm g_current_ctm;

/*
 * Returns true if current configuration matches identity transform
 * (100% saturation, 100% contrast, 0% brightness).
 */
static inline bool chroma_is_identity(void)
{
	return (g_saturation == 100 && g_contrast == 100 && g_brightness == 0);
}

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

/*
 * Recalculates current CTM from active tuning parameters
 */
static void chroma_recalc_ctm(void)
{
	struct drm_color_ctm new_ctm;
	unsigned long flags;

	chroma_compute_ctm(&new_ctm, g_saturation, g_contrast, g_brightness);

	spin_lock_irqsave(&g_ctm_lock, flags);
	memcpy(&g_current_ctm, &new_ctm, sizeof(new_ctm));
	spin_unlock_irqrestore(&g_ctm_lock, flags);
}

/*
 * /dev/chroma_amd File Operations (miscdevice)
 */
static ssize_t chroma_dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	char msg[128];
	int len;

	len = scnprintf(msg, sizeof(msg),
			"enabled=%d\n"
			"saturation=%d\n"
			"contrast=%d\n"
			"brightness=%d\n",
			g_enabled, g_saturation, g_contrast, g_brightness);

	return simple_read_from_buffer(buf, count, ppos, msg, len);
}

static ssize_t chroma_dev_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char kbuf[256];
	char *p, *line, *key, *val_str;
	int val;
	bool changed = false;

	if (count == 0)
		return 0;
	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	p = kbuf;

	while ((line = strsep(&p, " \t\r\n")) != NULL) {
		if (*line == '\0')
			continue;

		key = strsep(&line, "=");
		val_str = line;
		if (!key || !val_str || *val_str == '\0')
			return -EINVAL;

		if (kstrtoint(val_str, 10, &val) < 0)
			return -EINVAL;

		if (strcmp(key, "saturation") == 0) {
			if (val < 0 || val > 400)
				return -EINVAL;
			g_saturation = val;
			changed = true;
		} else if (strcmp(key, "contrast") == 0) {
			if (val < 10 || val > 300)
				return -EINVAL;
			g_contrast = val;
			changed = true;
		} else if (strcmp(key, "brightness") == 0) {
			if (val < -100 || val > 100)
				return -EINVAL;
			g_brightness = val;
			changed = true;
		} else if (strcmp(key, "enabled") == 0) {
			if (val != 0 && val != 1)
				return -EINVAL;
			g_enabled = val;
			changed = true;
		} else {
			return -EINVAL;
		}
	}

	if (changed) {
		chroma_recalc_ctm();
		atomic_inc(&config_generation);
	}

	return count;
}

static const struct file_operations chroma_fops = {
	.owner = THIS_MODULE,
	.read = chroma_dev_read,
	.write = chroma_dev_write,
	.llseek = default_llseek,
};

static struct miscdevice chroma_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "chroma_amd",
	.fops = &chroma_fops,
	.mode = 0660,
};

/*
 * Sysfs Read-Only Telemetry (/sys/kernel/chroma_amd/)
 */
static ssize_t saturation_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_saturation);
}
static struct kobj_attribute saturation_attr = __ATTR_RO(saturation);

static ssize_t contrast_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_contrast);
}
static struct kobj_attribute contrast_attr = __ATTR_RO(contrast);

static ssize_t brightness_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_brightness);
}
static struct kobj_attribute brightness_attr = __ATTR_RO(brightness);

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", g_enabled);
}
static struct kobj_attribute enabled_attr = __ATTR_RO(enabled);

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

	if (enabled == 1 && !chroma_is_identity()) {
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

			/* Apply CTM matrix (combines saturation, contrast and linear brightness gain) */
			if (g_crtc_applied_ctm_gen[idx] != cur_gen || !new_crtc_state->ctm) {
				new_blob = drm_property_create_blob(dev, sizeof(struct drm_color_ctm), &ctm_copy);
				if (!IS_ERR(new_blob)) {
					drm_property_replace_blob(&new_crtc_state->ctm, new_blob);
					drm_property_blob_put(new_blob);
					new_crtc_state->color_mgmt_changed = true;
					g_crtc_applied_ctm_gen[idx] = cur_gen;
				}
			}
		}
	} else {
		/* enabled == 0 or identity: clean up custom CTM if active */
		for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
			unsigned int idx;

			if (!new_crtc_state)
				continue;

			idx = drm_crtc_index(crtc);
			if (idx >= MAX_CRTCS)
				continue;

			if (new_crtc_state->ctm && g_crtc_applied_ctm_gen[idx] != 0) {
				drm_property_replace_blob(&new_crtc_state->ctm, NULL);
				new_crtc_state->color_mgmt_changed = true;
				g_crtc_applied_ctm_gen[idx] = 0;
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

	/* Register misc character device /dev/chroma_amd */
	ret = misc_register(&chroma_misc_dev);
	if (ret) {
		pr_err("chroma_amd: Failed to register misc device /dev/chroma_amd: %d\n", ret);
		return ret;
	}

	/* Create read-only sysfs telemetry interface /sys/kernel/chroma_amd/ */
	chroma_kobj = kobject_create_and_add("chroma_amd", kernel_kobj);
	if (!chroma_kobj) {
		pr_err("chroma_amd: Failed to create kobject in kernel_kobj\n");
		misc_deregister(&chroma_misc_dev);
		return -ENOMEM;
	}

	ret = sysfs_create_group(chroma_kobj, &chroma_attr_group);
	if (ret) {
		pr_err("chroma_amd: Failed to create sysfs attribute group: %d\n", ret);
		kobject_put(chroma_kobj);
		misc_deregister(&chroma_misc_dev);
		return ret;
	}

	/* Register kprobe on drm_atomic_helper_commit */
	ret = register_kprobe(&chroma_kp);
	if (ret < 0) {
		pr_err("chroma_amd: Failed to register kprobe on drm_atomic_helper_commit: %d\n", ret);
		sysfs_remove_group(chroma_kobj, &chroma_attr_group);
		kobject_put(chroma_kobj);
		misc_deregister(&chroma_misc_dev);
		return ret;
	}

	pr_info("chroma_amd: Kprobe registered successfully on %s (addr: %p)\n",
		chroma_kp.symbol_name, chroma_kp.addr);
	pr_info("chroma_amd: /dev/chroma_amd ready for zero-overhead hardware control!\n");

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

	misc_deregister(&chroma_misc_dev);

	pr_info("chroma_amd: Module unloaded successfully.\n");
}

module_init(chroma_init);
module_exit(chroma_exit);
