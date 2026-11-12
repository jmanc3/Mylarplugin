#pragma once

#include "drawing/opengl_context.h"
#include <memory>

struct wl_display;
struct wl_surface;

// Owns native EGL presentation and keeps the GL context current during cleanup.
class EGLWindow {
  public:
    EGLWindow(wl_display *display, wl_surface *surface, int width, int height);
    ~EGLWindow();
    void resize(int width, int height);
    void begin_frame();
    void present();
    drawing::Context *context();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
