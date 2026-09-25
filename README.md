# zgl — OpenGL for sic

The host GPU from user space, in two layers. Optional, like zwm: `make
install` drops the libraries and headers into the sysroot and the test
programs into its `rootfs/` overlay.

```
lib/virgl.c, include/virgl.h    libvirgl: /dev/gpu0 and the virgl command stream
lib/gl/                         libzgl: OpenGL 2.1 on top of it
include/GL/gl.h                 the API; GL/sic_gl.h the context ABI for SDL
tools/virgltri.c                a triangle through libvirgl alone
tools/zgltest.c                 libzgl: immediate mode, a lit cube, GLSL + VBO + texture
```

## How it works

sic's virtio-gpu driver speaks virgl when the host offers it (QEMU with
virglrenderer: `-device virtio-vga-gl -display sdl,gl=on`). `/dev/gpu0`
gives a process a rendering context, resources (textures and buffers, with
guest memory behind them when asked, mmap'able), transfers both ways, and
command submission (`include/abi/gpu.h` in sic). The host runs the commands
on its own GL; there is no GPU code in the kernel beyond that.

libvirgl wraps the ioctls and encodes the command stream: create and bind
objects (shaders as TGSI text, blend/rasterizer/depth-stencil state,
vertex elements, sampler views and states, surfaces), set the framebuffer,
viewport, constants and vertex/index buffers, draw, clear, copy, and the
inline writes for small uploads. `virgltri` uses it directly.

libzgl is OpenGL 2.1 on that: the object model (buffers, textures with
mipmaps, shaders, programs, uniforms, vertex attributes), the state (blend,
depth, stencil, cull, scissor, alpha test, polygon offset), and the
fixed-function pipeline OpenGL 1.x programs use — matrix stacks, lights and
materials, immediate mode, client arrays, texture environments — expressed
as generated GLSL programs so the host needs one path only. GLSL 1.20 is
compiled to TGSI by `lib/gl/glsl.c`: the types, constructors and swizzles,
control flow (constant-bound loops unrolled, the rest real loops), user
functions (inlined), the built-in functions, `gl_*` state and varyings,
textures. Quads, quad strips, polygons and line loops become index lists.

The frame is an offscreen colour + depth resource; `sic_gl_swap()` reads it
back (top row first, `0x00RRGGBB`) into wherever `sic_gl_set_target()`
pointed — SDL's zwm driver points it at the window's shared buffer, so a
swap is one readback and a damage message. When zwm composites on the GPU
nothing is read back: `sic_gl_flush()` copies the frame into a host buffer
(`sic_gl_buffer()`) that the server samples in place. zwm itself renders
with zgl into a buffer the display scans out (`sic_gl_scanout()`,
`sic_gl_present()`); `GL/zgl_ext.h` has what a compositor needs on top:
another program's buffer as a texture, fast uploads of `0x00RRGGBB`.

Not there: display lists, evaluators, feedback/selection, accumulation,
multisampling, 1D/3D textures, framebuffer objects, `glCopyTex*`.
`GL_RENDERER` names the host GPU (`zgl on virgl (Apple M4)`). Big-endian
guests (the PowerPC port) are out: the command stream and vertex data are
little-endian and virglrenderer assumes as much.

## Building

```bash
make            # build/<arch>/libvirgl.a libzgl.a virgltri zgltest
make install    # -> $SYSROOT/usr/{include,lib}, rootfs/bin
```

Needs the sysroot (sic's `abi/gpu.h`, libc, libzwm for the tools). Programs
include `<GL/gl.h>` and link `libzgl.a libvirgl.a`; SDL programs get it
through `SDL_GL_*` (see zde). `zgltest` prints what it rendered as ASCII
and `ok`, so it works over a serial console.

The host side is virglrenderer inside QEMU. Homebrew's QEMU is built
without it; on macOS build QEMU with `--enable-virglrenderer --enable-opengl
--enable-sdl` against Homebrew's `virglrenderer`, `mesa` (for EGL) and a
static `libepoxy` with EGL enabled. The SDL display needs a core-profile
context (QEMU asks for a legacy one; patch `ui/sdl2.c` to request 4.1 core
and `ui/shader/*` to `#version 150`). For a Retina screen the same file
gets `SDL_WINDOW_ALLOW_HIGHDPI`, reports the drawable's pixels (not the
window's points) in the resize event, and leaves the window alone once the
guest runs at that size (`ui/sdl2-gl.c` blits to the drawable): the guest
then runs at the panel's real resolution in a window of the same apparent
size, shown 1:1 instead of scaled up by macOS. The same build with
`--target-list=x86_64-softmmu,aarch64-softmmu` serves the aarch64 port:
`-device virtio-gpu-gl-pci -display sdl,gl=on` on `-M virt` (zgl is the
same code, little-endian either way).

## License

Copyright (C) 2026 Rigby Foundation. The libraries (`lib/`, `include/`) are
LGPL-2.1-or-later (`LICENSE.LGPL`) so any program can link them; the tools
and the build are GPL-2.0-only (`LICENSE`). `include/virgl_protocol.h`,
`virgl_hw.h` and `p_defines.h` are virglrenderer's (MIT).
