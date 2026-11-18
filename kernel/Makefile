obj-m += chroma_amd.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Detecta automaticamente se o kernel foi compilado com Clang/LLVM (padrão no CachyOS)
CLANG_BUILT := $(shell grep -s "CONFIG_CC_IS_CLANG=y" $(KDIR)/.config)
ifneq ($(CLANG_BUILT),)
LLVM ?= 1
endif

EXTRA_FLAGS := $(if $(LLVM),LLVM=$(LLVM),)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) $(EXTRA_FLAGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $(EXTRA_FLAGS) clean

load:
	sudo insmod chroma_amd.ko

unload:
	sudo rmmod chroma_amd

.PHONY: all clean load unload
