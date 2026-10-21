/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The interface types of compiled shaders (enum zgl_type): their names,
 * shapes and, for samplers, the TGSI texture target and returned type. */
#include "zgl.h"

static const struct {
    const char *name;
    enum zgl_type base;
    unsigned char vec, cols;
    const char *target;             /* samplers */
} info[T_TYPE_COUNT] = {
    [T_VOID]   = { "void", T_VOID, 0, 0, 0 },
    [T_FLOAT]  = { "float", T_FLOAT, 1, 0, 0 }, [T_VEC2] = { "vec2", T_FLOAT, 2, 0, 0 },
    [T_VEC3]   = { "vec3", T_FLOAT, 3, 0, 0 },  [T_VEC4] = { "vec4", T_FLOAT, 4, 0, 0 },
    [T_INT]    = { "int", T_INT, 1, 0, 0 },     [T_IVEC2] = { "ivec2", T_INT, 2, 0, 0 },
    [T_IVEC3]  = { "ivec3", T_INT, 3, 0, 0 },   [T_IVEC4] = { "ivec4", T_INT, 4, 0, 0 },
    [T_UINT]   = { "uint", T_UINT, 1, 0, 0 },   [T_UVEC2] = { "uvec2", T_UINT, 2, 0, 0 },
    [T_UVEC3]  = { "uvec3", T_UINT, 3, 0, 0 },  [T_UVEC4] = { "uvec4", T_UINT, 4, 0, 0 },
    [T_BOOL]   = { "bool", T_BOOL, 1, 0, 0 },   [T_BVEC2] = { "bvec2", T_BOOL, 2, 0, 0 },
    [T_BVEC3]  = { "bvec3", T_BOOL, 3, 0, 0 },  [T_BVEC4] = { "bvec4", T_BOOL, 4, 0, 0 },
    [T_MAT2]   = { "mat2", T_FLOAT, 2, 2, 0 },  [T_MAT3] = { "mat3", T_FLOAT, 3, 3, 0 }, [T_MAT4] = { "mat4", T_FLOAT, 4, 4, 0 },
    [T_MAT2X3] = { "mat2x3", T_FLOAT, 3, 2, 0 }, [T_MAT2X4] = { "mat2x4", T_FLOAT, 4, 2, 0 },
    [T_MAT3X2] = { "mat3x2", T_FLOAT, 2, 3, 0 }, [T_MAT3X4] = { "mat3x4", T_FLOAT, 4, 3, 0 },
    [T_MAT4X2] = { "mat4x2", T_FLOAT, 2, 4, 0 }, [T_MAT4X3] = { "mat4x3", T_FLOAT, 3, 4, 0 },
    [T_STRUCT] = { "struct", T_VOID, 0, 0, 0 },
    [T_SAMPLER1D] = { "sampler1D", T_FLOAT, 4, 0, "1D" },               [T_SAMPLER2D] = { "sampler2D", T_FLOAT, 4, 0, "2D" },
    [T_SAMPLER3D] = { "sampler3D", T_FLOAT, 4, 0, "3D" },               [T_SAMPLERCUBE] = { "samplerCube", T_FLOAT, 4, 0, "CUBE" },
    [T_SAMPLER2DRECT] = { "sampler2DRect", T_FLOAT, 4, 0, "RECT" },     [T_SAMPLER1DARRAY] = { "sampler1DArray", T_FLOAT, 4, 0, "1D_ARRAY" },
    [T_SAMPLER2DARRAY] = { "sampler2DArray", T_FLOAT, 4, 0, "2D_ARRAY" }, [T_SAMPLERBUFFER] = { "samplerBuffer", T_FLOAT, 4, 0, "BUFFER" },
    [T_SAMPLER2DMS] = { "sampler2DMS", T_FLOAT, 4, 0, "2D_MSAA" },      [T_SAMPLER2DMSARRAY] = { "sampler2DMSArray", T_FLOAT, 4, 0, "2D_ARRAY_MSAA" },
    [T_SAMPLERCUBEARRAY] = { "samplerCubeArray", T_FLOAT, 4, 0, "CUBE_ARRAY" },
    [T_SAMPLER1DSHADOW] = { "sampler1DShadow", T_FLOAT, 4, 0, "SHADOW1D" }, [T_SAMPLER2DSHADOW] = { "sampler2DShadow", T_FLOAT, 4, 0, "SHADOW2D" },
    [T_SAMPLERCUBESHADOW] = { "samplerCubeShadow", T_FLOAT, 4, 0, "SHADOWCUBE" }, [T_SAMPLER2DRECTSHADOW] = { "sampler2DRectShadow", T_FLOAT, 4, 0, "SHADOWRECT" },
    [T_SAMPLER1DARRAYSHADOW] = { "sampler1DArrayShadow", T_FLOAT, 4, 0, "SHADOW1D_ARRAY" },
    [T_SAMPLER2DARRAYSHADOW] = { "sampler2DArrayShadow", T_FLOAT, 4, 0, "SHADOW2D_ARRAY" },
    [T_SAMPLERCUBEARRAYSHADOW] = { "samplerCubeArrayShadow", T_FLOAT, 4, 0, "SHADOWCUBE_ARRAY" },
    [T_ISAMPLER1D] = { "isampler1D", T_INT, 4, 0, "1D" },               [T_ISAMPLER2D] = { "isampler2D", T_INT, 4, 0, "2D" },
    [T_ISAMPLER3D] = { "isampler3D", T_INT, 4, 0, "3D" },               [T_ISAMPLERCUBE] = { "isamplerCube", T_INT, 4, 0, "CUBE" },
    [T_ISAMPLER2DRECT] = { "isampler2DRect", T_INT, 4, 0, "RECT" },     [T_ISAMPLER1DARRAY] = { "isampler1DArray", T_INT, 4, 0, "1D_ARRAY" },
    [T_ISAMPLER2DARRAY] = { "isampler2DArray", T_INT, 4, 0, "2D_ARRAY" }, [T_ISAMPLERBUFFER] = { "isamplerBuffer", T_INT, 4, 0, "BUFFER" },
    [T_ISAMPLER2DMS] = { "isampler2DMS", T_INT, 4, 0, "2D_MSAA" },      [T_ISAMPLER2DMSARRAY] = { "isampler2DMSArray", T_INT, 4, 0, "2D_ARRAY_MSAA" },
    [T_ISAMPLERCUBEARRAY] = { "isamplerCubeArray", T_INT, 4, 0, "CUBE_ARRAY" },
    [T_USAMPLER1D] = { "usampler1D", T_UINT, 4, 0, "1D" },              [T_USAMPLER2D] = { "usampler2D", T_UINT, 4, 0, "2D" },
    [T_USAMPLER3D] = { "usampler3D", T_UINT, 4, 0, "3D" },              [T_USAMPLERCUBE] = { "usamplerCube", T_UINT, 4, 0, "CUBE" },
    [T_USAMPLER2DRECT] = { "usampler2DRect", T_UINT, 4, 0, "RECT" },    [T_USAMPLER1DARRAY] = { "usampler1DArray", T_UINT, 4, 0, "1D_ARRAY" },
    [T_USAMPLER2DARRAY] = { "usampler2DArray", T_UINT, 4, 0, "2D_ARRAY" }, [T_USAMPLERBUFFER] = { "usamplerBuffer", T_UINT, 4, 0, "BUFFER" },
    [T_USAMPLER2DMS] = { "usampler2DMS", T_UINT, 4, 0, "2D_MSAA" },     [T_USAMPLER2DMSARRAY] = { "usampler2DMSArray", T_UINT, 4, 0, "2D_ARRAY_MSAA" },
    [T_USAMPLERCUBEARRAY] = { "usamplerCubeArray", T_UINT, 4, 0, "CUBE_ARRAY" },
};

static int ok(enum zgl_type t) { return (unsigned)t < T_TYPE_COUNT && info[t].name; }

enum zgl_type zgl_type_base(enum zgl_type t) { return ok(t) ? info[t].base : T_VOID; }
int zgl_type_vec(enum zgl_type t) { return ok(t) ? info[t].vec : 0; }
int zgl_type_cols(enum zgl_type t) { return ok(t) ? info[t].cols : 0; }
int zgl_type_is_sampler(enum zgl_type t) { return ok(t) && info[t].target != 0; }
int zgl_type_slots(enum zgl_type t) { return ok(t) && info[t].cols ? info[t].cols : 1; }
const char *zgl_type_name(enum zgl_type t) { return ok(t) ? info[t].name : "?"; }
const char *zgl_type_tgsi_target(enum zgl_type t) { return ok(t) && info[t].target ? info[t].target : "2D"; }
