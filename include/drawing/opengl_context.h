#pragma once

#include "drawing/context.h"

namespace drawing {
// Requires a current OpenGL ES 3 context throughout use AND destruction.
// Owns its GL objects, but not the EGL context or its presentation surface.
class OpenGLContext : public Context {
  public:
    virtual void resize(int width, int height) = 0;
};

// flush() resolves the antialiased frame to framebuffer 0. Presentation is the
// caller's responsibility. No CPU readback is performed by the renderer.
std::unique_ptr<OpenGLContext> create_opengl_context(int width, int height);
} // namespace drawing
