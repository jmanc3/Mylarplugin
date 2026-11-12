#include "client/egl_window.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <wayland-egl.h>

namespace {
void require_egl(bool ok, const char *operation) {
    if (!ok)
        throw std::runtime_error(std::string(operation) + " failed (EGL error " + std::to_string(eglGetError()) + ")");
}
struct Display {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config{};
    explicit Display(wl_display *native) {
        display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, native, nullptr);
        require_egl(display != EGL_NO_DISPLAY, "eglGetPlatformDisplay");
        require_egl(eglInitialize(display, nullptr, nullptr), "eglInitialize");
        try {
            require_egl(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
            const EGLint attributes[] = {EGL_SURFACE_TYPE,
                                         EGL_WINDOW_BIT,
                                         EGL_RENDERABLE_TYPE,
                                         EGL_OPENGL_ES3_BIT,
                                         EGL_RED_SIZE,
                                         8,
                                         EGL_GREEN_SIZE,
                                         8,
                                         EGL_BLUE_SIZE,
                                         8,
                                         EGL_ALPHA_SIZE,
                                         8,
                                         EGL_SAMPLE_BUFFERS,
                                         0,
                                         EGL_SAMPLES,
                                         0,
                                         EGL_NONE};
            EGLint count = 0;
            require_egl(eglChooseConfig(display, attributes, &config, 1, &count) && count, "eglChooseConfig");
        } catch (...) {
            eglTerminate(display);
            display = EGL_NO_DISPLAY;
            throw;
        }
    }
    ~Display() {
        if (display != EGL_NO_DISPLAY)
            eglTerminate(display);
    }
};
std::shared_ptr<Display> display_for(wl_display *native) {
    // Every app (dock per monitor, settings, edit_pin) runs its own event-loop
    // thread with its own wl_display, so the registry itself needs a lock.
    static std::mutex mutex;
    static std::map<wl_display *, std::weak_ptr<Display>> displays;
    std::lock_guard lock(mutex);
    std::erase_if(displays, [](const auto &entry) { return entry.second.expired(); });
    auto &weak = displays[native];
    auto display = weak.lock();
    if (!display) {
        display = std::make_shared<Display>(native);
        weak = display;
    }
    return display;
}
} // namespace
struct EGLWindow::Impl {
    std::shared_ptr<Display> display;
    wl_egl_window *window = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    std::unique_ptr<drawing::OpenGLContext> drawing;
    int width = 0, height = 0;
    bool resize_pending = false;
    void current() { require_egl(eglMakeCurrent(display->display, surface, surface, context), "eglMakeCurrent"); }
    ~Impl() {
        if (!display)
            return;
        // Prefer releasing GL objects without binding the drawable: binding it
        // can make Mesa wait on the compositor for a free buffer, and windows are
        // sometimes closed while the app mutex is held.
        if (context != EGL_NO_CONTEXT &&
            !eglMakeCurrent(display->display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) &&
            surface != EGL_NO_SURFACE)
            eglMakeCurrent(display->display, surface, surface, context);
        drawing.reset();
        eglMakeCurrent(display->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display->display, surface);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display->display, context);
        if (window)
            wl_egl_window_destroy(window);
    }
};
EGLWindow::EGLWindow(wl_display *display, wl_surface *surface, int width, int height)
    : impl_(std::make_unique<Impl>()) {
    auto &i = *impl_;
    i.display = display_for(display);
    i.window = wl_egl_window_create(surface, width, height);
    if (!i.window)
        throw std::runtime_error("wl_egl_window_create failed");
    i.surface = eglCreatePlatformWindowSurface(i.display->display, i.display->config, i.window, nullptr);
    require_egl(i.surface != EGL_NO_SURFACE, "eglCreatePlatformWindowSurface");
    const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    i.context = eglCreateContext(i.display->display, i.display->config, EGL_NO_CONTEXT, attributes);
    require_egl(i.context != EGL_NO_CONTEXT, "eglCreateContext");
    i.current();
    // UI invalidation/frame callbacks drive repainting; avoid blocking the event
    // loop in swap on an obscured window or while a popup waits for configure.
    require_egl(eglSwapInterval(i.display->display, 0), "eglSwapInterval");
    i.drawing = drawing::create_opengl_context(width, height);
    i.width = width;
    i.height = height;
}
EGLWindow::~EGLWindow() = default;
void EGLWindow::resize(int width, int height) {
    // Only record the size: the drawable is rebuilt at the next begin_frame, so
    // resizing never binds the drawable or blocks.
    if (width == impl_->width && height == impl_->height)
        return;
    impl_->width = width;
    impl_->height = height;
    impl_->resize_pending = true;
}
void EGLWindow::begin_frame() {
    auto &i = *impl_;
    if (i.resize_pending) {
        // Drivers apply wl_egl_window_resize lazily (NVIDIA's egl-wayland can keep
        // the old buffer size for a frame), which presented a frame laid out at
        // the new size into an old-size buffer: content looked scaled and clipped
        // until the next redraw. Recreating the EGLSurface after the resize makes
        // the very next buffer the requested size on every driver.
        eglMakeCurrent(i.display->display, EGL_NO_SURFACE, EGL_NO_SURFACE, i.context);
        if (i.surface != EGL_NO_SURFACE)
            eglDestroySurface(i.display->display, i.surface);
        i.surface = EGL_NO_SURFACE;
        wl_egl_window_resize(i.window, i.width, i.height, 0, 0);
        i.surface = eglCreatePlatformWindowSurface(i.display->display, i.display->config, i.window, nullptr);
        require_egl(i.surface != EGL_NO_SURFACE, "eglCreatePlatformWindowSurface");
        i.current();
        require_egl(eglSwapInterval(i.display->display, 0), "eglSwapInterval");
        i.drawing->resize(i.width, i.height);
        i.resize_pending = false;
    } else {
        i.current();
    }
    i.drawing->begin_frame();
}
void EGLWindow::present() {
    impl_->drawing->flush();
    // EGL attaches, damages, and commits the wl_surface itself.
    require_egl(eglSwapBuffers(impl_->display->display, impl_->surface), "eglSwapBuffers");
}
drawing::Context *EGLWindow::context() { return impl_->drawing.get(); }
