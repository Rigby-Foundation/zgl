/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* libzgl internals. One context is current per process (sic has one GL
 * user per window and the state lives here). */
#pragma once
#include <GL/gl.h>
#include <virgl.h>
#include <stdint.h>
#include <stddef.h>

#define ZGL_MAX_ATTRIBS   16
#define ZGL_MAX_UNITS     32            /* texture units, all stages together */
#define ZGL_MAX_STAGE_SAMPLERS 16       /* samplers one stage can use */
#define ZGL_MAX_DRAW_BUFFERS 8
#define ZGL_MAX_UBO_BINDINGS 36         /* GL_UNIFORM_BUFFER binding points */
#define ZGL_MAX_STAGE_UBOS 12           /* uniform blocks per stage */
#define ZGL_MAX_XFB_BUFFERS 4
#define ZGL_TBO_WIDTH     4096          /* texture buffers are 2D textures this wide on the host */
#define ZGL_TBO_SHIFT     12
#define ZGL_TBSIZE_PREFIX "gl_zglTexBufSize."   /* the hidden uniform with a buffer sampler's element count */
#define ZGL_MAX_LIGHTS    8
#define ZGL_MAX_CONSTS    1000          /* vec4 slots per stage, uniform blocks included (macOS GL gives a stage 1024; the host keeps some) */
#define ZGL_MV_STACK      32
#define ZGL_PROJ_STACK    8
#define ZGL_TEX_STACK     4

/* conventional attribute locations for the fixed-function inputs (and the
 * gl_* attributes of compat GLSL): the same numbers NVIDIA and Mesa use */
#define ZGL_ATTR_VERTEX   0
#define ZGL_ATTR_NORMAL   2
#define ZGL_ATTR_COLOR    3
#define ZGL_ATTR_TEXCOORD 8

/* ---- compiled shaders ---------------------------------------------------- */
/* The types of interface variables (uniforms, attributes, varyings): what the
 * GL layer needs to store and check values. The first 19 keep their old
 * numbers. glsl_types.c has the table behind the helpers below. */
enum zgl_type { T_VOID, T_FLOAT, T_INT, T_BOOL, T_VEC2, T_VEC3, T_VEC4, T_IVEC2, T_IVEC3, T_IVEC4,
                T_BVEC2, T_BVEC3, T_BVEC4, T_MAT2, T_MAT3, T_MAT4, T_SAMPLER2D, T_SAMPLERCUBE, T_STRUCT,
                T_UINT, T_UVEC2, T_UVEC3, T_UVEC4,
                T_MAT2X3, T_MAT2X4, T_MAT3X2, T_MAT3X4, T_MAT4X2, T_MAT4X3,
                T_SAMPLER1D, T_SAMPLER3D, T_SAMPLER2DRECT, T_SAMPLER1DARRAY, T_SAMPLER2DARRAY, T_SAMPLERBUFFER,
                T_SAMPLER2DMS, T_SAMPLER2DMSARRAY, T_SAMPLERCUBEARRAY,
                T_SAMPLER1DSHADOW, T_SAMPLER2DSHADOW, T_SAMPLERCUBESHADOW, T_SAMPLER2DRECTSHADOW,
                T_SAMPLER1DARRAYSHADOW, T_SAMPLER2DARRAYSHADOW, T_SAMPLERCUBEARRAYSHADOW,
                T_ISAMPLER1D, T_ISAMPLER2D, T_ISAMPLER3D, T_ISAMPLERCUBE, T_ISAMPLER2DRECT, T_ISAMPLER1DARRAY,
                T_ISAMPLER2DARRAY, T_ISAMPLERBUFFER, T_ISAMPLER2DMS, T_ISAMPLER2DMSARRAY, T_ISAMPLERCUBEARRAY,
                T_USAMPLER1D, T_USAMPLER2D, T_USAMPLER3D, T_USAMPLERCUBE, T_USAMPLER2DRECT, T_USAMPLER1DARRAY,
                T_USAMPLER2DARRAY, T_USAMPLERBUFFER, T_USAMPLER2DMS, T_USAMPLER2DMSARRAY, T_USAMPLERCUBEARRAY,
                T_TYPE_COUNT };

/* the scalar type of t's components (T_FLOAT, T_INT, T_UINT, T_BOOL; for a
 * sampler what it returns), its rows (1-4), its columns (0: not a matrix) */
enum zgl_type zgl_type_base(enum zgl_type t);
int  zgl_type_vec(enum zgl_type t);
int  zgl_type_cols(enum zgl_type t);
int  zgl_type_is_sampler(enum zgl_type t);
int  zgl_type_slots(enum zgl_type t);       /* vec4 registers one value takes */
const char *zgl_type_name(enum zgl_type t);
const char *zgl_type_tgsi_target(enum zgl_type t);   /* samplers: "2D", "SHADOWCUBE", ... */

#define ZGL_MAX_VARS 96
struct zgl_var {                            /* an interface variable of a compiled shader */
    char name[48];
    enum zgl_type type;
    int  array;                             /* 0 = not an array, else the element count */
    int  slot;                              /* attribute index / varying index / first vec4 constant / sampler unit */
    int  slots;                             /* vec4 slots taken (uniforms) */
    int  builtin;                           /* ZGL_STATE_* for the gl_* state uniforms, else 0 */
    int  location;                          /* layout(location = n), -1 if none */
    int  flat;                              /* varyings: integer or `flat` (no interpolation) */
    int  reg;                               /* VS varyings: the first OUT register */
};

#define ZGL_MAX_BLOCK_MEMBERS 32
struct zgl_block_member {                   /* a uniform block member as GL lists it (struct and array leaves expanded) */
    char name[64];
    enum zgl_type type;
    int array;                              /* 0 = not an array */
    int offset, array_stride, matrix_stride, row_major;
};
struct zgl_block {                          /* a uniform block: constants cb.. (std140), filled from its buffer at draw time */
    char name[48];
    int cb, size, binding;                  /* cb: the first constant register; binding: layout(binding = n), -1 if none */
    struct zgl_block_member members[ZGL_MAX_BLOCK_MEMBERS]; int nmembers;
};

struct zgl_compiled {
    char *tgsi;
    struct zgl_block blocks[ZGL_MAX_STAGE_UBOS]; int nblocks;
    struct zgl_var attribs[ZGL_MAX_VARS]; int nattribs;      /* VS: attribute -> IN index */
    struct zgl_var varyings[ZGL_MAX_VARS]; int nvaryings;    /* GENERIC[n] */
    struct zgl_var uniforms[ZGL_MAX_VARS]; int nuniforms;    /* CONST slots */
    struct zgl_var samplers[ZGL_MAX_VARS]; int nsamplers;    /* SAMP index; slot = the unit set through the uniform */
    int nconsts;                            /* vec4 constants declared */
    struct zgl_var outputs[8]; int noutputs;    /* FS: user `out` variables -> COLOR[slot] */
    int version;                            /* the #version (110 if none) */
    int uses_frontcolor;                    /* varyings the compat built-ins declared */
    int position_reg, pointsize_reg;        /* VS: the OUT registers of gl_Position, gl_PointSize (-1: none) */
};

/* gl_* state uniforms the compiler recognises; filled in at draw time */
enum { ZGL_STATE_NONE, ZGL_STATE_MVP, ZGL_STATE_MV, ZGL_STATE_PROJ, ZGL_STATE_NORMAL, ZGL_STATE_MV_INV_T,
       ZGL_STATE_LIGHT0 /* + n * 8 slots */, ZGL_STATE_MATERIAL_FRONT = 100, ZGL_STATE_LIGHTMODEL_AMBIENT,
       ZGL_STATE_TEXTURE0_MATRIX, ZGL_STATE_FOG };

/* glsl*.c: GLSL 1.10-3.30 (and ES 1.00/3.00) to TGSI text. type is
 * GL_VERTEX_SHADER or GL_FRAGMENT_SHADER. Returns 0 and fills *out, or -1
 * with the log. zgl_compile_linked compiles a fragment shader against the
 * vertex shader's outputs (varying numbering). */
int zgl_compile(const char *source, GLenum type, struct zgl_compiled *out, char *log, size_t logsize);
int zgl_compile_linked(const char *source, GLenum type, const struct zgl_compiled *vs, struct zgl_compiled *out, char *log, size_t logsize);
/* ... with glBindFragDataLocation's bindings: frag_loc[k] names the output at location k ("" for none) */
int zgl_compile_linked_ex(const char *source, GLenum type, const struct zgl_compiled *vs, const char (*frag_loc)[48], const char *frag_index1,
                          struct zgl_compiled *out, char *log, size_t logsize);
void zgl_compiled_free(struct zgl_compiled *c);

/* ---- objects --------------------------------------------------------------- */
struct zgl_shader {
    GLuint name;
    GLenum type;
    char *source;
    struct zgl_compiled c;
    int compiled, deleted;
    char log[512];
    int refs;
};

struct zgl_uniform {                        /* per program: where a uniform lives in each stage */
    char name[48];
    enum zgl_type type;
    int  array;
    int  vs_slot, fs_slot;                  /* first vec4 slot, -1 if not in that stage */
    int  slots;
    int  builtin;
    int  sampler_unit;                      /* for samplers: the texture unit (glUniform1i) */
    int  sampler_units[16];                 /* ... one per element of a sampler array */
    int  block;                             /* the uniform block it is a member of, -1 for the default block */
    int  offset, array_stride, matrix_stride, row_major;   /* block members: the std140 layout */
};

struct zgl_program {
    GLuint name;
    struct zgl_shader *vs, *fs;
    int linked, deleted;
    char log[512];
    uint32_t h_vs, h_fs;                    /* virgl shader objects (0 = not created) */
    struct zgl_uniform uniforms[ZGL_MAX_VARS * 2]; int nuniforms;
    int attrib_loc[ZGL_MAX_VARS];           /* VS attrib i -> its (first) location */
    int in_loc[ZGL_MAX_ATTRIBS]; int nin;   /* VS input register k -> the location feeding it */
    int instance_in;                        /* the VS input register gl_InstanceID reads, -1 if none */
    char bound_attr[ZGL_MAX_ATTRIBS][48];   /* glBindAttribLocation requests */
    char bound_frag[ZGL_MAX_DRAW_BUFFERS][48];  /* glBindFragDataLocation requests */
    char bound_frag_index1[48];             /* glBindFragDataLocationIndexed(..., 0, 1, name) */
    float vs_consts[ZGL_MAX_CONSTS * 4], fs_consts[ZGL_MAX_CONSTS * 4];
    int vs_nconsts, fs_nconsts;
    int consts_dirty;
    uint32_t attrib_mask;                   /* attribute locations the VS reads */
    /* uniform blocks: index -> name, size, the binding point; per stage, the
     * host constant buffer index it was given (CONST[n], n >= 1), -1 if unused */
    struct { char name[48]; int size; int binding; int vs_index, fs_index;     /* the first constant register in each stage, -1 if unused */
             const struct zgl_buffer *src; size_t src_off; uint32_t src_version; } blocks[ZGL_MAX_STAGE_UBOS * 2]; int nblocks;   /* what was copied last */
    /* transform feedback */
    char **xfb_names; int nxfb; GLenum xfb_mode;
};

/* every GL buffer is one virgl buffer usable for vertices, indices,
 * constants, texture buffers and stream output (the host keeps it as an
 * untyped GL buffer), with the guest copy mapped */
struct zgl_buffer {
    GLuint name;
    virgl_res *res;
    size_t size;
    GLenum usage;
    uint8_t *shadow;                        /* the guest copy (= res->map) */
    int host_newer;                         /* the GPU wrote it since the guest copy was made (copies, stream output, packs) */
    uint32_t version;                       /* bumped at every write: uniform blocks copied from it know when to copy again */
    int mapped; GLbitfield map_access; size_t map_off, map_len;
    int deleted;
};

/* texture targets, as indices for the per-unit bindings */
enum { ZT_1D, ZT_2D, ZT_3D, ZT_CUBE, ZT_RECT, ZT_1D_ARRAY, ZT_2D_ARRAY, ZT_BUFFER, ZT_2D_MS, ZT_2D_MS_ARRAY, ZT_CUBE_ARRAY, ZT_COUNT };
int zgl_target_index(GLenum target);        /* GL_TEXTURE_* -> ZT_*, -1 if not a target (cube faces map to ZT_CUBE) */

struct zgl_sampler_params {                 /* what a texture or a sampler object says about filtering */
    GLenum min_filter, mag_filter, wrap_s, wrap_t, wrap_r;
    float min_lod, max_lod, lod_bias, max_aniso;
    GLenum compare_mode, compare_func;
    union { float f[4]; int32_t i[4]; } border;
};

/* how a GL internal format is stored on the host */
struct zgl_format {
    GLenum internal;                        /* the sized internal format */
    uint32_t vformat;                       /* VIRGL_FORMAT_* */
    uint8_t bpp;                            /* bytes per pixel (per block for compressed) */
    uint8_t comps;                          /* R, RG, RGB, RGBA: 1-4; depth/stencil: 1/2 */
    uint8_t kind;                           /* ZF_* */
    uint8_t block;                          /* block size (1 = not compressed) */
};
enum { ZF_UNORM, ZF_SNORM, ZF_FLOAT, ZF_INT, ZF_UINT, ZF_DEPTH, ZF_DEPTH_STENCIL, ZF_STENCIL, ZF_SRGB, ZF_COMPRESSED };
const struct zgl_format *zgl_format_of(GLenum internal, GLenum format, GLenum type);  /* NULL: not a format */
const struct zgl_format *zgl_format_ms(const struct zgl_format *f);   /* how a multisampled image of f is kept */
int zgl_can_multisample(uint32_t vformat);

struct zgl_texture {
    GLuint name;
    GLenum target;                          /* fixed at the first bind */
    virgl_res *res;
    const struct zgl_format *fmt;
    int w, h, d;                            /* level 0; d: depth (3D) or layers (arrays; 6 * n for cube arrays) */
    int levels;                             /* allocated on the host */
    int immutable, samples, fixed_locations;
    uint32_t defined;                       /* the levels that have an image */
    struct zgl_sampler_params sp;
    int base_level, max_level;
    GLint swizzle[4];
    GLenum depth_mode;                      /* GL_DEPTH_STENCIL_TEXTURE_MODE (4.3, but harmless) */
    int generate_mipmap;
    uint32_t h_view, h_sampler;             /* virgl objects, 0 = stale */
    int view_srgb;                          /* the view was made for this sRGB decode */
    struct zgl_buffer *buffer; size_t buf_off, buf_size;   /* GL_TEXTURE_BUFFER */
    const struct zgl_buffer *tbo_src; uint32_t tbo_version; int tbo_n;   /* what the host 2D texture holds */
    int has_level0;
};

struct zgl_renderbuffer {
    GLuint name;
    virgl_res *res;
    const struct zgl_format *fmt;
    int w, h, samples;
};

struct zgl_attachment {
    GLenum type;                            /* GL_NONE, GL_TEXTURE, GL_RENDERBUFFER */
    struct zgl_texture *tex;
    struct zgl_renderbuffer *rb;
    int level, layer;                       /* layer -1: all of them (layered) */
    uint32_t h_surf; uint64_t surf_key;     /* the surface, remade when the key changes */
};

struct zgl_fbo {
    GLuint name;
    struct zgl_attachment color[ZGL_MAX_DRAW_BUFFERS], depth, stencil;
    GLenum draw_buffers[ZGL_MAX_DRAW_BUFFERS]; int ndraw;
    GLenum read_buffer;
};

struct zgl_attrib {                         /* glVertexAttrib*Pointer state (part of a VAO) */
    int enabled;
    int size; GLenum type; int normalized; int stride;
    int integer;                            /* glVertexAttribIPointer: no conversion to float */
    int divisor;
    const void *pointer;
    struct zgl_buffer *buffer;              /* the ARRAY_BUFFER bound at the pointer call */
};

struct zgl_vao {
    GLuint name;
    struct zgl_attrib attribs[ZGL_MAX_ATTRIBS];
    struct zgl_buffer *element_buffer;
    int bound_once;
};

struct zgl_sampler {                        /* a sampler object (3.3) */
    GLuint name;
    struct zgl_sampler_params sp;
    uint32_t h_sampler;
};

struct zgl_query {
    GLuint name;
    GLenum target;                          /* fixed at the first glBeginQuery */
    virgl_res *res;                         /* the host writes struct virgl_host_query_state here */
    uint32_t handle;
    int active, pending;
    uint64_t result;
};

struct zgl_sync {
    GLenum status;                          /* GL_UNSIGNALED or GL_SIGNALED */
};

struct zgl_xfb {                            /* transform feedback state */
    struct zgl_buffer *buf[ZGL_MAX_XFB_BUFFERS];
    size_t off[ZGL_MAX_XFB_BUFFERS], size[ZGL_MAX_XFB_BUFFERS];
    uint32_t h_target[ZGL_MAX_XFB_BUFFERS];
    int active, paused, append;
    GLenum mode;
    struct zgl_program *program;
    struct zgl_query *written;              /* counts what was written, for GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN */
};

struct zgl_light {
    float ambient[4], diffuse[4], specular[4], position[4], spot_dir[4];
    float spot_exp, spot_cutoff, att[3];
    int enabled;
};

/* ---- the context --------------------------------------------------------- */
struct zgl_ctx {
    virgl *v;
    int width, height;
    virgl_res *color, *depth; uint32_t depth_format;
    uint32_t h_color_surf, h_depth_surf;
    uint32_t *frame;                        /* the last swapped frame, top-down */
    uint32_t *target; int target_stride;    /* where swaps go instead, if set (sic_gl_set_target) */

    GLenum error;
    float clear_color[4]; double clear_depth; int clear_stencil;
    int vp_x, vp_y, vp_w, vp_h; float depth_near, depth_far;
    int sc_x, sc_y, sc_w, sc_h;
    /* enables */
    int depth_test, cull, scissor, alpha_test, texture_2d[ZGL_MAX_UNITS], lighting, color_material, normalize_n, stencil_test;
    int polygon_offset, polygon_offset_line, polygon_offset_point;
    int depth_clamp, rasterizer_discard, primitive_restart, program_point_size, framebuffer_srgb, multisample;
    uint32_t clip_mask;                     /* GL_CLIP_DISTANCEi enabled */
    int sample_alpha_to_coverage, sample_alpha_to_one, line_smooth, polygon_smooth, cube_seamless, dither;
    GLuint restart_index;
    GLenum depth_func; int depth_mask;
    /* blending, per draw buffer (glEnablei, glColorMaski) */
    int blend[ZGL_MAX_DRAW_BUFFERS];
    int color_masks[ZGL_MAX_DRAW_BUFFERS];
    GLenum blend_src, blend_dst, blend_src_a, blend_dst_a, blend_eq, blend_eq_a; float blend_color[4];
    GLenum logic_op; int color_logic_op;
    int host_dual_src;                      /* the host GL's blend factors are dual-source ones (see draw.c) */
    /* stencil: [0] front, [1] back */
    GLenum stencil_func[2], stencil_fail[2], stencil_zfail[2], stencil_zpass[2];
    GLint stencil_ref[2]; GLuint stencil_vmask[2], stencil_wmask[2];
    GLenum cull_mode, front_face, polygon_mode; float line_width, point_size, po_factor, po_units;
    GLenum provoking_vertex;
    GLenum alpha_func; float alpha_ref;
    GLenum shade_model;
    GLbitfield sample_mask; int sample_mask_enabled; float sample_coverage; int sample_coverage_invert, sample_coverage_enabled;
    /* pixel storage */
    int unpack_alignment, unpack_row_length, unpack_image_height, unpack_skip_rows, unpack_skip_pixels, unpack_skip_images, unpack_swap, unpack_lsb;
    int pack_alignment, pack_row_length, pack_image_height, pack_skip_rows, pack_skip_pixels, pack_skip_images, pack_swap, pack_lsb;
    /* objects */
    struct zgl_shader **shaders; int nshaders;
    struct zgl_program **programs; int nprograms;
    struct zgl_buffer **buffers; int nbuffers;
    struct zgl_texture **textures; int ntextures;
    struct zgl_fbo **fbos; int nfbos;
    struct zgl_renderbuffer **rbs; int nrbs;
    struct zgl_vao **vaos; int nvaos;
    struct zgl_sampler **samplers; int nsamplers;
    struct zgl_query **queries; int nqueries;
    struct zgl_fbo *fbo;                    /* bound for drawing, NULL = the window */
    struct zgl_fbo *read_fbo;               /* bound for reading, NULL = the window */
    GLenum win_draw_buffer, win_read_buffer;    /* the window's (GL_BACK, GL_FRONT or GL_NONE) */
    struct zgl_renderbuffer *rb;            /* glBindRenderbuffer */
    int rt_w, rt_h;                         /* the current render target's size */
    GLuint next_name;
    struct zgl_program *program;            /* glUseProgram */
    struct zgl_vao vao0, *vao;              /* the bound vertex array (vao0: the default) */
    struct zgl_buffer *array_buffer, *copy_read_buffer, *copy_write_buffer, *pixel_pack_buffer, *pixel_unpack_buffer;
    struct zgl_buffer *uniform_buffer, *texture_buffer, *xfb_buffer;
    struct { struct zgl_buffer *buf; size_t off, size; } ubo[ZGL_MAX_UBO_BINDINGS];
    int active_unit, client_unit;
    struct zgl_texture *unit_tex[ZGL_MAX_UNITS][ZT_COUNT];
    struct zgl_sampler *unit_sampler[ZGL_MAX_UNITS];
    GLenum unit_env[ZGL_MAX_UNITS];
    struct { union { float f[4]; int32_t i[4]; uint32_t u[4]; } v; GLenum type; } cur_attrib[ZGL_MAX_ATTRIBS];  /* glVertexAttrib*: context state */
    struct zgl_query *active_query[8];      /* by ZQ_* kind */
    struct zgl_query *cond_query; GLenum cond_mode;
    struct zgl_xfb xfb;
    /* fixed function */
    GLenum matrix_mode;
    float mv[ZGL_MV_STACK][16]; int mv_top;
    float proj[ZGL_PROJ_STACK][16]; int proj_top;
    float texm[ZGL_TEX_STACK][16]; int tex_top;
    float cur_color[4], cur_normal[3], cur_texcoord[4];
    struct zgl_light lights[ZGL_MAX_LIGHTS];
    float mat_ambient[4], mat_diffuse[4], mat_specular[4], mat_emission[4], mat_shininess;
    float lm_ambient[4]; int lm_two_side;
    GLenum color_material_mode;
    /* immediate mode */
    GLenum imm_mode; int in_begin, imm_draw;
    float *imm; int imm_n, imm_cap;         /* 12 floats per vertex: pos4 color4 normal3 tc1(s) ... see ffp.c */
    /* client arrays (compat) */
    struct { int enabled; int size; GLenum type; int stride; const void *p; struct zgl_buffer *buffer; } cl_vertex, cl_normal, cl_color, cl_texcoord;
    struct zgl_program *ffp_program;        /* the current fixed-function program */
    struct zgl_program *clear_program;      /* clears under partial write masks (fbo.c) */
    int ffp_key;
    /* virgl state objects */
    uint32_t h_blend, h_rs, h_dsa, h_ve;
    uint64_t blend_key, rs_key, dsa_key, ve_key, ve_key2;
    uint32_t stencil_ref_sent, sample_mask_sent; int stencil_ref_valid, sample_mask_valid;
    struct { uint64_t key; uint32_t h; } sampler_cache[64]; int nsampler_cache;   /* sampler states by parameters */
    int fb_dirty, vp_dirty;
    virgl_res *scratch, *iscratch; size_t scratch_size, scratch_used, iscratch_used;   /* vertex and index staging */
    uint32_t h_scratch_ve;
    uint32_t stats_draws;
};

extern struct zgl_ctx *zgl;                 /* the current context */
#define ZGL_ATTRIBS (zgl->vao->attribs)

void zgl_set_error(GLenum e);
struct zgl_program *zgl_ffp_program(void);  /* ffp.c: the program for the current fixed-function state */
void zgl_ffp_fill_state(struct zgl_program *p);   /* fill gl_* state uniforms */
void zgl_ffp_init(struct zgl_ctx *c);
void zgl_ffp_bind_client_arrays(int enable);
/* draw.c */
struct zgl_draw_args {
    GLenum mode; GLint first; GLsizei count;
    GLenum index_type; const void *indices;     /* index_type 0: not indexed */
    GLsizei instances; GLint base_vertex;
    int range_known; GLuint range_start, range_end;   /* glDrawRangeElements */
};
void zgl_draw_ex(const struct zgl_draw_args *a);
void zgl_draw(GLenum mode, GLint first, GLsizei count, GLenum index_type, const void *indices);
void zgl_apply_framebuffer(void);
void zgl_rt_size(int *w, int *h);
void zgl_program_upload(struct zgl_program *p);
int  zgl_link(struct zgl_program *p);
/* Store `count` elements of a uniform from `data`: 32-bit words already in the
 * uniform's representation (floats, int/uint bits, bools as ~0/0). */
void zgl_uniform_set(struct zgl_program *p, int loc, const void *data, int count, int is_matrix_cols);
uint32_t zgl_texture_view(struct zgl_texture *t);
uint32_t zgl_sampler_state(struct zgl_texture *t, struct zgl_sampler *s);
int  zgl_texture_complete(struct zgl_texture *t);
int  zgl_tbo_elements(const struct zgl_texture *t);
int  zgl_tbo_update(struct zgl_texture *t);
void zgl_flush_scratch(void);
/* buf.c */
struct zgl_buffer *zgl_buffer_get(GLuint name);
struct zgl_buffer **zgl_buffer_slot(GLenum target);     /* the binding point, NULL if not a buffer target */
void zgl_buffer_sync(struct zgl_buffer *b);             /* make the guest copy current (after the GPU wrote it) */
int  zgl_buffer_alloc(struct zgl_buffer *b, size_t size);
void zgl_buffer_write(struct zgl_buffer *b, size_t off, const void *data, size_t size);
/* tex.c */
struct zgl_texture *zgl_texture_get(GLuint name);
struct zgl_texture *zgl_bound_texture(GLenum target);   /* on the active unit, NULL with an error if the target is bad */
void zgl_texture_free(struct zgl_texture *t);
/* Pixels between client memory and a format: pack is GL's (format, type)
 * with the pixel storage state. tex.c. */
struct zgl_pixel_store { int alignment, row_length, image_height, skip_rows, skip_pixels, skip_images, swap; };
void zgl_unpack_store(struct zgl_pixel_store *ps);
void zgl_pack_store(struct zgl_pixel_store *ps);
int  zgl_pixels_to_format(const struct zgl_format *f, int w, int h, int d, GLenum format, GLenum type, const void *src,
                          const struct zgl_pixel_store *ps, uint8_t *dst);
int  zgl_pixels_from_format(const struct zgl_format *f, int w, int h, int d, const uint8_t *src, GLenum format, GLenum type,
                            const struct zgl_pixel_store *ps, void *dst);
size_t zgl_pixels_size(int w, int h, int d, GLenum format, GLenum type, const struct zgl_pixel_store *ps);
const void *zgl_unpack_pointer(const void *pixels, size_t need);   /* through a bound PIXEL_UNPACK buffer; NULL + error if out of range */
/* fbo.c */
struct zgl_fbo *zgl_fbo_get(GLuint name);
struct zgl_renderbuffer *zgl_rb_get(GLuint name);
uint32_t zgl_attachment_surface(struct zgl_attachment *a);
void zgl_detach_texture(struct zgl_texture *t);
/* Read a rectangle of a resource level/layer back into guest memory as the
 * resource's own format, through a staging copy; returns malloc'd rows. */
uint8_t *zgl_read_resource(virgl_res *res, const struct zgl_format *f, int level, int x, int y, int z, int w, int h);
/* state.c */
void zgl_query_result(struct zgl_query *q, int wait);
/* gl.c: transform feedback's stream output for a program being linked */
struct zgl_so_info { int n; uint32_t stride[4]; uint32_t out[64]; };
int zgl_xfb_layout(struct zgl_program *p, struct zgl_so_info *so);
