# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Rigby Foundation
# Host-side shader tests for zgl (macOS or Linux, not sic):
#
#   vtrans   TGSI text -> the GLSL virglrenderer would run on the host
#   glsltest runs tests/glsl/*.test: zgl compiles each shader to TGSI,
#            virglrenderer's translator turns that into host GLSL, the host
#            OpenGL (macOS: a 4.1 core context) compiles, links and draws it,
#            and the pixels are checked against the expected values
#
# virglrenderer is only test tooling: a pinned checkout under build/, its TGSI
# parser and vrend_shader.c compiled as they are, the way the host runs them.
#
#   make            build both
#   make check      run every test

VIRGL_URL    := https://gitlab.freedesktop.org/virgl/virglrenderer.git
VIRGL_COMMIT := ca50e008863837e094747a69974dde3ae148aeaa   # virglrenderer-1.3.0, what Homebrew's qemu hosts run

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
CC := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/clang,cc)
B  := build
V  := $(B)/virglrenderer/src
ZGL := ../..

VINC := -I$(B) -I$(V) -I$(V)/gallium/include -I$(V)/gallium/auxiliary -I$(V)/gallium/auxiliary/util \
        -I$(V)/mesa -I$(V)/mesa/util -I$(V)/mesa/pipe -I$(V)/mesa/compat -I$(V)/vrend -I$(V)/mesa/include \
        -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -include $(B)/config.h
VSRC := gallium/auxiliary/tgsi/tgsi_text.c gallium/auxiliary/tgsi/tgsi_build.c gallium/auxiliary/tgsi/tgsi_parse.c \
        gallium/auxiliary/tgsi/tgsi_info.c gallium/auxiliary/tgsi/tgsi_iterate.c gallium/auxiliary/tgsi/tgsi_scan.c \
        gallium/auxiliary/tgsi/tgsi_strings.c gallium/auxiliary/tgsi/tgsi_util.c gallium/auxiliary/tgsi/tgsi_sanity.c \
        gallium/auxiliary/tgsi/tgsi_dump.c gallium/auxiliary/cso_cache/cso_hash.c gallium/auxiliary/cso_cache/cso_cache.c gallium/auxiliary/util/u_format.c mesa/util/u_debug.c mesa/util/os_misc.c vrend/vrend_shader.c
VOBJ := $(patsubst %.c,$(B)/v/%.o,$(VSRC)) $(B)/v/u_format_table.o

# zgl's compiler, built for the host
ZSRC := $(wildcard $(ZGL)/lib/gl/glsl*.c)
ZOBJ := $(patsubst $(ZGL)/lib/gl/%.c,$(B)/z/%.o,$(ZSRC))
ZINC := -I$(ZGL)/include -I$(ZGL)/lib/gl -idirafter $(ZGL)/../sic/include   # abi/gpu.h, after the host headers

ifeq ($(shell uname),Darwin)
GL_LIBS := -framework OpenGL
else
GL_LIBS := -lEGL -lGL
endif

.PHONY: all check clean distclean
all: $(B)/vtrans $(B)/glsltest

$(V)/vrend/vrend_shader.c:
	@mkdir -p $(B)
	git clone -q $(VIRGL_URL) $(B)/virglrenderer
	cd $(B)/virglrenderer && git checkout -q $(VIRGL_COMMIT)

$(B)/config.h: | $(V)/vrend/vrend_shader.c
	printf '#define HAVE___BUILTIN_EXPECT 1\n#define HAVE___BUILTIN_CLZ 1\n#define HAVE___BUILTIN_CLZLL 1\n#define HAVE___BUILTIN_FFS 1\n#define HAVE___BUILTIN_POPCOUNT 1\n#define HAVE_FUNC_ATTRIBUTE_UNUSED 1\n#define HAVE_FUNC_ATTRIBUTE_FORMAT 1\n' > $@
	printf '#define VIRGL_VERSION_MAJOR 1\n#define VIRGL_VERSION_MINOR 0\n#define VIRGL_VERSION_MICRO 0\n' > $(B)/virgl-version.h
	python3 $(V)/gallium/auxiliary/util/u_format_table.py $(V)/gallium/auxiliary/util/u_format.yaml --enums > $(B)/u_format_gen.h
	python3 $(V)/gallium/auxiliary/util/u_format_table.py $(V)/gallium/auxiliary/util/u_format.yaml > $(B)/u_format_table.c

$(B)/v/u_format_table.o: $(B)/config.h
	@mkdir -p $(dir $@)
	$(CC) -c -O1 -g -w $(VINC) $(B)/u_format_table.c -o $@

$(B)/v/%.o: $(V)/%.c $(B)/config.h
	@mkdir -p $(dir $@)
	$(CC) -c -O1 -g -w $(VINC) $< -o $@

$(B)/vtrans.o: vtrans.c $(B)/config.h
	$(CC) -c -O1 -g -Wall $(VINC) $< -o $@

$(B)/vtrans: vtrans.c $(VOBJ) $(B)/config.h
	$(CC) -O1 -g -Wall -DVTRANS_MAIN $(VINC) vtrans.c stubs.c $(VOBJ) -o $@

$(B)/z/%.o: $(ZGL)/lib/gl/%.c $(wildcard $(ZGL)/lib/gl/*.h)
	@mkdir -p $(dir $@)
	$(CC) -c -O1 -g -Wall -Wno-unused-function $(ZINC) $< -o $@

$(B)/glsltest: glsltest.c $(B)/vtrans.o $(VOBJ) $(ZOBJ)
	$(CC) -O1 -g -Wall -Wno-deprecated-declarations $(ZINC) glsltest.c stubs.c $(B)/vtrans.o $(VOBJ) $(ZOBJ) $(GL_LIBS) -o $@

check: all
	$(B)/glsltest ../glsl/*.test

clean:
	rm -rf $(B)/v $(B)/z $(B)/vtrans $(B)/glsltest $(B)/*.o

distclean:
	rm -rf $(B)
