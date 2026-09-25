/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* A stand-in for GLEW on sic: there is one OpenGL here (libzgl, linked
 * statically), so nothing has to be looked up at run time. Programs written
 * against GLEW's ARB/EXT-suffixed names (Ren'Py) get the core functions. */
#ifndef SIC_GLEW_H
#define SIC_GLEW_H
#include <GL/gl.h>

#define GLEW_OK 0
#define GLEW_VERSION 1
#define GLEW_VERSION_1_1 1
#define GLEW_VERSION_1_2 1
#define GLEW_VERSION_1_3 1
#define GLEW_VERSION_1_4 1
#define GLEW_VERSION_1_5 1
#define GLEW_VERSION_2_0 1
#define GLEW_VERSION_2_1 1
#define GLEW_ARB_multitexture 1
#define GLEW_ARB_vertex_buffer_object 1
#define GLEW_ARB_shader_objects 1
#define GLEW_ARB_vertex_shader 1
#define GLEW_ARB_fragment_shader 1
#define GLEW_ARB_texture_non_power_of_two 1
#define GLEW_EXT_framebuffer_object 1
static inline GLenum glewInit(void) { return GLEW_OK; }
static inline const GLubyte *glewGetErrorString(GLenum e) { (void)e; return (const GLubyte *)"no error"; }
static inline GLboolean glewIsSupported(const char *name) { (void)name; return 1; }
static inline const GLubyte *glewGetString(GLenum e) { (void)e; return (const GLubyte *)"zgl"; }

typedef GLuint GLhandleARB;
typedef char GLcharARB;

/* GL_ARB_shader_objects / vertex_shader / fragment_shader as the core 2.0 API */
#define GL_OBJECT_COMPILE_STATUS_ARB 0x8B81
#define GL_OBJECT_LINK_STATUS_ARB 0x8B82
#define GL_OBJECT_INFO_LOG_LENGTH_ARB 0x8B84   /* literal: glcompat.h redefines the core names as these */
#define GL_OBJECT_ACTIVE_UNIFORMS_ARB 0x8B86
#define GL_OBJECT_ACTIVE_ATTRIBUTES_ARB 0x8B89
#define GL_VERTEX_SHADER_ARB GL_VERTEX_SHADER
#define GL_FRAGMENT_SHADER_ARB GL_FRAGMENT_SHADER
#define GL_PROGRAM_OBJECT_ARB 0x8B40
#define GL_SHADER_OBJECT_ARB 0x8B48
#define glCreateShaderObjectARB glCreateShader
#define glShaderSourceARB glShaderSource
#define glCompileShaderARB glCompileShader
#define glCreateProgramObjectARB glCreateProgram
#define glAttachObjectARB glAttachShader
#define glDetachObjectARB glDetachShader
#define glLinkProgramARB glLinkProgram
#define glUseProgramObjectARB glUseProgram
#define glValidateProgramARB glValidateProgram
#define glGetAttribLocationARB glGetAttribLocation
#define glBindAttribLocationARB glBindAttribLocation
#define glGetUniformLocationARB glGetUniformLocation
#define glUniform1fARB glUniform1f
#define glUniform2fARB glUniform2f
#define glUniform3fARB glUniform3f
#define glUniform4fARB glUniform4f
#define glUniform1iARB glUniform1i
#define glUniform2iARB glUniform2i
#define glUniform3iARB glUniform3i
#define glUniform4iARB glUniform4i
#define glUniform1fvARB glUniform1fv
#define glUniform2fvARB glUniform2fv
#define glUniform3fvARB glUniform3fv
#define glUniform4fvARB glUniform4fv
#define glUniform1ivARB glUniform1iv
#define glUniformMatrix2fvARB glUniformMatrix2fv
#define glUniformMatrix3fvARB glUniformMatrix3fv
#define glUniformMatrix4fvARB glUniformMatrix4fv
#define glVertexAttribPointerARB glVertexAttribPointer
#define glEnableVertexAttribArrayARB glEnableVertexAttribArray
#define glDisableVertexAttribArrayARB glDisableVertexAttribArray
#define glGetInfoLogARB zgl_GetInfoLogARB
#define glGetObjectParameterivARB zgl_GetObjectParameterivARB
#define glDeleteObjectARB zgl_DeleteObjectARB
GLAPI void GLAPIENTRY zgl_GetInfoLogARB(GLuint obj, GLsizei maxlen, GLsizei *len, char *log);
GLAPI void GLAPIENTRY zgl_GetObjectParameterivARB(GLuint obj, GLenum pname, GLint *v);
GLAPI void GLAPIENTRY zgl_DeleteObjectARB(GLuint obj);
/* GL_ARB_multitexture */
#define glActiveTextureARB glActiveTexture
#define glClientActiveTextureARB glClientActiveTexture
#define GL_TEXTURE0_ARB GL_TEXTURE0
#define GL_TEXTURE1_ARB GL_TEXTURE1
#define GL_TEXTURE2_ARB GL_TEXTURE2
#define GL_TEXTURE3_ARB GL_TEXTURE3
#define GL_MAX_TEXTURE_UNITS_ARB GL_MAX_TEXTURE_UNITS
/* GL_ARB_vertex_buffer_object */
#define glBindBufferARB glBindBuffer
#define glGenBuffersARB glGenBuffers
#define glDeleteBuffersARB glDeleteBuffers
#define glBufferDataARB glBufferData
#define glBufferSubDataARB glBufferSubData
#define GL_ARRAY_BUFFER_ARB GL_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER_ARB GL_ELEMENT_ARRAY_BUFFER
#define GL_STATIC_DRAW_ARB GL_STATIC_DRAW
#define GL_DYNAMIC_DRAW_ARB GL_DYNAMIC_DRAW
/* GL_EXT_framebuffer_object */
#define GL_FRAMEBUFFER_EXT GL_FRAMEBUFFER
#define GL_COLOR_ATTACHMENT0_EXT GL_COLOR_ATTACHMENT0
#define GL_FRAMEBUFFER_COMPLETE_EXT GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_BINDING_EXT GL_FRAMEBUFFER_BINDING
#define glGenFramebuffersEXT glGenFramebuffers
#define glDeleteFramebuffersEXT glDeleteFramebuffers
#define glBindFramebufferEXT glBindFramebuffer
#define glFramebufferTexture2DEXT glFramebufferTexture2D
#define glCheckFramebufferStatusEXT glCheckFramebufferStatus
#define glIsFramebufferEXT glIsFramebuffer
#define glGenerateMipmapEXT glGenerateMipmap
/* GL_ARB_texture_env_combine and friends: the core 1.3 names */
#define GL_COMBINE_ARB GL_COMBINE
#define GL_COMBINE_RGB_ARB GL_COMBINE_RGB
#define GL_COMBINE_ALPHA_ARB GL_COMBINE_ALPHA
#define GL_SOURCE0_RGB_ARB GL_SOURCE0_RGB
#define GL_SOURCE1_RGB_ARB GL_SOURCE1_RGB
#define GL_SOURCE2_RGB_ARB GL_SOURCE2_RGB
#define GL_SOURCE0_ALPHA_ARB GL_SOURCE0_ALPHA
#define GL_SOURCE1_ALPHA_ARB GL_SOURCE1_ALPHA
#define GL_SOURCE2_ALPHA_ARB GL_SOURCE2_ALPHA
#define GL_OPERAND0_RGB_ARB GL_OPERAND0_RGB
#define GL_OPERAND1_RGB_ARB GL_OPERAND1_RGB
#define GL_OPERAND2_RGB_ARB GL_OPERAND2_RGB
#define GL_OPERAND0_ALPHA_ARB GL_OPERAND0_ALPHA
#define GL_OPERAND1_ALPHA_ARB GL_OPERAND1_ALPHA
#define GL_OPERAND2_ALPHA_ARB GL_OPERAND2_ALPHA
#define GL_RGB_SCALE_ARB GL_RGB_SCALE
#define GL_PREVIOUS_ARB GL_PREVIOUS
#define GL_CONSTANT_ARB GL_CONSTANT
#define GL_PRIMARY_COLOR_ARB GL_PRIMARY_COLOR
#define GL_INTERPOLATE_ARB GL_INTERPOLATE
#define GL_SUBTRACT_ARB GL_SUBTRACT

#endif
