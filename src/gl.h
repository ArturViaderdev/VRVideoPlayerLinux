// Minimal OpenGL loader for vrvideo: only the functions it uses, loaded through GLFW so the same code
// builds on Linux and (later) Windows without glad/glew. GL 1.1 functions are linked directly.
#pragma once
#ifdef _WIN32
#include <windows.h>
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <cstdint>

#ifndef APIENTRY
#define APIENTRY
#endif

// constants (guarded: old Windows gl.h only has GL 1.1)
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_TEXTURE_TILING_EXT
#define GL_TEXTURE_TILING_EXT 0x9580
#endif
#ifndef GL_DEDICATED_MEMORY_OBJECT_EXT
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#endif
#ifndef GL_OPTIMAL_TILING_EXT
#define GL_OPTIMAL_TILING_EXT 0x9584
#endif
#ifndef GL_HANDLE_TYPE_OPAQUE_FD_EXT
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586
#endif
#ifndef GL_HANDLE_TYPE_OPAQUE_WIN32_EXT
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587
#endif
#ifndef GL_DEVICE_UUID_EXT
#define GL_DEVICE_UUID_EXT 0x9597
#endif

#define VRGL_FUNCS(X)                                                                                   \
  X(void, glGenFramebuffers, (GLsizei n, GLuint * ids))                                                 \
  X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *ids))                                         \
  X(void, glBindFramebuffer, (GLenum target, GLuint fb))                                                \
  X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint lvl)) \
  X(GLenum, glCheckFramebufferStatus, (GLenum target))                                                  \
  X(void, glBlitFramebuffer,                                                                            \
    (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,            \
     GLbitfield mask, GLenum filter))                                                                   \
  X(void, glCopyImageSubData,                                                                           \
    (GLuint src, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint dst, GLenum dt, GLint dl,    \
     GLint dx, GLint dy, GLint dz, GLsizei w, GLsizei h, GLsizei d))                                    \
  X(void, glTexStorage2D, (GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h))            \
  X(GLuint, glCreateShader, (GLenum type))                                                              \
  X(void, glShaderSource, (GLuint sh, GLsizei n, const char *const *src, const GLint *len))             \
  X(void, glCompileShader, (GLuint sh))                                                                 \
  X(void, glGetShaderiv, (GLuint sh, GLenum p, GLint * v))                                              \
  X(void, glGetShaderInfoLog, (GLuint sh, GLsizei n, GLsizei * len, char *log))                         \
  X(GLuint, glCreateProgram, (void))                                                                    \
  X(void, glAttachShader, (GLuint pr, GLuint sh))                                                       \
  X(void, glLinkProgram, (GLuint pr))                                                                   \
  X(void, glGetProgramiv, (GLuint pr, GLenum p, GLint * v))                                             \
  X(void, glGetProgramInfoLog, (GLuint pr, GLsizei n, GLsizei * len, char *log))                        \
  X(void, glUseProgram, (GLuint pr))                                                                    \
  X(void, glDeleteShader, (GLuint sh))                                                                  \
  X(GLint, glGetUniformLocation, (GLuint pr, const char *name))                                         \
  X(void, glUniformMatrix4fv, (GLint loc, GLsizei n, GLboolean tr, const GLfloat *v))                   \
  X(void, glUniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                         \
  X(void, glUniform1i, (GLint loc, GLint v))                                                            \
  X(void, glGenVertexArrays, (GLsizei n, GLuint * ids))                                                 \
  X(void, glBindVertexArray, (GLuint id))                                                               \
  X(void, glActiveTexture, (GLenum tex))                                                                \
  X(void, glBindBuffer, (GLenum target, GLuint buf))                                                    \
  X(const GLubyte *, glGetStringi, (GLenum name, GLuint i))                                             \
  X(void, glCreateMemoryObjectsEXT, (GLsizei n, GLuint * ids))                                          \
  X(void, glDeleteMemoryObjectsEXT, (GLsizei n, const GLuint *ids))                                     \
  X(void, glMemoryObjectParameterivEXT, (GLuint mem, GLenum p, const GLint *v))                         \
  X(void, glTexStorageMem2DEXT,                                                                         \
    (GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h, GLuint mem, uint64_t offset))     \
  X(void, glGetUnsignedBytei_vEXT, (GLenum target, GLuint index, GLubyte * data))

#ifdef _WIN32
#define VRGL_PLATFORM_FUNCS(X) \
  X(void, glImportMemoryWin32HandleEXT, (GLuint mem, uint64_t size, GLenum type, void *handle))
#else
#define VRGL_PLATFORM_FUNCS(X) X(void, glImportMemoryFdEXT, (GLuint mem, uint64_t size, GLenum type, GLint fd))
#endif

#define VRGL_DECL(ret, name, args)       \
  typedef ret(APIENTRY *PFN_vr_##name) args; \
  extern PFN_vr_##name vr_##name;
VRGL_FUNCS(VRGL_DECL)
VRGL_PLATFORM_FUNCS(VRGL_DECL)
#undef VRGL_DECL

// use the loaded pointers under their normal names
#define glGenFramebuffers vr_glGenFramebuffers
#define glDeleteFramebuffers vr_glDeleteFramebuffers
#define glBindFramebuffer vr_glBindFramebuffer
#define glFramebufferTexture2D vr_glFramebufferTexture2D
#define glCheckFramebufferStatus vr_glCheckFramebufferStatus
#define glBlitFramebuffer vr_glBlitFramebuffer
#define glCopyImageSubData vr_glCopyImageSubData
#define glTexStorage2D vr_glTexStorage2D
#define glCreateShader vr_glCreateShader
#define glShaderSource vr_glShaderSource
#define glCompileShader vr_glCompileShader
#define glGetShaderiv vr_glGetShaderiv
#define glGetShaderInfoLog vr_glGetShaderInfoLog
#define glCreateProgram vr_glCreateProgram
#define glAttachShader vr_glAttachShader
#define glLinkProgram vr_glLinkProgram
#define glGetProgramiv vr_glGetProgramiv
#define glGetProgramInfoLog vr_glGetProgramInfoLog
#define glUseProgram vr_glUseProgram
#define glDeleteShader vr_glDeleteShader
#define glGetUniformLocation vr_glGetUniformLocation
#define glUniformMatrix4fv vr_glUniformMatrix4fv
#define glUniform4f vr_glUniform4f
#define glUniform1i vr_glUniform1i
#define glGenVertexArrays vr_glGenVertexArrays
#define glBindVertexArray vr_glBindVertexArray
#define glActiveTexture vr_glActiveTexture
#define glBindBuffer vr_glBindBuffer
#define glGetStringi vr_glGetStringi
#define glCreateMemoryObjectsEXT vr_glCreateMemoryObjectsEXT
#define glDeleteMemoryObjectsEXT vr_glDeleteMemoryObjectsEXT
#define glMemoryObjectParameterivEXT vr_glMemoryObjectParameterivEXT
#define glTexStorageMem2DEXT vr_glTexStorageMem2DEXT
#define glGetUnsignedBytei_vEXT vr_glGetUnsignedBytei_vEXT
#ifdef _WIN32
#define glImportMemoryWin32HandleEXT vr_glImportMemoryWin32HandleEXT
#else
#define glImportMemoryFdEXT vr_glImportMemoryFdEXT
#endif

// Loads every function; returns false (and prints what is missing) if one is not available.
bool vrgl_load();
bool vrgl_has_extension(const char *name);
// compiles and links a program; returns 0 on error (log printed)
GLuint vrgl_program(const char *vs, const char *fs);
