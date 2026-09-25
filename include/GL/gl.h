/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/*
 * OpenGL for sic: libzgl. The OpenGL 2.1 API (shaders, buffers, textures,
 * the fixed-function pipeline of 1.x that 2.1 still carries) implemented
 * on libvirgl, so the host GPU does the work. Constants have their
 * standard values; only what is implemented is declared.
 */
#ifndef ZGL_GL_H
#define ZGL_GL_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GLAPI
#define GLAPIENTRY
#define APIENTRY
#define GL_VERSION_1_1 1
#define GL_VERSION_1_2 1
#define GL_VERSION_1_3 1
#define GL_VERSION_1_4 1
#define GL_VERSION_1_5 1
#define GL_VERSION_2_0 1
#define GL_VERSION_2_1 1

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW 0x0503
#define GL_STACK_UNDERFLOW 0x0504
#define GL_OUT_OF_MEMORY 0x0505

/* primitives */
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_LOOP 0x0002
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_QUADS 0x0007
#define GL_QUAD_STRIP 0x0008
#define GL_POLYGON 0x0009

/* types */
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_DOUBLE 0x140A

/* comparison, blend, cull */
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_CONSTANT_COLOR 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_POINT 0x1B00
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02

/* enables */
#define GL_CULL_FACE 0x0B44
#define GL_LIGHTING 0x0B50
#define GL_LIGHT0 0x4000
#define GL_LIGHT1 0x4001
#define GL_LIGHT2 0x4002
#define GL_LIGHT3 0x4003
#define GL_LIGHT4 0x4004
#define GL_LIGHT5 0x4005
#define GL_LIGHT6 0x4006
#define GL_LIGHT7 0x4007
#define GL_COLOR_MATERIAL 0x0B57
#define GL_FOG 0x0B60
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_NORMALIZE 0x0BA1
#define GL_ALPHA_TEST 0x0BC0
#define GL_DITHER 0x0BD0
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_TEXTURE_2D 0x0DE1
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_LINE_SMOOTH 0x0B20
#define GL_POINT_SMOOTH 0x0B10
#define GL_MULTISAMPLE 0x809D
#define GL_RESCALE_NORMAL 0x803A

/* gets */
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_VIEWPORT 0x0BA2
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS 0x8B4A
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS 0x8B49
#define GL_MAX_VARYING_FLOATS 0x8B4B
#define GL_MAX_LIGHTS 0x0D31
#define GL_MAX_MODELVIEW_STACK_DEPTH 0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38
#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_DEPTH_BITS 0x0D56
#define GL_STENCIL_BITS 0x0D57
#define GL_RED_BITS 0x0D52
#define GL_GREEN_BITS 0x0D53
#define GL_BLUE_BITS 0x0D54
#define GL_ALPHA_BITS 0x0D55
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_MAX_TEXTURE_UNITS 0x84E2
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_UNSUPPORTED 0x8CDD
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506
#define GL_CLIP_PLANE0 0x3000
#define GL_CLIP_PLANE1 0x3001
#define GL_CLIP_PLANE2 0x3002
#define GL_CLIP_PLANE3 0x3003
#define GL_CLIP_PLANE4 0x3004
#define GL_CLIP_PLANE5 0x3005

/* clear */
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000

/* matrices */
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702

/* lighting & material */
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_EMISSION 0x1600
#define GL_SHININESS 0x1601
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01

/* textures */
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#define GL_TEXTURE4 0x84C4
#define GL_TEXTURE5 0x84C5
#define GL_TEXTURE6 0x84C6
#define GL_TEXTURE7 0x84C7
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP 0x2900
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_ADD 0x0104
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_RGB_SCALE 0x8573
#define GL_ALPHA_SCALE 0x0D1C
#define GL_ADD_SIGNED 0x8574
#define GL_INTERPOLATE 0x8575
#define GL_SUBTRACT 0x84E7
#define GL_CONSTANT 0x8576
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE1_RGB 0x8581
#define GL_SOURCE2_RGB 0x8582
#define GL_SOURCE0_ALPHA 0x8588
#define GL_SOURCE1_ALPHA 0x8589
#define GL_SOURCE2_ALPHA 0x858A
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_OPERAND2_RGB 0x8592
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND1_ALPHA 0x8599
#define GL_OPERAND2_ALPHA 0x859A
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_REPLACE 0x1E01
#define GL_GENERATE_MIPMAP 0x8191
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_BGR 0x80E0
#define GL_BGRA 0x80E1
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_DEPTH_COMPONENT 0x1902

/* buffers */
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_BUFFER_SIZE 0x8764

/* shaders */
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VALIDATE_STATUS 0x8B83
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ATTACHED_SHADERS 0x8B85
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_SHADER_TYPE 0x8B4F
#define GL_DELETE_STATUS 0x8B80
#define GL_FLOAT_VEC2 0x8B50
#define GL_FLOAT_VEC3 0x8B51
#define GL_FLOAT_VEC4 0x8B52
#define GL_INT_VEC2 0x8B53
#define GL_INT_VEC3 0x8B54
#define GL_INT_VEC4 0x8B55
#define GL_BOOL 0x8B56
#define GL_FLOAT_MAT2 0x8B5A
#define GL_FLOAT_MAT3 0x8B5B
#define GL_FLOAT_MAT4 0x8B5C
#define GL_SAMPLER_2D 0x8B5E

/* client arrays (compat) */
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

/* misc */
#define GL_SHADE_MODEL 0x0B54
#define GL_DEPTH_FUNC 0x0B74
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_FRONT_FACE 0x0B46
#define GL_CULL_FACE_MODE 0x0B45
#define GL_POLYGON_OFFSET_FACTOR 0x8038
#define GL_POLYGON_OFFSET_UNITS 0x2A00

/* ---- 1.x / 2.1 entry points ------------------------------------------- */
GLAPI void GLAPIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
GLAPI void GLAPIENTRY glClearDepth(GLclampd d);
GLAPI void GLAPIENTRY glClearStencil(GLint s);
GLAPI void GLAPIENTRY glClear(GLbitfield mask);
GLAPI void GLAPIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
GLAPI void GLAPIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
GLAPI void GLAPIENTRY glEnable(GLenum cap);
GLAPI void GLAPIENTRY glDisable(GLenum cap);
GLAPI GLboolean GLAPIENTRY glIsEnabled(GLenum cap);
GLAPI void GLAPIENTRY glDepthFunc(GLenum func);
GLAPI void GLAPIENTRY glDepthMask(GLboolean flag);
GLAPI void GLAPIENTRY glDepthRange(GLclampd n, GLclampd f);
GLAPI void GLAPIENTRY glBlendFunc(GLenum src, GLenum dst);
GLAPI void GLAPIENTRY glBlendFuncSeparate(GLenum srgb, GLenum drgb, GLenum sa, GLenum da);
GLAPI void GLAPIENTRY glBlendEquation(GLenum mode);
GLAPI void GLAPIENTRY glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
GLAPI void GLAPIENTRY glAlphaFunc(GLenum func, GLclampf ref);
GLAPI void GLAPIENTRY glCullFace(GLenum mode);
GLAPI void GLAPIENTRY glFrontFace(GLenum mode);
GLAPI void GLAPIENTRY glPolygonMode(GLenum face, GLenum mode);
GLAPI void GLAPIENTRY glPolygonOffset(GLfloat factor, GLfloat units);
GLAPI void GLAPIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
GLAPI void GLAPIENTRY glLineWidth(GLfloat w);
GLAPI void GLAPIENTRY glPointSize(GLfloat s);
GLAPI void GLAPIENTRY glShadeModel(GLenum mode);
GLAPI void GLAPIENTRY glHint(GLenum target, GLenum mode);
GLAPI void GLAPIENTRY glPixelStorei(GLenum pname, GLint param);
GLAPI void GLAPIENTRY glFlush(void);
GLAPI void GLAPIENTRY glFinish(void);
GLAPI GLenum GLAPIENTRY glGetError(void);
GLAPI const GLubyte *GLAPIENTRY glGetString(GLenum name);
GLAPI void GLAPIENTRY glGetIntegerv(GLenum pname, GLint *v);
GLAPI void GLAPIENTRY glGetFloatv(GLenum pname, GLfloat *v);
GLAPI void GLAPIENTRY glGetDoublev(GLenum pname, GLdouble *v);
GLAPI void GLAPIENTRY glGetBooleanv(GLenum pname, GLboolean *v);
GLAPI void GLAPIENTRY glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels);

/* matrices and the fixed-function pipeline */
GLAPI void GLAPIENTRY glMatrixMode(GLenum mode);
GLAPI void GLAPIENTRY glLoadIdentity(void);
GLAPI void GLAPIENTRY glLoadMatrixf(const GLfloat *m);
GLAPI void GLAPIENTRY glLoadMatrixd(const GLdouble *m);
GLAPI void GLAPIENTRY glMultMatrixf(const GLfloat *m);
GLAPI void GLAPIENTRY glMultMatrixd(const GLdouble *m);
GLAPI void GLAPIENTRY glPushMatrix(void);
GLAPI void GLAPIENTRY glPopMatrix(void);
GLAPI void GLAPIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z);
GLAPI void GLAPIENTRY glTranslated(GLdouble x, GLdouble y, GLdouble z);
GLAPI void GLAPIENTRY glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z);
GLAPI void GLAPIENTRY glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z);
GLAPI void GLAPIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z);
GLAPI void GLAPIENTRY glScaled(GLdouble x, GLdouble y, GLdouble z);
GLAPI void GLAPIENTRY glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
GLAPI void GLAPIENTRY glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
GLAPI void GLAPIENTRY glBegin(GLenum mode);
GLAPI void GLAPIENTRY glEnd(void);
GLAPI void GLAPIENTRY glVertex2f(GLfloat x, GLfloat y);
GLAPI void GLAPIENTRY glVertex2i(GLint x, GLint y);
GLAPI void GLAPIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z);
GLAPI void GLAPIENTRY glVertex3fv(const GLfloat *v);
GLAPI void GLAPIENTRY glVertex3d(GLdouble x, GLdouble y, GLdouble z);
GLAPI void GLAPIENTRY glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
GLAPI void GLAPIENTRY glColor3f(GLfloat r, GLfloat g, GLfloat b);
GLAPI void GLAPIENTRY glColor3fv(const GLfloat *v);
GLAPI void GLAPIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
GLAPI void GLAPIENTRY glColor4fv(const GLfloat *v);
GLAPI void GLAPIENTRY glColor3ub(GLubyte r, GLubyte g, GLubyte b);
GLAPI void GLAPIENTRY glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
GLAPI void GLAPIENTRY glNormal3f(GLfloat x, GLfloat y, GLfloat z);
GLAPI void GLAPIENTRY glNormal3fv(const GLfloat *v);
GLAPI void GLAPIENTRY glTexCoord2f(GLfloat s, GLfloat t);
GLAPI void GLAPIENTRY glTexCoord2fv(const GLfloat *v);
GLAPI void GLAPIENTRY glLightfv(GLenum light, GLenum pname, const GLfloat *params);
GLAPI void GLAPIENTRY glLightf(GLenum light, GLenum pname, GLfloat param);
GLAPI void GLAPIENTRY glLightModelfv(GLenum pname, const GLfloat *params);
GLAPI void GLAPIENTRY glLightModeli(GLenum pname, GLint param);
GLAPI void GLAPIENTRY glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
GLAPI void GLAPIENTRY glMaterialf(GLenum face, GLenum pname, GLfloat param);
GLAPI void GLAPIENTRY glColorMaterial(GLenum face, GLenum mode);
GLAPI void GLAPIENTRY glTexEnvi(GLenum target, GLenum pname, GLint param);
GLAPI void GLAPIENTRY glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
GLAPI void GLAPIENTRY glClientActiveTexture(GLenum unit);        /* unit 0 has the texcoord array; the others are accepted */
GLAPI void GLAPIENTRY glTexEnvf(GLenum target, GLenum pname, GLfloat param);
GLAPI void GLAPIENTRY glEnableClientState(GLenum cap);
GLAPI void GLAPIENTRY glDisableClientState(GLenum cap);
GLAPI void GLAPIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
GLAPI void GLAPIENTRY glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p);
GLAPI void GLAPIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);
GLAPI void GLAPIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p);

/* textures */
GLAPI void GLAPIENTRY glGenTextures(GLsizei n, GLuint *t);
GLAPI void GLAPIENTRY glDeleteTextures(GLsizei n, const GLuint *t);
GLAPI void GLAPIENTRY glBindTexture(GLenum target, GLuint t);
GLAPI GLboolean GLAPIENTRY glIsTexture(GLuint t);
GLAPI void GLAPIENTRY glActiveTexture(GLenum unit);
GLAPI void GLAPIENTRY glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
GLAPI void GLAPIENTRY glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels);
GLAPI void GLAPIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param);
GLAPI void GLAPIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat param);
GLAPI void GLAPIENTRY glGenerateMipmap(GLenum target);
GLAPI void GLAPIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
/* framebuffer objects (GL_EXT_framebuffer_object, colour attachment 0 only) */
GLAPI void GLAPIENTRY glGenFramebuffers(GLsizei n, GLuint *ids);
GLAPI void GLAPIENTRY glDeleteFramebuffers(GLsizei n, const GLuint *ids);
GLAPI void GLAPIENTRY glBindFramebuffer(GLenum target, GLuint id);
GLAPI void GLAPIENTRY glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
GLAPI GLenum GLAPIENTRY glCheckFramebufferStatus(GLenum target);
GLAPI GLboolean GLAPIENTRY glIsFramebuffer(GLuint id);
GLAPI void GLAPIENTRY glClipPlane(GLenum plane, const GLdouble *equation);   /* accepted, not applied */

/* buffers */
GLAPI void GLAPIENTRY glGenBuffers(GLsizei n, GLuint *b);
GLAPI void GLAPIENTRY glDeleteBuffers(GLsizei n, const GLuint *b);
GLAPI void GLAPIENTRY glBindBuffer(GLenum target, GLuint b);
GLAPI void GLAPIENTRY glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
GLAPI void GLAPIENTRY glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const GLvoid *data);

/* shaders and programs */
GLAPI GLuint GLAPIENTRY glCreateShader(GLenum type);
GLAPI void GLAPIENTRY glDeleteShader(GLuint s);
GLAPI void GLAPIENTRY glShaderSource(GLuint s, GLsizei count, const GLchar *const *strings, const GLint *lengths);
GLAPI void GLAPIENTRY glCompileShader(GLuint s);
GLAPI void GLAPIENTRY glGetShaderiv(GLuint s, GLenum pname, GLint *v);
GLAPI void GLAPIENTRY glGetShaderInfoLog(GLuint s, GLsizei max, GLsizei *len, GLchar *log);
GLAPI GLuint GLAPIENTRY glCreateProgram(void);
GLAPI void GLAPIENTRY glDeleteProgram(GLuint p);
GLAPI void GLAPIENTRY glAttachShader(GLuint p, GLuint s);
GLAPI void GLAPIENTRY glDetachShader(GLuint p, GLuint s);
GLAPI void GLAPIENTRY glLinkProgram(GLuint p);
GLAPI void GLAPIENTRY glUseProgram(GLuint p);
GLAPI void GLAPIENTRY glGetProgramiv(GLuint p, GLenum pname, GLint *v);
GLAPI void GLAPIENTRY glGetProgramInfoLog(GLuint p, GLsizei max, GLsizei *len, GLchar *log);
GLAPI void GLAPIENTRY glValidateProgram(GLuint p);
GLAPI GLint GLAPIENTRY glGetAttribLocation(GLuint p, const GLchar *name);
GLAPI void GLAPIENTRY glBindAttribLocation(GLuint p, GLuint index, const GLchar *name);
GLAPI GLint GLAPIENTRY glGetUniformLocation(GLuint p, const GLchar *name);
GLAPI void GLAPIENTRY glUniform1f(GLint loc, GLfloat x);
GLAPI void GLAPIENTRY glUniform2f(GLint loc, GLfloat x, GLfloat y);
GLAPI void GLAPIENTRY glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z);
GLAPI void GLAPIENTRY glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
GLAPI void GLAPIENTRY glUniform1i(GLint loc, GLint x);
GLAPI void GLAPIENTRY glUniform2i(GLint loc, GLint x, GLint y);
GLAPI void GLAPIENTRY glUniform3i(GLint loc, GLint x, GLint y, GLint z);
GLAPI void GLAPIENTRY glUniform4i(GLint loc, GLint x, GLint y, GLint z, GLint w);
GLAPI void GLAPIENTRY glUniform1fv(GLint loc, GLsizei n, const GLfloat *v);
GLAPI void GLAPIENTRY glUniform2fv(GLint loc, GLsizei n, const GLfloat *v);
GLAPI void GLAPIENTRY glUniform3fv(GLint loc, GLsizei n, const GLfloat *v);
GLAPI void GLAPIENTRY glUniform4fv(GLint loc, GLsizei n, const GLfloat *v);
GLAPI void GLAPIENTRY glUniform1iv(GLint loc, GLsizei n, const GLint *v);
GLAPI void GLAPIENTRY glUniformMatrix2fv(GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v);
GLAPI void GLAPIENTRY glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v);
GLAPI void GLAPIENTRY glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v);
GLAPI void GLAPIENTRY glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *p);
GLAPI void GLAPIENTRY glEnableVertexAttribArray(GLuint index);
GLAPI void GLAPIENTRY glDisableVertexAttribArray(GLuint index);
GLAPI void GLAPIENTRY glVertexAttrib1f(GLuint index, GLfloat x);
GLAPI void GLAPIENTRY glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y);
GLAPI void GLAPIENTRY glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
GLAPI void GLAPIENTRY glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
GLAPI void GLAPIENTRY glVertexAttrib4fv(GLuint index, const GLfloat *v);
GLAPI void GLAPIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count);
GLAPI void GLAPIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

#include "sic_gl.h"           /* the context, for the window system glue */

#ifdef __cplusplus
}
#endif
#endif
