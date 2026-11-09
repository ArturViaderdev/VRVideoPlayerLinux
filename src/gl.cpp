#include "gl.h"
#include <cstdio>
#include <cstring>

#define VRGL_DEF(ret, name, args) PFN_vr_##name vr_##name = nullptr;
VRGL_FUNCS(VRGL_DEF)
VRGL_PLATFORM_FUNCS(VRGL_DEF)
#undef VRGL_DEF

bool vrgl_load() {
  bool ok = true;
#define VRGL_LOAD(ret, name, args)                                         \
  vr_##name = (PFN_vr_##name)glfwGetProcAddress(#name);                    \
  if (!vr_##name) {                                                        \
    fprintf(stderr, "vrvideo: falta la funcion OpenGL %s\n", #name);       \
    ok = false;                                                            \
  }
  VRGL_FUNCS(VRGL_LOAD)
  VRGL_PLATFORM_FUNCS(VRGL_LOAD)
#undef VRGL_LOAD
  return ok;
}

bool vrgl_has_extension(const char *name) {
  GLint n = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &n);
  for (GLint i = 0; i < n; i++) {
    const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i);
    if (e && strcmp(e, name) == 0) return true;
  }
  return false;
}

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    fprintf(stderr, "vrvideo: error de shader: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint vrgl_program(const char *vs, const char *fs) {
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(p, sizeof log, nullptr, log);
    fprintf(stderr, "vrvideo: error enlazando shader: %s\n", log);
    return 0;
  }
  return p;
}
