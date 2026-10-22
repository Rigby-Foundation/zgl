# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Rigby Foundation
# libzgl on macOS, with virglrenderer linked in (vhost.c) the way QEMU's
# virtio-gpu uses it: the GL layer, the compiler and libvirgl's encoder exactly
# as on sic, the same virglrenderer the VM's host runs (Homebrew's) executing
# what they send. Test tooling only.
#
#   make            build/glslrun (the .test runner) and build/glapi (API tests)
#   make check      run both

LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
CC := $(if $(LLVM_PREFIX),$(LLVM_PREFIX)/bin/clang,cc)
VIRGL ?= $(shell brew --prefix virglrenderer 2>/dev/null)
ZGL := ../..
B := build
CFLAGS := -O1 -g -Wall -Wno-unused-function -Wno-deprecated-declarations -DZGL_VHOST -I. -I$(ZGL)/include -I$(ZGL)/lib -I$(ZGL)/lib/gl \
          -I$(VIRGL)/include -idirafter $(ZGL)/../sic/include
LIBS := -L$(VIRGL)/lib -lvirglrenderer -Wl,-rpath,$(VIRGL)/lib -framework OpenGL
LIB_SRC := $(wildcard $(ZGL)/lib/gl/*.c) $(ZGL)/lib/virgl.c vhost.c
LIB_OBJ := $(patsubst %.c,$(B)/obj/%.o,$(notdir $(LIB_SRC)))

vpath %.c $(ZGL)/lib/gl $(ZGL)/lib .

.PHONY: all check clean
all: $(B)/glslrun $(B)/glapi

$(B)/obj/%.o: %.c $(wildcard $(ZGL)/lib/gl/*.h $(ZGL)/include/*.h $(ZGL)/include/GL/*.h) vhost.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ZGL)/lib/gl/procs.inc: $(ZGL)/include/GL/gl.h $(ZGL)/include/GL/gl3_funcs.h $(ZGL)/tools/gen/procs.awk
	awk -f $(ZGL)/tools/gen/procs.awk $(ZGL)/include/GL/gl.h $(ZGL)/include/GL/gl3_funcs.h > $@
$(B)/obj/glue.o: $(ZGL)/lib/gl/procs.inc

$(B)/glslrun: $(ZGL)/tools/glslrun.c $(LIB_OBJ)
	$(CC) $(CFLAGS) $< $(LIB_OBJ) $(LIBS) -o $@

$(B)/glapi: $(ZGL)/tools/glapi.c $(LIB_OBJ)
	$(CC) $(CFLAGS) $< $(LIB_OBJ) $(LIBS) -o $@

check: all
	$(B)/glslrun ../glsl/*.test && $(B)/glapi

clean:
	rm -rf $(B)
