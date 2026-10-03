# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Rigby Foundation
# Mesa's ir3, the shader compiler of its Adreno drivers, built for sic:
# with NIR, Gallium's TGSI front end (what libzgl's GLSL compiler emits) and
# the parts of Mesa's util they use. libir3.a, and ir3c: TGSI text in, an
# Adreno 6xx shader binary out, with its disassembly.
#
# The sources are Mesa's, from a pinned release (fetched once, checked);
# the files Mesa generates come from its own Python generators (python3
# with mako and pyyaml), except the assembler's flex/bison output, kept in
# gen/. sources.txt is what a full Mesa build compiles for these libraries
# (gen: marks the generated ones, zgl: our own in place of Mesa's: no
# shader cache on disk). HOST=1 builds ir3c for this machine.

MESA_VERSION := 26.2.4
MESA_SHA256  := bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9
MESA_URL     := https://archive.mesa3d.org/mesa-$(MESA_VERSION).tar.xz
PYTHON       ?= python3

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
HOST    ?= 0

MESA  := build/mesa-$(MESA_VERSION)
GEN   := build/gen
BUILD := build/$(if $(filter 1,$(HOST)),host,$(ARCH))

ifeq ($(ARCH),aarch64)
TARGET := aarch64-linux-musl
IMAGE_BASE := 0x8000000000
LD_EMUL := -m aarch64elf
else
TARGET := x86_64-linux-musl
IMAGE_BASE := 0x8000000000
LD_EMUL :=
endif

# What Mesa's configuration says about the system, for sic and its musl:
# no futex, dl*, zlib/zstd, shader cache on disk, libdrm or valgrind.
MESA_DEFS := -DPACKAGE_VERSION='"$(MESA_VERSION)"' -DPACKAGE_BUGREPORT='""' -DMESA_DEBUG=0 -DNDEBUG \
    -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
    -DHAVE_FREEDRENO -DHAVE_OPENGL=1 -DHAVE_OPENGL_ES_1=0 -DHAVE_OPENGL_ES_2=0 -DHAVE_GFX_COMPUTE \
    -DHAVE_PTHREAD -DHAVE_POLL_H -DHAVE_ENDIAN_H -DHAVE_DIRENT_D_TYPE -DHAVE_STRUCT_TIMESPEC -DHAVE_SYSCONF \
    -DHAVE_STRTOF -DHAVE_STRTOK_R -DHAVE_GNU_QSORT_R -DHAVE_POSIX_MEMALIGN -DHAVE_MKOSTEMP -DHAVE_FMEMOPEN \
    -DHAVE_UINT128 -DUSE_GCC_ATOMIC_BUILTINS -DTHREAD_SANITIZER=0 -DMAJOR_IN_SYSMACROS \
    -DHAVE_FUNC_ATTRIBUTE_ALIAS -DHAVE_FUNC_ATTRIBUTE_COLD -DHAVE_FUNC_ATTRIBUTE_CONST -DHAVE_FUNC_ATTRIBUTE_FLATTEN \
    -DHAVE_FUNC_ATTRIBUTE_FORMAT -DHAVE_FUNC_ATTRIBUTE_MALLOC -DHAVE_FUNC_ATTRIBUTE_NORETURN -DHAVE_FUNC_ATTRIBUTE_PACKED \
    -DHAVE_FUNC_ATTRIBUTE_PURE -DHAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL -DHAVE_FUNC_ATTRIBUTE_UNUSED \
    -DHAVE_FUNC_ATTRIBUTE_VISIBILITY -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT -DHAVE_FUNC_ATTRIBUTE_WEAK \
    -DHAVE___BUILTIN_ADD_OVERFLOW -DHAVE___BUILTIN_BSWAP32 -DHAVE___BUILTIN_BSWAP64 -DHAVE___BUILTIN_CLZ \
    -DHAVE___BUILTIN_CLZLL -DHAVE___BUILTIN_CTZ -DHAVE___BUILTIN_EXPECT -DHAVE___BUILTIN_FFS -DHAVE___BUILTIN_FFSLL \
    -DHAVE___BUILTIN_POPCOUNT -DHAVE___BUILTIN_POPCOUNTLL -DHAVE___BUILTIN_TYPES_COMPATIBLE_P -DHAVE___BUILTIN_UNREACHABLE \
    -DXXH_FORCE_ALIGN_CHECK=0 -DXXH_FORCE_MEMORY_ACCESS=0 \
    -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0

INC_DIRS := include src src/c11/impl src/compiler src/compiler/isaspec src/compiler/nir src/freedreno src/freedreno/common \
    src/freedreno/ir3 src/freedreno/isa src/freedreno/registers src/freedreno/registers/adreno src/gallium/auxiliary \
    src/gallium/auxiliary/util src/gallium/include src/util src/util/blake3 src/util/format
INCS := $(foreach d,$(INC_DIRS),-I$(GEN)/$(d) -I$(MESA)/$(d)) -I$(GEN)

ifeq ($(HOST),1)
CFLAGS := -O2 -g0 -w -fno-strict-aliasing -fno-math-errno -fno-trapping-math $(MESA_DEFS) -DHAVE_SECURE_GETENV $(INCS)
else
CFLAGS := --target=$(TARGET) -nostdinc -isystem $(LIBC)/include -idirafter $(shell $(CC) -print-resource-dir)/include -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables \
          -O2 -g0 -w -fno-strict-aliasing -fno-math-errno -fno-trapping-math $(MESA_DEFS) $(INCS)
LDFLAGS := $(LD_EMUL) -static -nostdlib --image-base=$(IMAGE_BASE) -z max-page-size=0x1000 -z noexecstack \
           --defsym=_DYNAMIC=$(IMAGE_BASE)
CRT_BEGIN := $(LIBC)/lib/crt1.o $(LIBC)/lib/crti.o
CRT_END   := $(LIBC)/lib/crtn.o
endif
stamp-osabi = printf '\123' | dd of=$(1) bs=1 seek=7 count=1 conv=notrunc status=none

SOURCES  := $(shell cat sources.txt)
src-path  = $(if $(filter gen:%,$(1)),$(GEN)/$(patsubst gen:%,%,$(1)),$(if $(filter zgl:%,$(1)),$(patsubst zgl:%,%,$(1)),$(MESA)/$(1)))
obj-path  = $(BUILD)/obj/$(basename $(patsubst zgl:%,zgl/%,$(patsubst gen:%,%,$(1)))).o
OBJS     := $(foreach s,$(SOURCES),$(call obj-path,$(s)))
LIB      := $(BUILD)/libir3.a
TOOL     := $(BUILD)/ir3c

.PHONY: all fetch generate install clean
all: $(LIB) $(TOOL)

# ---- Mesa's source ----------------------------------------------------------------------
$(MESA)/.unpacked:
	@mkdir -p build
	curl -fL --retry 3 -o build/mesa-$(MESA_VERSION).tar.xz $(MESA_URL)
	echo "$(MESA_SHA256)  build/mesa-$(MESA_VERSION).tar.xz" | shasum -a 256 -c -
	tar -xJf build/mesa-$(MESA_VERSION).tar.xz -C build
	@touch $@
fetch: $(MESA)/.unpacked

# ---- what Mesa generates (as its meson.build does) ---------------------------------------
N  := $(MESA)/src/compiler/nir
GN := $(GEN)/src/compiler/nir
GENERATED := $(GEN)/src/compiler/builtin_types.c $(GEN)/src/compiler/builtin_types.h \
    $(GN)/nir_constant_expressions.c $(GN)/nir_intrinsics.c $(GN)/nir_intrinsics.h $(GN)/nir_intrinsics_indices.h \
    $(GN)/nir_opcodes.c $(GN)/nir_opcodes.h $(GN)/nir_builder_opcodes.h $(GN)/nir_opt_algebraic.c \
    $(foreach p,branch_and_or_not imul trig triop_bitwise,$(GEN)/src/freedreno/ir3/ir3_nir_$(p).c) \
    $(GEN)/src/freedreno/isa/ir3-isa.c $(GEN)/src/freedreno/isa/ir3-isa.h $(GEN)/src/freedreno/isa/encode.h \
    $(GEN)/src/freedreno/common/freedreno_devices.h \
    $(GEN)/src/util/format/u_format_table.c $(GEN)/src/util/format/u_format_pack.h $(GEN)/src/util/format/u_format_gen.h \
    $(GEN)/src/util/format_srgb.c \
    $(GEN)/src/freedreno/ir3/ir3_lexer.c $(GEN)/src/freedreno/ir3/ir3_parser.c $(GEN)/src/freedreno/ir3/ir3_parser.h
generate: $(GENERATED)

$(GEN)/src/compiler/builtin_types.%: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/compiler/builtin_types_$*.py $@
$(GN)/nir_intrinsics.c $(GN)/nir_intrinsics.h $(GN)/nir_intrinsics_indices.h: $(GN)/%: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(N)/$(subst .,_,$*).py --out $@
$(GN)/nir_opt_algebraic.c: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(N)/nir_opt_algebraic.py --out $@
$(GN)/nir_constant_expressions.c: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(N)/nir_constant_expressions.py > $@
$(GN)/nir_opcodes.c $(GN)/nir_opcodes.h $(GN)/nir_builder_opcodes.h: $(GN)/%: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(N)/$(subst .,_,$*).py > $@
$(GEN)/src/freedreno/ir3/ir3_nir_%.c: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/freedreno/ir3/ir3_nir_$*.py -p $(N)/ > $@
$(GEN)/src/freedreno/isa/ir3-isa.c $(GEN)/src/freedreno/isa/ir3-isa.h &: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/compiler/isaspec/decode.py --xml $(MESA)/src/freedreno/isa/ir3.xml \
	    --out-c $(GEN)/src/freedreno/isa/ir3-isa.c --out-h $(GEN)/src/freedreno/isa/ir3-isa.h
$(GEN)/src/freedreno/isa/encode.h: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/compiler/isaspec/encode.py --xml $(MESA)/src/freedreno/isa/ir3.xml --out-h $@
$(GEN)/src/freedreno/registers/adreno/%.py: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/freedreno/registers/gen_header.py --rnn $(MESA)/src/freedreno/registers \
	    --xml $(MESA)/src/freedreno/registers/adreno/$*.xml py-defines > $@
$(GEN)/src/freedreno/common/freedreno_devices.h: $(GEN)/src/freedreno/registers/adreno/a6xx.py
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/freedreno/common/freedreno_devices.py -p $(GEN)/src/freedreno/registers/adreno/ > $@
$(GEN)/src/util/format/u_format_table.c: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/util/format/u_format_table.py $(MESA)/src/util/format/u_format.yaml > $@
$(GEN)/src/util/format/u_format_pack.h: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/util/format/u_format_table.py $(MESA)/src/util/format/u_format.yaml --header > $@
$(GEN)/src/util/format/u_format_gen.h: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/util/format/u_format_table.py $(MESA)/src/util/format/u_format.yaml --enums > $@
$(GEN)/src/util/format_srgb.c: $(MESA)/.unpacked
	@mkdir -p $(dir $@)
	$(PYTHON) $(MESA)/src/util/format_srgb.py > $@
$(GEN)/src/freedreno/ir3/ir3_lexer.c $(GEN)/src/freedreno/ir3/ir3_parser.c $(GEN)/src/freedreno/ir3/ir3_parser.h: $(GEN)/%: gen/%
	@mkdir -p $(dir $@)
	cp $< $@

# ---- the library and the tool -----------------------------------------------------------
define compile
$(call obj-path,$(1)): $(call src-path,$(1)) | $(GENERATED)
	@mkdir -p $$(dir $$@)
	@echo "  CC   $(1)"
	@$(CC) $(CFLAGS) $(if $(filter %.cpp,$(1)),-std=c++17,-std=gnu11) -c $$< -o $$@
endef
$(foreach s,$(SOURCES),$(eval $(call compile,$(s))))

$(LIB): $(OBJS)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/obj/ir3c.o: ir3c.c | $(GENERATED)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu11 -c $< -o $@

ifeq ($(HOST),1)
$(TOOL): $(BUILD)/obj/ir3c.o $(LIB)
	$(CC) -o $@ $^ -lm -lpthread
else
$(TOOL): $(BUILD)/obj/ir3c.o $(LIB)
	$(LD) $(LDFLAGS) -o $@ $(CRT_BEGIN) $^ $(LIBC)/lib/libc.a $(wildcard $(LIBC)/lib/libcompiler_rt.a) $(CRT_END)
	@$(call stamp-osabi,$@)
endif

install: all
	@mkdir -p $(SYSROOT)/rootfs/bin $(SYSROOT)/usr/lib
	cp $(TOOL) $(SYSROOT)/rootfs/bin/
	cp $(LIB) $(SYSROOT)/usr/lib/

clean:
	rm -rf build/host build/x86_64 build/aarch64 build/gen
