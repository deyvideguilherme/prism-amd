#ifndef PRISMA_ABI_H
#define PRISMA_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

/*
 * Prisma Protocol & ABI Specification
 * Shared contract between chroma_amd.ko and prisma-ctl userspace.
 */

#define PRISMA_VERSION "1.0.0"
#define PRISMA_ABI_VERSION 1

struct prisma_ioctl_state {
	uint32_t abi_version;
	int32_t enabled;
	int32_t saturation;
	int32_t contrast;
	int32_t brightness;
};

#define PRISMA_IOC_MAGIC 'P'
#define PRISMA_IOC_GET_ABI   _IOR(PRISMA_IOC_MAGIC, 1, uint32_t)
#define PRISMA_IOC_GET_STATE _IOR(PRISMA_IOC_MAGIC, 2, struct prisma_ioctl_state)
#define PRISMA_IOC_SET_STATE _IOW(PRISMA_IOC_MAGIC, 3, struct prisma_ioctl_state)

#endif /* PRISMA_ABI_H */
