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
#define ZGL_MAX_UNITS     8
#define ZGL_MAX_LIGHTS    8
#define ZGL_MAX_CONSTS    256           /* vec4 slots per stage */
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
};

struct zgl_compiled {
    char *tgsi;
    struct zgl_var attribs[ZGL_MAX_VARS]; int nattribs;      /* VS: attribute -> IN index */
    struct zgl_var varyings[ZGL_MAX_VARS]; int nvaryings;    /* GENERIC[n] */
    struct zgl_var uniforms[ZGL_MAX_VARS]; int nuniforms;    /* CONST slots */
    struct zgl_var samplers[ZGL_MAX_VARS]; int nsamplers;    /* SAMP index; slot = the unit set through the uniform */
    int nconsts;                            /* vec4 constants declared */
    struct zgl_var outputs[8]; int noutputs;    /* FS: user `out` variables -> COLOR[slot] */
    int version;                            /* the #version (110 if none) */
    int uses_frontcolor;                    /* varyings the compat built-ins declared */
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
    char bound_attr[ZGL_MAX_ATTRIBS][48];   /* glBindAttribLocation requests */
    float vs_consts[ZGL_MAX_CONSTS * 4], fs_consts[ZGL_MAX_CONSTS * 4];
    int vs_nconsts, fs_nconsts;
    int consts_dirty;
    uint32_t attrib_mask;                   /* attribute locations the VS reads */
};

struct zgl_buffer {
    GLuint name;
    virgl_res *res;
    size_t size;
    GLenum bind;                            /* GL_ARRAY_BUFFER or GL_ELEMENT_ARRAY_BUFFER: the virgl bind was chosen at glBufferData */
    uint8_t *shadow;                        /* the guest copy (= res->map): index ranges, primitive rewrites */
};

struct zgl_texture {
    GLuint name;
    virgl_res *res;
    int w, h, levels;                       /* levels allocated on the host */
    uint32_t vformat;
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    int generate_mipmap;
    uint32_t h_view, h_sampler;             /* virgl objects, 0 = stale */
    int has_level0;
};

struct zgl_fbo {                            /* a framebuffer object: one colour attachment */
    GLuint name;
    struct zgl_texture *tex; int level;
    uint32_t h_surf;                        /* the surface on the texture level, 0 = not made yet */
    virgl_res *surf_res;                    /* the resource the surface was made on */
};

struct zgl_attrib {                         /* glVertexAttribPointer state */
    int enabled;
    int size; GLenum type; int normalized; int stride;
    const void *pointer;
    struct zgl_buffer *buffer;              /* the ARRAY_BUFFER bound at the pointer call */
    float value[4];                         /* the constant value when no array */
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
    int depth_test, blend, cull, scissor, alpha_test, texture_2d[ZGL_MAX_UNITS], lighting, color_material, normalize_n, stencil_test, polygon_offset;
    GLenum depth_func; int depth_mask;
    GLenum blend_src, blend_dst, blend_src_a, blend_dst_a, blend_eq; float blend_color[4];
    GLenum cull_mode, front_face, polygon_mode; float line_width, point_size, po_factor, po_units;
    int color_mask;
    GLenum alpha_func; float alpha_ref;
    GLenum shade_model;
    int unpack_alignment, pack_alignment, pack_row_length;
    /* objects */
    struct zgl_shader **shaders; int nshaders;
    struct zgl_program **programs; int nprograms;
    struct zgl_buffer **buffers; int nbuffers;
    struct zgl_texture **textures; int ntextures;
    struct zgl_fbo **fbos; int nfbos;
    struct zgl_fbo *fbo;                    /* bound, NULL = the window */
    int rt_w, rt_h;                         /* the current render target's size */
    GLuint next_name;
    struct zgl_program *program;            /* glUseProgram */
    struct zgl_buffer *array_buffer, *element_buffer;
    int active_unit, client_unit;
    struct zgl_texture *unit_tex[ZGL_MAX_UNITS];
    GLenum unit_env[ZGL_MAX_UNITS];
    struct zgl_attrib attribs[ZGL_MAX_ATTRIBS];
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
    int ffp_key;
    /* virgl state objects */
    uint32_t h_blend, h_rs, h_dsa, h_ve;
    uint64_t blend_key, rs_key, dsa_key, ve_key;
    int fb_dirty, vp_dirty;
    virgl_res *scratch, *iscratch; size_t scratch_size, scratch_used, iscratch_used;   /* vertex and index staging */
    uint32_t h_scratch_ve;
    uint32_t stats_draws;
};

extern struct zgl_ctx *zgl;                 /* the current context */

void zgl_set_error(GLenum e);
struct zgl_program *zgl_ffp_program(void);  /* ffp.c: the program for the current fixed-function state */
void zgl_ffp_fill_state(struct zgl_program *p);   /* fill gl_* state uniforms */
void zgl_ffp_init(struct zgl_ctx *c);
void zgl_ffp_bind_client_arrays(int enable);
void zgl_draw(GLenum mode, GLint first, GLsizei count, GLenum index_type, const void *indices);   /* draw.c */
void zgl_program_upload(struct zgl_program *p);
int  zgl_link(struct zgl_program *p);
/* Store `count` elements of a uniform from `data`: 32-bit words already in the
 * uniform's representation (floats, int/uint bits, bools as ~0/0). */
void zgl_uniform_set(struct zgl_program *p, int loc, const void *data, int count, int is_matrix_cols);
uint32_t zgl_texture_view(struct zgl_texture *t);
uint32_t zgl_texture_sampler(struct zgl_texture *t);
void zgl_flush_scratch(void);
