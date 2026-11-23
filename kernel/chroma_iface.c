// SPDX-License-Identifier: GPL-2.0
#include "chroma_internal.h"

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

int chroma_iface_init(void)
{
	int ret;

	chroma_kobj = kobject_create_and_add("chroma_amd", kernel_kobj);
	if (!chroma_kobj)
		return -ENOMEM;

	ret = sysfs_create_groups(chroma_kobj, chroma_groups);
	if (ret)
		goto put_kobj;

	ret = misc_register(&chroma_misc_dev);
	if (ret)
		goto remove_sysfs;

	return 0;

remove_sysfs:
	sysfs_remove_groups(chroma_kobj, chroma_groups);
put_kobj:
	kobject_put(chroma_kobj);
	chroma_kobj = NULL;
	return ret;
}

void chroma_iface_exit(void)
{
	misc_deregister(&chroma_misc_dev);
	if (chroma_kobj) {
		sysfs_remove_groups(chroma_kobj, chroma_groups);
		kobject_put(chroma_kobj);
		chroma_kobj = NULL;
	}
}
