obj-m += chroma_amd.o

KDIR ?= /lib/modules/$(if $(KERNELRELEASE),$(KERNELRELEASE),$(shell uname -r))/build
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
	sudo insmod chroma_amd.ko

unload:
	sudo rmmod chroma_amd

install-udev:
	sudo install -Dm644 99-chroma-amd.rules $(UDEV_DIR)/99-chroma-amd.rules
	sudo udevadm control --reload-rules

uninstall-udev:
	sudo rm -f $(UDEV_DIR)/99-chroma-amd.rules
	sudo udevadm control --reload-rules

.PHONY: all clean load unload install-udev uninstall-udev
