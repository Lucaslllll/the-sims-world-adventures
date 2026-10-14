// OpenGL ES function table and drawable state.
#pragma once

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "hle/common.h"
#include "hle/gl_funcs.inc"

namespace gl {

struct Fns {
#define X(name, ret, params) ret(*name) params;
    GL_FUNCS(X)
#undef X
};
extern Fns fn;
extern bool g_es1;

// Resolve all entry points for an ES1.1 or ES2/3 context.
bool load(bool es1);
void init_classes();  // EAGLContext

GLuint drawable_texture();
int drawable_width();
int drawable_height();

}  // namespace gl
