/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* The OpenGL 1.0-3.3 functions libzgl has beyond the 2.1 set in gl.h, with the
 * registry's signatures. Included by GL/gl.h. */
#pragma once

/* 1.0/1.1 */
GLAPI void GLAPIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask);
GLAPI void GLAPIENTRY glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
GLAPI void GLAPIENTRY glStencilMask(GLuint mask);
GLAPI void GLAPIENTRY glLogicOp(GLenum opcode);
GLAPI void GLAPIENTRY glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format, GLenum type, const void *pixels);
GLAPI void GLAPIENTRY glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const void *pixels);
GLAPI void GLAPIENTRY glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border);
GLAPI void GLAPIENTRY glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
GLAPI void GLAPIENTRY glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
GLAPI void GLAPIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *pixels);
GLAPI void GLAPIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params);
GLAPI void GLAPIENTRY glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params);
GLAPI void GLAPIENTRY glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
GLAPI void GLAPIENTRY glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
GLAPI void GLAPIENTRY glDrawBuffer(GLenum buf);
GLAPI void GLAPIENTRY glReadBuffer(GLenum src);
GLAPI void GLAPIENTRY glPixelStoref(GLenum pname, GLfloat param);
GLAPI void GLAPIENTRY glGetPointerv(GLenum pname, void **params);

/* 1.2 */
GLAPI void GLAPIENTRY glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices);
GLAPI void GLAPIENTRY glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels);
GLAPI void GLAPIENTRY glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void *pixels);
GLAPI void GLAPIENTRY glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height);

/* 1.3 */
GLAPI void GLAPIENTRY glSampleCoverage(GLfloat value, GLboolean invert);
GLAPI void GLAPIENTRY glCompressedTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border, GLsizei imageSize, const void *data);
GLAPI void GLAPIENTRY glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data);
GLAPI void GLAPIENTRY glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void *data);
GLAPI void GLAPIENTRY glCompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void *data);
GLAPI void GLAPIENTRY glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void *data);
GLAPI void GLAPIENTRY glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void *data);
GLAPI void GLAPIENTRY glGetCompressedTexImage(GLenum target, GLint level, void *img);

/* 1.4 */
GLAPI void GLAPIENTRY glMultiDrawArrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei drawcount);
GLAPI void GLAPIENTRY glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type, const void *const*indices, GLsizei drawcount);
GLAPI void GLAPIENTRY glPointParameterf(GLenum pname, GLfloat param);
GLAPI void GLAPIENTRY glPointParameterfv(GLenum pname, const GLfloat *params);
GLAPI void GLAPIENTRY glPointParameteri(GLenum pname, GLint param);
GLAPI void GLAPIENTRY glPointParameteriv(GLenum pname, const GLint *params);

/* 1.5 */
GLAPI void GLAPIENTRY glGenQueries(GLsizei n, GLuint *ids);
GLAPI void GLAPIENTRY glDeleteQueries(GLsizei n, const GLuint *ids);
GLAPI GLboolean GLAPIENTRY glIsQuery(GLuint id);
GLAPI void GLAPIENTRY glBeginQuery(GLenum target, GLuint id);
GLAPI void GLAPIENTRY glEndQuery(GLenum target);
GLAPI void GLAPIENTRY glGetQueryiv(GLenum target, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params);
GLAPI GLboolean GLAPIENTRY glIsBuffer(GLuint buffer);
GLAPI void GLAPIENTRY glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void *data);
GLAPI void * GLAPIENTRY glMapBuffer(GLenum target, GLenum access);
GLAPI GLboolean GLAPIENTRY glUnmapBuffer(GLenum target);
GLAPI void GLAPIENTRY glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetBufferPointerv(GLenum target, GLenum pname, void **params);

/* 2.0 */
GLAPI void GLAPIENTRY glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
GLAPI void GLAPIENTRY glDrawBuffers(GLsizei n, const GLenum *bufs);
GLAPI void GLAPIENTRY glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
GLAPI void GLAPIENTRY glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
GLAPI void GLAPIENTRY glStencilMaskSeparate(GLenum face, GLuint mask);
GLAPI void GLAPIENTRY glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
GLAPI void GLAPIENTRY glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
GLAPI void GLAPIENTRY glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders);
GLAPI void GLAPIENTRY glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source);
GLAPI void GLAPIENTRY glGetUniformfv(GLuint program, GLint location, GLfloat *params);
GLAPI void GLAPIENTRY glGetUniformiv(GLuint program, GLint location, GLint *params);
GLAPI void GLAPIENTRY glGetVertexAttribdv(GLuint index, GLenum pname, GLdouble *params);
GLAPI void GLAPIENTRY glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params);
GLAPI void GLAPIENTRY glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer);
GLAPI GLboolean GLAPIENTRY glIsProgram(GLuint program);
GLAPI GLboolean GLAPIENTRY glIsShader(GLuint shader);
GLAPI void GLAPIENTRY glUniform2iv(GLint location, GLsizei count, const GLint *value);
GLAPI void GLAPIENTRY glUniform3iv(GLint location, GLsizei count, const GLint *value);
GLAPI void GLAPIENTRY glUniform4iv(GLint location, GLsizei count, const GLint *value);
GLAPI void GLAPIENTRY glVertexAttrib1fv(GLuint index, const GLfloat *v);
GLAPI void GLAPIENTRY glVertexAttrib2fv(GLuint index, const GLfloat *v);
GLAPI void GLAPIENTRY glVertexAttrib3fv(GLuint index, const GLfloat *v);
GLAPI void GLAPIENTRY glVertexAttrib1d(GLuint index, GLdouble x);
GLAPI void GLAPIENTRY glVertexAttrib2d(GLuint index, GLdouble x, GLdouble y);
GLAPI void GLAPIENTRY glVertexAttrib3d(GLuint index, GLdouble x, GLdouble y, GLdouble z);
GLAPI void GLAPIENTRY glVertexAttrib4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
GLAPI void GLAPIENTRY glVertexAttrib4Nub(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w);
GLAPI void GLAPIENTRY glVertexAttrib4Nubv(GLuint index, const GLubyte *v);
GLAPI void GLAPIENTRY glVertexAttrib1s(GLuint index, GLshort x);
GLAPI void GLAPIENTRY glVertexAttrib2s(GLuint index, GLshort x, GLshort y);
GLAPI void GLAPIENTRY glVertexAttrib3s(GLuint index, GLshort x, GLshort y, GLshort z);
GLAPI void GLAPIENTRY glVertexAttrib4s(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w);

/* 2.1 */
GLAPI void GLAPIENTRY glUniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLAPI void GLAPIENTRY glUniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLAPI void GLAPIENTRY glUniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLAPI void GLAPIENTRY glUniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLAPI void GLAPIENTRY glUniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
GLAPI void GLAPIENTRY glUniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

/* 3.0 */
GLAPI void GLAPIENTRY glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a);
GLAPI void GLAPIENTRY glGetBooleani_v(GLenum target, GLuint index, GLboolean *data);
GLAPI void GLAPIENTRY glGetIntegeri_v(GLenum target, GLuint index, GLint *data);
GLAPI void GLAPIENTRY glEnablei(GLenum target, GLuint index);
GLAPI void GLAPIENTRY glDisablei(GLenum target, GLuint index);
GLAPI GLboolean GLAPIENTRY glIsEnabledi(GLenum target, GLuint index);
GLAPI void GLAPIENTRY glBeginTransformFeedback(GLenum primitiveMode);
GLAPI void GLAPIENTRY glEndTransformFeedback(void);
GLAPI void GLAPIENTRY glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
GLAPI void GLAPIENTRY glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
GLAPI void GLAPIENTRY glTransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar *const*varyings, GLenum bufferMode);
GLAPI void GLAPIENTRY glGetTransformFeedbackVarying(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLsizei *size, GLenum *type, GLchar *name);
GLAPI void GLAPIENTRY glClampColor(GLenum target, GLenum clamp);
GLAPI void GLAPIENTRY glBeginConditionalRender(GLuint id, GLenum mode);
GLAPI void GLAPIENTRY glEndConditionalRender(void);
GLAPI void GLAPIENTRY glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer);
GLAPI void GLAPIENTRY glGetVertexAttribIiv(GLuint index, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetVertexAttribIuiv(GLuint index, GLenum pname, GLuint *params);
GLAPI void GLAPIENTRY glVertexAttribI1i(GLuint index, GLint x);
GLAPI void GLAPIENTRY glVertexAttribI2i(GLuint index, GLint x, GLint y);
GLAPI void GLAPIENTRY glVertexAttribI3i(GLuint index, GLint x, GLint y, GLint z);
GLAPI void GLAPIENTRY glVertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w);
GLAPI void GLAPIENTRY glVertexAttribI1ui(GLuint index, GLuint x);
GLAPI void GLAPIENTRY glVertexAttribI2ui(GLuint index, GLuint x, GLuint y);
GLAPI void GLAPIENTRY glVertexAttribI3ui(GLuint index, GLuint x, GLuint y, GLuint z);
GLAPI void GLAPIENTRY glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
GLAPI void GLAPIENTRY glVertexAttribI1iv(GLuint index, const GLint *v);
GLAPI void GLAPIENTRY glVertexAttribI2iv(GLuint index, const GLint *v);
GLAPI void GLAPIENTRY glVertexAttribI3iv(GLuint index, const GLint *v);
GLAPI void GLAPIENTRY glVertexAttribI4iv(GLuint index, const GLint *v);
GLAPI void GLAPIENTRY glVertexAttribI1uiv(GLuint index, const GLuint *v);
GLAPI void GLAPIENTRY glVertexAttribI2uiv(GLuint index, const GLuint *v);
GLAPI void GLAPIENTRY glVertexAttribI3uiv(GLuint index, const GLuint *v);
GLAPI void GLAPIENTRY glVertexAttribI4uiv(GLuint index, const GLuint *v);
GLAPI void GLAPIENTRY glVertexAttribI4bv(GLuint index, const GLbyte *v);
GLAPI void GLAPIENTRY glVertexAttribI4sv(GLuint index, const GLshort *v);
GLAPI void GLAPIENTRY glVertexAttribI4ubv(GLuint index, const GLubyte *v);
GLAPI void GLAPIENTRY glVertexAttribI4usv(GLuint index, const GLushort *v);
GLAPI void GLAPIENTRY glGetUniformuiv(GLuint program, GLint location, GLuint *params);
GLAPI void GLAPIENTRY glBindFragDataLocation(GLuint program, GLuint color, const GLchar *name);
GLAPI GLint GLAPIENTRY glGetFragDataLocation(GLuint program, const GLchar *name);
GLAPI void GLAPIENTRY glUniform1ui(GLint location, GLuint v0);
GLAPI void GLAPIENTRY glUniform2ui(GLint location, GLuint v0, GLuint v1);
GLAPI void GLAPIENTRY glUniform3ui(GLint location, GLuint v0, GLuint v1, GLuint v2);
GLAPI void GLAPIENTRY glUniform4ui(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3);
GLAPI void GLAPIENTRY glUniform1uiv(GLint location, GLsizei count, const GLuint *value);
GLAPI void GLAPIENTRY glUniform2uiv(GLint location, GLsizei count, const GLuint *value);
GLAPI void GLAPIENTRY glUniform3uiv(GLint location, GLsizei count, const GLuint *value);
GLAPI void GLAPIENTRY glUniform4uiv(GLint location, GLsizei count, const GLuint *value);
GLAPI void GLAPIENTRY glTexParameterIiv(GLenum target, GLenum pname, const GLint *params);
GLAPI void GLAPIENTRY glTexParameterIuiv(GLenum target, GLenum pname, const GLuint *params);
GLAPI void GLAPIENTRY glGetTexParameterIiv(GLenum target, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetTexParameterIuiv(GLenum target, GLenum pname, GLuint *params);
GLAPI void GLAPIENTRY glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint *value);
GLAPI void GLAPIENTRY glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint *value);
GLAPI void GLAPIENTRY glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value);
GLAPI void GLAPIENTRY glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil);
GLAPI const GLubyte * GLAPIENTRY glGetStringi(GLenum name, GLuint index);
GLAPI GLboolean GLAPIENTRY glIsRenderbuffer(GLuint renderbuffer);
GLAPI void GLAPIENTRY glBindRenderbuffer(GLenum target, GLuint renderbuffer);
GLAPI void GLAPIENTRY glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
GLAPI void GLAPIENTRY glGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
GLAPI void GLAPIENTRY glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
GLAPI void GLAPIENTRY glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
GLAPI void GLAPIENTRY glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level, GLint zoffset);
GLAPI void GLAPIENTRY glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
GLAPI void GLAPIENTRY glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
GLAPI void GLAPIENTRY glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height);
GLAPI void GLAPIENTRY glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
GLAPI void * GLAPIENTRY glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
GLAPI void GLAPIENTRY glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);
GLAPI void GLAPIENTRY glBindVertexArray(GLuint array);
GLAPI void GLAPIENTRY glDeleteVertexArrays(GLsizei n, const GLuint *arrays);
GLAPI void GLAPIENTRY glGenVertexArrays(GLsizei n, GLuint *arrays);
GLAPI GLboolean GLAPIENTRY glIsVertexArray(GLuint array);

/* 3.1 */
GLAPI void GLAPIENTRY glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
GLAPI void GLAPIENTRY glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
GLAPI void GLAPIENTRY glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer);
GLAPI void GLAPIENTRY glPrimitiveRestartIndex(GLuint index);
GLAPI void GLAPIENTRY glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
GLAPI void GLAPIENTRY glGetUniformIndices(GLuint program, GLsizei uniformCount, const GLchar *const*uniformNames, GLuint *uniformIndices);
GLAPI void GLAPIENTRY glGetActiveUniformsiv(GLuint program, GLsizei uniformCount, const GLuint *uniformIndices, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetActiveUniformName(GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei *length, GLchar *uniformName);
GLAPI GLuint GLAPIENTRY glGetUniformBlockIndex(GLuint program, const GLchar *uniformBlockName);
GLAPI void GLAPIENTRY glGetActiveUniformBlockiv(GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetActiveUniformBlockName(GLuint program, GLuint uniformBlockIndex, GLsizei bufSize, GLsizei *length, GLchar *uniformBlockName);
GLAPI void GLAPIENTRY glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);

/* 3.2 */
GLAPI void GLAPIENTRY glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex);
GLAPI void GLAPIENTRY glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex);
GLAPI void GLAPIENTRY glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex);
GLAPI void GLAPIENTRY glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count, GLenum type, const void *const*indices, GLsizei drawcount, const GLint *basevertex);
GLAPI void GLAPIENTRY glProvokingVertex(GLenum mode);
GLAPI GLsync GLAPIENTRY glFenceSync(GLenum condition, GLbitfield flags);
GLAPI GLboolean GLAPIENTRY glIsSync(GLsync sync);
GLAPI void GLAPIENTRY glDeleteSync(GLsync sync);
GLAPI GLenum GLAPIENTRY glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
GLAPI void GLAPIENTRY glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
GLAPI void GLAPIENTRY glGetInteger64v(GLenum pname, GLint64 *data);
GLAPI void GLAPIENTRY glGetSynciv(GLsync sync, GLenum pname, GLsizei count, GLsizei *length, GLint *values);
GLAPI void GLAPIENTRY glGetInteger64i_v(GLenum target, GLuint index, GLint64 *data);
GLAPI void GLAPIENTRY glGetBufferParameteri64v(GLenum target, GLenum pname, GLint64 *params);
GLAPI void GLAPIENTRY glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level);
GLAPI void GLAPIENTRY glTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations);
GLAPI void GLAPIENTRY glTexImage3DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLboolean fixedsamplelocations);
GLAPI void GLAPIENTRY glGetMultisamplefv(GLenum pname, GLuint index, GLfloat *val);
GLAPI void GLAPIENTRY glSampleMaski(GLuint maskNumber, GLbitfield mask);

/* 3.3 */
GLAPI void GLAPIENTRY glBindFragDataLocationIndexed(GLuint program, GLuint colorNumber, GLuint index, const GLchar *name);
GLAPI GLint GLAPIENTRY glGetFragDataIndex(GLuint program, const GLchar *name);
GLAPI void GLAPIENTRY glGenSamplers(GLsizei count, GLuint *samplers);
GLAPI void GLAPIENTRY glDeleteSamplers(GLsizei count, const GLuint *samplers);
GLAPI GLboolean GLAPIENTRY glIsSampler(GLuint sampler);
GLAPI void GLAPIENTRY glBindSampler(GLuint unit, GLuint sampler);
GLAPI void GLAPIENTRY glSamplerParameteri(GLuint sampler, GLenum pname, GLint param);
GLAPI void GLAPIENTRY glSamplerParameteriv(GLuint sampler, GLenum pname, const GLint *param);
GLAPI void GLAPIENTRY glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param);
GLAPI void GLAPIENTRY glSamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat *param);
GLAPI void GLAPIENTRY glSamplerParameterIiv(GLuint sampler, GLenum pname, const GLint *param);
GLAPI void GLAPIENTRY glSamplerParameterIuiv(GLuint sampler, GLenum pname, const GLuint *param);
GLAPI void GLAPIENTRY glGetSamplerParameteriv(GLuint sampler, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetSamplerParameterIiv(GLuint sampler, GLenum pname, GLint *params);
GLAPI void GLAPIENTRY glGetSamplerParameterfv(GLuint sampler, GLenum pname, GLfloat *params);
GLAPI void GLAPIENTRY glGetSamplerParameterIuiv(GLuint sampler, GLenum pname, GLuint *params);
GLAPI void GLAPIENTRY glQueryCounter(GLuint id, GLenum target);
GLAPI void GLAPIENTRY glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64 *params);
GLAPI void GLAPIENTRY glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params);
GLAPI void GLAPIENTRY glVertexAttribDivisor(GLuint index, GLuint divisor);
/* GL_ARB_texture_storage (core in 4.2) */
GLAPI void GLAPIENTRY glTexStorage1D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width);
GLAPI void GLAPIENTRY glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
GLAPI void GLAPIENTRY glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth);
