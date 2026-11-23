obj-m += chroma_amd.o
chroma_amd-y := chroma_main.o chroma_ctm.o chroma_drm.o chroma_iface.o

ifneq ($(srctree),)
ifneq ($(shell grep -l "^drm_atomic_commit_alloc" "$(srctree)/include/drm/drm_atomic.h"),)
ccflags-y += -DPRISMA_DRM_ATOMIC_COMMIT
endif
endif

ifndef KDIR
DEFAULT_KDIR := /lib/modules/$(if $(KERNELRELEASE),$(KERNELRELEASE),$(shell uname -r))/build
ifneq ($(wildcard $(DEFAULT_KDIR)),)
KDIR := $(DEFAULT_KDIR)
else
FALLBACK_KDIR := $(shell find /lib/modules /usr/lib/modules -mindepth 2 -maxdepth 2 -type d -name build 2>/dev/null | sort -V | tail -n 1)
KDIR := $(if $(FALLBACK_KDIR),$(FALLBACK_KDIR),$(DEFAULT_KDIR))
endif
endif
M ?= $(CURDIR)
UDEV_DIR ?= /etc/udev/rules.d

# Detecta automaticamente se o kernel foi compilado com Clang/LLVM (padrão no CachyOS)
ifneq ($(wildcard $(KDIR)/.config),)
CLANG_BUILT := $(shell grep -s "CONFIG_CC_IS_CLANG=y" $(KDIR)/.config)
ifneq ($(CLANG_BUILT),)
LLVM ?= 1
endif
endif

EXTRA_FLAGS := $(if $(LLVM),LLVM=$(LLVM),)

all:
	$(MAKE) -C $(KDIR) M=$(M) $(EXTRA_FLAGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(M) $(EXTRA_FLAGS) clean

load:
	-sudo modprobe drm_kms_helper 2>/dev/null || true
	sudo insmod chroma_amd.ko

unload:
	sudo rmmod chroma_amd

install-udev:
	sudo install -Dm644 70-chroma-amd.rules $(UDEV_DIR)/70-chroma-amd.rules
	sudo udevadm control --reload-rules

uninstall-udev:
	sudo rm -f $(UDEV_DIR)/70-chroma-amd.rules
	sudo udevadm control --reload-rules

.PHONY: all clean load unload install-udev uninstall-udev
