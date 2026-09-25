# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# zgl: the GPU from user space on sic — libvirgl (the host GPU through virtio-gpu)
# and libzgl (OpenGL on top of it).
# Optional: builds against the sysroot and installs
# into $(SYSROOT)/rootfs, which ZAE overlays onto the initrd.

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  ?= $(firstword $(foreach f,lld lld@21 lld@20 llvm,$(if $(wildcard $(shell brew --prefix $(f) 2>/dev/null)/bin/ld.lld),$(shell brew --prefix $(f)),)))
ifeq ($(origin CC),default)
CC := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/clang,clang)
endif
ifeq ($(origin LD),default)
LD := $(if $(LLD_PREFIX),$(LLD_PREFIX)/bin/ld.lld,ld.lld)
endif
ifeq ($(origin AR),default)
AR := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/llvm-ar,ar)
endif

SYSROOT ?= $(if $(SIC_SYSROOT),$(SIC_SYSROOT),$(HOME)/.sic/sysroot)
LIBC    := $(SYSROOT)/usr
ARCH    ?= x86_64
BUILD   := build/$(ARCH)

ifeq ($(ARCH),powerpc)
TARGET := powerpc-linux-musl
ARCH_CFLAGS := -mcpu=7450 -maltivec -fno-pic -fno-pie
IMAGE_BASE := 0x10000000
LD_EMUL := -m elf32ppc
else ifeq ($(ARCH),aarch64)
TARGET := aarch64-linux-musl
ARCH_CFLAGS := -fPIE
IMAGE_BASE := 0x8000000000
LD_EMUL := -m aarch64elf
else
TARGET := x86_64-linux-musl
ARCH_CFLAGS := -fPIE
IMAGE_BASE := 0x8000000000
LD_EMUL :=
endif

CFLAGS  := --target=$(TARGET) -std=c11 -nostdinc -isystem $(LIBC)/include -Iinclude \
           $(ARCH_CFLAGS) -fno-stack-protector -fno-asynchronous-unwind-tables \
           -O2 -g -Wall -Wextra -D_GNU_SOURCE
LDFLAGS := $(LD_EMUL) -static -nostdlib --image-base=$(IMAGE_BASE) -z max-page-size=0x1000 -z noexecstack \
           --defsym=_DYNAMIC=$(IMAGE_BASE)
CRT_BEGIN := $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o
CRT_END   := $(LIBC)/lib/crtn.o
stamp-osabi = printf '\123' | dd of=$(1) bs=1 seek=7 count=1 conv=notrunc status=none

VIRGL_OBJS := $(BUILD)/lib/virgl.o
VIRGL_LIB  := $(BUILD)/libvirgl.a
GL_OBJS    := $(patsubst lib/gl/%.c,$(BUILD)/lib/gl/%.o,$(wildcard lib/gl/*.c))
GL_LIB     := $(BUILD)/libzgl.a
TOOLS      := $(patsubst tools/%.c,$(BUILD)/%,$(wildcard tools/*.c))

.PHONY: all install install-headers clean
all: $(VIRGL_LIB) $(GL_LIB) $(TOOLS)

$(BUILD)/%.o: %.c $(wildcard include/*.h include/GL/*.h lib/gl/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(VIRGL_LIB): $(VIRGL_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

$(GL_LIB): $(GL_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/%: $(BUILD)/tools/%.o $(VIRGL_LIB) $(GL_LIB)
	$(LD) $(LDFLAGS) -o $@ $(CRT_BEGIN) $< $(GL_LIB) $(VIRGL_LIB) $(LIBC)/lib/libzwm.a $(LIBC)/lib/libc.a $(wildcard $(LIBC)/lib/libcompiler_rt.a) $(CRT_END)
	@$(call stamp-osabi,$@)

# Just the headers: the context ABI (GL/sic_gl.h) and the GL API TinyGL
# stands behind on machines without a GPU.
install-headers:
	@mkdir -p $(SYSROOT)/usr/include/GL
	cp include/GL/*.h $(SYSROOT)/usr/include/GL/
	@echo "headers installed into $(SYSROOT)"

install: all
	@mkdir -p $(SYSROOT)/rootfs/bin $(SYSROOT)/usr/include/GL $(SYSROOT)/usr/lib
	cp $(TOOLS) $(SYSROOT)/rootfs/bin/
	cp include/virgl.h include/virgl_protocol.h include/virgl_hw.h include/p_defines.h $(SYSROOT)/usr/include/
	cp include/GL/*.h $(SYSROOT)/usr/include/GL/ 2>/dev/null || true
	cp $(VIRGL_LIB) $(GL_LIB) $(SYSROOT)/usr/lib/
	@echo "installed into $(SYSROOT)"

clean:
	rm -rf build
