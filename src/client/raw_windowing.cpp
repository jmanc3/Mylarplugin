// wl_input.c
 
#include "client/raw_windowing.h"

#include "second.h"

#include <cairo-deprecated.h>
#include <cstddef>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-cursor.h>
#include <wayland-server-core.h>
#define _POSIX_C_SOURCE 200809L
#include <poll.h>    // for POLLIN, POLLOUT, POLLERR, etc.
#include <errno.h>   // for EAGAIN, EINTR, and other errno constants
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <wayland-client.h>
#include <vector>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include <cairo/cairo.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <climits>

extern "C" {
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "wp-viewporter-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
}

#include <xkbcommon/xkbcommon.h>

#include "../include/container.h"
#include "../include/events.h"
#include "../include/hypriso.h"

static int unique_id = 0;

struct wl_window;

bool wl_window_resize_buffer(struct wl_window *win, int new_width, int new_height);

struct wl_context {
    int wake_pipe[2];
    int id;
    RawApp *ra;

    std::vector<PolledFunction> polled_fds;
    
    bool running = true;
    struct wl_display *display = nullptr;
    struct wl_registry *registry = nullptr;
    struct wl_compositor *compositor = nullptr;
    struct wl_shm *shm = nullptr;
    struct wl_seat *seat = nullptr;
    struct xdg_wm_base *wm_base = nullptr;
    struct zwlr_layer_shell_v1 *layer_shell = nullptr;
    struct wl_keyboard *keyboard = nullptr;
    struct wl_pointer *pointer = nullptr;
    struct xkb_context *xkb_ctx = nullptr;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager = nullptr;
    struct wp_viewporter *viewporter = nullptr;
    struct wp_cursor_shape_manager_v1 *shape_manager = nullptr;
    struct wp_cursor_shape_device_v1 *shape_device = nullptr;

    //struct wl_output *output;
    uint32_t shm_format;

    std::vector<wl_window *> windows;
};

struct wl_window {
    int id;
    bool keeper_of_life = true; // While this window exists app should continue to run
    RawWindow *rw = nullptr;
    bool configured = false;
    
    struct wl_context *ctx = nullptr;
    struct wl_surface *surface = nullptr;
    struct xdg_surface *xdg_surface = nullptr;
    struct xdg_toplevel *xdg_toplevel = nullptr;
    struct zwlr_layer_surface_v1 *layer_surface = nullptr;
    struct wl_output *output = nullptr;
    struct xkb_keymap *keymap = nullptr;
    struct xkb_state *xkb_state = nullptr;
    struct wl_shm_pool *pool = nullptr;
    struct wp_fractional_scale_v1 *fractional_scale = nullptr;
    wp_viewport *viewport = nullptr;
    wl_buffer *buffer = nullptr;
    bool busy = false;
    bool dropped_frame = false;
    bool resize_next = false;

    struct wl_cursor_theme *cursor_theme = nullptr;
    struct wl_cursor *cursor = nullptr;
    struct wl_surface *cursor_surface = nullptr;

    float current_fractional_scale = 1.0; // default value

    std::function<void(wl_window *)> on_render = nullptr;

    cairo_surface_t *cairo_surface = nullptr;
    cairo_t *cr = nullptr;
    
    int pending_width, pending_height; // recieved from configured event
    
    int logical_width, logical_height;
    int scaled_w, scaled_h;
    void *data;
    size_t size;
    int stride;

    int cur_x = 0;
    int cur_y = 0;

    std::string title;
    bool has_pointer_focus = false;
    bool has_keyboard_focus = false;
    bool is_layer = true;
    bool marked_for_closing = false;
};

std::vector<wl_context *> apps;
std::vector<wl_window *> windows;

static struct wl_buffer *create_shm_buffer(struct wl_context *d, int width, int height);

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
    log("buffer released and available");
    auto win = (wl_window *) data;
    win->busy = false;
    if (win->dropped_frame) {
        win->dropped_frame = false;

        if (win->on_render) {
            win->on_render(win);
        }
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void handle_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    auto win = (wl_window *) data;
    win->marked_for_closing = true;
    printf("Compositor requested window close\n");
    //running = false;  // set your main loop flag to exit
}

static void handle_toplevel_configure(
    void *data,
    struct xdg_toplevel *toplevel,
    int32_t width,
    int32_t height,
    struct wl_array *states) 
{
    auto win = (wl_window *) data;

    // Save for later (don’t resize yet)
    if (width > 0) win->pending_width  = width;
    if (height > 0) win->pending_height = height;
    //wl_window_resize_buffer(win, width, height);
    //wl_window_draw(win);
   
    // Usually you’d handle resize here
    printf("size reconfigured\n");
}

static void config_surface(wl_window *win, uint32_t w, uint32_t h) {
    wl_window_resize_buffer(win, win->pending_width, win->pending_height);
    if (win->rw->on_resize) {
        win->rw->on_resize(win->rw, win->scaled_w, win->scaled_h);
    }
    if (win->on_render)
        win->on_render(win);
}

static void handle_surface_configure(void *data,
			  struct xdg_surface *xdg_surface,
			  uint32_t serial) {
    struct wl_window *win = (struct wl_window *)data;

    if (win->pending_width > 0 && win->pending_height > 0 &&
        (win->pending_width != win->logical_width || win->pending_height != win->logical_height)) {
        if (win->configured) {
            config_surface(win, win->pending_width, win->pending_height);
        }
    }

    xdg_surface_ack_configure(xdg_surface, serial);
    win->configured = true;
    log("surface commit");
    wl_surface_commit(win->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = handle_surface_configure, 	 
};

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = handle_toplevel_configure,
    .close = handle_toplevel_close,
};

/* ---- helper: create a simple shm buffer so surface is mapped ---- */
static int create_shm_file(size_t size) {
    char temp[] = "/tmp/wl-shm-XXXXXX";
    int fd = mkstemp(temp);
    if (fd >= 0) {
        unlink(temp); // unlink so it is removed after close
        if (ftruncate(fd, size) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static int create_anonymous_file(off_t size) {
    char path[] = "/dev/shm/wayland-shm-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        return -1;
    }
    unlink(path);
    if (ftruncate(fd, size) < 0) {
        fprintf(stderr, "ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void destroy_shm_buffer(struct wl_window *win) {
    if (win->cairo_surface) {
        cairo_surface_destroy(win->cairo_surface);
        win->cairo_surface = nullptr;
    }

    if (win->buffer) {
        wl_buffer_destroy(win->buffer);
        win->buffer = nullptr;
    }

    if (win->data) {
        const int stride = win->scaled_w* 4;
        const int size = stride * win->scaled_h;
        munmap(win->data, size);
        win->data = nullptr;
    }
}

void on_window_render(wl_window *win) {
    if (win->resize_next) {
        wl_window_resize_buffer(win, win->logical_width, win->logical_height);
        win->resize_next = false;
    }
    log("on_window_render");
    if (win->busy) {
        win->dropped_frame  = true;
        return;
    }

    if (win->rw) {
        if (win->rw->on_render) {
            win->rw->on_render(win->rw, win->scaled_w, win->scaled_h);
        }
    }
    wl_surface_attach(win->surface, win->buffer, 0, 0);
    wl_surface_damage_buffer(win->surface, 0, 0, INT32_MAX, INT32_MAX);
    log("surface commit");
    wl_surface_commit(win->surface);
    static long start = get_current_time_in_ms();
    long current = get_current_time_in_ms();
    if (current - start > 200)
        win->busy = true;
}

bool wl_window_resize_buffer(struct wl_window *win, int _new_width, int _new_height) {
    log("wl_window_resize_buffer");
    destroy_shm_buffer(win);

    win->logical_width = _new_width;
    win->logical_height = _new_height;
    win->scaled_w = win->logical_width * win->current_fractional_scale;
    win->scaled_h = win->logical_height * win->current_fractional_scale;

    const int stride = win->scaled_w * 4;
    const int size = stride * win->scaled_h;

    int fd = create_anonymous_file(size);
    if (fd < 0) {
        fprintf(stderr, "Failed to create shm file\n");
        return false;
    }

    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(win->ctx->shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, win->scaled_w, win->scaled_h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!buffer) {
        fprintf(stderr, "Failed to create wl_buffer\n");
        munmap(data, size);
        return false;
    }

    win->buffer = buffer;
    win->data = data;

    win->cairo_surface = cairo_image_surface_create_for_data(
        (unsigned char*)data,
        CAIRO_FORMAT_ARGB32,
        win->scaled_w,
        win->scaled_h,
        stride
    );

    if (cairo_surface_status(win->cairo_surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "Failed to create cairo surface\n");
        destroy_shm_buffer(win);
        return false;
    }

    win->cr = cairo_create(win->cairo_surface);
    if (win->rw)
        win->rw->cr = win->cr;

    wl_buffer_add_listener(win->buffer, &buffer_listener, win);
    
    auto cr = win->cr;
    
    if (win->rw) {
        win->on_render = on_window_render;
        //win->on_render(win);
    }

    if (win->viewport)
        wp_viewport_set_destination(win->viewport, win->logical_width, win->logical_height);

    return true;
}

static void config_layer_shell(wl_window *win, uint32_t width, uint32_t height);

static void handle_fractional_scale_preferred_scale(
    void *data,
    wp_fractional_scale_v1 *obj,
    uint32_t scale)
{
    auto win = (wl_window *) data;
    win->current_fractional_scale = ((float) scale) / 120.0f;
    win->rw->dpi = win->current_fractional_scale;

    win->resize_next = true;

    wl_surface_set_buffer_scale(win->surface, std::ceil(scale));

    wp_viewport_set_destination(win->viewport,
                                win->logical_width,
                                win->logical_height);

    wl_surface_commit(win->surface);

    windowing::redraw(win->rw);
}

static const wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = handle_fractional_scale_preferred_scale
};

struct wl_window *wl_window_create(struct wl_context *ctx,
                                   int width, int height,
                                   const char *title)
{
    struct wl_window *win = new wl_window;
    win->ctx = ctx;
    win->logical_width = width;
    win->logical_height = height;
    win->scaled_w = win->logical_width * win->current_fractional_scale;
    win->scaled_h = win->logical_height * win->current_fractional_scale;
    win->title = title;
    win->pending_width = width;
    win->pending_height = height;
    win->buffer = NULL;
    win->pool = NULL;
    win->data = NULL;

    // 1️⃣ Create surface
    win->surface = wl_compositor_create_surface(ctx->compositor);
    if (!win->surface) {
        fprintf(stderr, "Failed to create wl_surface\n");
        delete win;
        return nullptr;
    }
    win->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(ctx->fractional_scale_manager, win->surface);
    wp_fractional_scale_v1_add_listener(win->fractional_scale, &fractional_scale_listener, win);
    win->viewport = wp_viewporter_get_viewport(ctx->viewporter, win->surface); 

    // 2️⃣ Get xdg_surface
    win->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->wm_base, win->surface);
    if (!win->xdg_surface) {
        fprintf(stderr, "Failed to get xdg_surface\n");
        wl_surface_destroy(win->surface);
        delete win;
        return nullptr;
    }

    // 3️⃣ Add surface listener BEFORE committing
    xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);

    // 4️⃣ Create toplevel
    win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    if (!win->xdg_toplevel) {
        fprintf(stderr, "Failed to get xdg_toplevel\n");
        xdg_surface_destroy(win->xdg_surface);
        wl_surface_destroy(win->surface);
        delete win;
        return nullptr;
    }

    // 5️⃣ Add toplevel listener
    xdg_toplevel_add_listener(win->xdg_toplevel, &xdg_toplevel_listener, win);

    // 6️⃣ Set metadata
    xdg_toplevel_set_title(win->xdg_toplevel, title ? title : "Wayland Window");

    // 7️⃣ Single initial commit
    log("surface commit");
    wl_surface_commit(win->surface);

    if (!win->configured)
        wl_display_dispatch(ctx->display); 
    
    wl_window_resize_buffer(win, win->scaled_w, win->scaled_h); // create shm buffer
    wl_surface_attach(win->surface, win->buffer, 0, 0);
    log("surface commit");
    wl_surface_commit(win->surface);

    ctx->windows.push_back(win);
    return win;
}

static void config_layer_shell(wl_window *win, uint32_t width, uint32_t height) {
    wl_window_resize_buffer(win, width, height);
    if (win->rw)
        if (win->rw->on_resize)
            win->rw->on_resize(win->rw, win->scaled_w, win->scaled_h);
    if (win->on_render)
        win->on_render(win);    
}

static void configure_layer_shell(void *data,
                        		  struct zwlr_layer_surface_v1 *surf,
                        		  uint32_t serial,
                        		  uint32_t width,
                            	  uint32_t height) {
    struct wl_window *win = (struct wl_window *)data;
    if (win->configured) {
        config_layer_shell(win, width, height);
    }
    win->configured = true;
    zwlr_layer_surface_v1_ack_configure(surf, serial);
}

static const struct zwlr_layer_surface_v1_listener layer_shell_listener = {
    .configure = configure_layer_shell,
    .closed = nullptr
};

struct wl_window *wl_layer_window_create(struct wl_context *ctx, int width, int height,
                                         zwlr_layer_shell_v1_layer layer, const char *title, bool exclusive_zone = true)
{
    struct wl_window *win = new wl_window;
    win->ctx = ctx;
    win->logical_width = width;
    win->logical_height= height;
    win->scaled_w = win->logical_width * win->current_fractional_scale; 
    win->scaled_h = win->logical_height * win->current_fractional_scale; 
    win->title = title;

    win->surface = wl_compositor_create_surface(ctx->compositor);
    
    win->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(ctx->fractional_scale_manager, win->surface);
    wp_fractional_scale_v1_add_listener(win->fractional_scale, &fractional_scale_listener, win);
    win->viewport = wp_viewporter_get_viewport(ctx->viewporter, win->surface); 

    win->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        ctx->layer_shell, win->surface, NULL, layer, title);

    zwlr_layer_surface_v1_set_anchor(win->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(win->layer_surface, width, height);
    zwlr_layer_surface_v1_set_keyboard_interactivity(win->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    if (exclusive_zone)
        zwlr_layer_surface_v1_set_exclusive_zone(win->layer_surface, height);

    zwlr_layer_surface_v1_add_listener(win->layer_surface, &layer_shell_listener, win);

    log("surface commit");
    wl_surface_commit(win->surface);
    if (!win->configured)
        wl_display_dispatch(ctx->display); 

    wl_window_resize_buffer(win, win->scaled_w, win->scaled_h); // create shm buffer
    wl_surface_attach(win->surface, win->buffer, 0, 0);
    log("surface commit");
    wl_surface_commit(win->surface);

    ctx->windows.push_back(win);
    return win;
}

static struct wl_buffer *create_shm_buffer(struct wl_context *ctx, int width, int height) {
    int stride = width * 4;
    size_t size = stride * height;
    int fd = create_shm_file(size);
    if (fd < 0) return NULL;
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    // fill with transparent black
    memset(data, 0, size);
        // Fill with white (255,255,255) at 60% transparency (A=153)
    uint8_t *pixels = (uint8_t *) data;
    const uint8_t alpha = (255.f * .2);
    const uint8_t value = 255 * alpha / 255; // premultiplied: 153
    for (size_t i = 0; i < size; i += 4) {
        pixels[i + 0] = value; // B
        pixels[i + 1] = value; // G
        pixels[i + 2] = value; // R
        pixels[i + 3] = alpha; // A
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
                                                         width, height,
                                                         stride, ctx->shm_format);
    wl_shm_pool_destroy(pool);
    munmap(data, size);
    close(fd);
    return buffer;
}

struct wl_buffer *create_shm_buffer_with_cairo(struct wl_context *ctx,
                                               int width, int height,
                                               void (**out_unmap)(void*, size_t),
                                               void **out_data,
                                               size_t *out_size)
{
    int stride = width * 4;
    size_t size = stride * height;
    int fd = create_shm_file(size);
    if (fd < 0) return NULL;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    // Create shm pool + buffer
    struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride, ctx->shm_format);
    wl_shm_pool_destroy(pool);
    close(fd);

    //if (out_unmap) *out_unmap = munmap;
    if (out_data) *out_data = data;
    if (out_size) *out_size = size;
    return buffer;
}

/* ---- pointer callbacks ---- */
static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy) {
    double dx = wl_fixed_to_double(sx);
    double dy = wl_fixed_to_double(sy);
    printf("pointer: enter at %.2f, %.2f\n", dx, dy);
    auto ctx = (wl_context *) data;
    for (auto w : ctx->windows) {
        if (w->surface == surface) {
            printf("pointer: enter at %.2f, %.2f for %s\n", dx, dy, w->title.data());
            w->has_pointer_focus = true;
            w->cur_x = sx;
            w->cur_y = sy;
            if (w->rw->on_mouse_enters) {
               w->rw->on_mouse_enters(w->rw, sx, sy);
            }
            if (w->on_render)
                w->on_render(w);

            {
                wp_cursor_shape_device_v1_set_shape(ctx->shape_device, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
            }
        }
    }
}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t serial, struct wl_surface *surface) {
    printf("pointer: leave\n");
    auto ctx = (wl_context *) data;
    for (auto w : ctx->windows) {
        if (w->surface == surface) {
            w->has_pointer_focus = false;
            if (w->rw->on_mouse_leaves) {
               w->rw->on_mouse_leaves(w->rw, w->cur_x, w->cur_y);
            }
            if (w->on_render)
                w->on_render(w);
        }
    }
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    double dx = wl_fixed_to_double(sx);
    double dy = wl_fixed_to_double(sy);
    auto ctx = (wl_context *) data;
    for (auto w : ctx->windows) {
        if (w->has_pointer_focus) {
            log(fz("pointer: motion at {}, {} for %s\n", dx, dy, w->title.data()));
            w->cur_x = dx;
            w->cur_y = dy;
            if (w->rw->on_mouse_move)
               w->rw->on_mouse_move(w->rw, dx, dy);
            if (w->on_render)
                w->on_render(w);
        }
    }
    //running = false;
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time,
                                  uint32_t button, uint32_t state) {
    const char *st = (state == WL_POINTER_BUTTON_STATE_PRESSED) ? "pressed" : "released";
    printf("pointer: button %u %s\n", button, st);
    //win->marked_for_closing = true;
    auto ctx = (wl_context *) data;
    for (auto w : ctx->windows) {
        if (w->has_pointer_focus) {
            log(fz("pointer: handle button {} {} {} {}", w->cur_x, w->cur_y, button, state));
            if (w->rw->on_mouse_press) {
                w->rw->on_mouse_press(w->rw, button, state, w->cur_x, w->cur_y);
            }

            if (w->on_render)
                w->on_render(w);
        }
    }

    //running = false;
    //stop_dock();
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value) {
    double v = wl_fixed_to_double(value);
    printf("pointer: axis %u value %.2f\n", axis, v);
    auto ctx = (wl_context *) data;
    for (auto w : ctx->windows) {
        if (w->has_pointer_focus) {
            printf("pointer: handle scroll\n");
            if (w->rw->on_scrolled) {
                w->rw->on_scrolled(w->rw, 0, axis, 0, value, 0, 0);
            }
            if (w->on_render)
                w->on_render(w);
        }
    }
}

static void pointer_handle_frame(void *data,
	      struct wl_pointer *wl_pointer) {
}

static void pointer_handle_axis_source(void *data,
	    struct wl_pointer *wl_pointer,
	    uint32_t axis_source) {
}

static void pointer_handle_axis_stop(void *data,
		  struct wl_pointer *wl_pointer,
		  uint32_t time,
		  uint32_t axis) {
}

static void pointer_handle_axis_discrete(void *data,
		      struct wl_pointer *wl_pointer,
		      uint32_t axis,
		      int32_t discrete) {
}

static void pointer_handle_axis_value120(void *data,
		      struct wl_pointer *wl_pointer,
		      uint32_t axis,
		      int32_t value120) {
}

static void pointer_handle_axis_relative_direction(void *data,
				struct wl_pointer *wl_pointer,
				uint32_t axis,
				uint32_t direction) {
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis = pointer_handle_axis,
    .frame = pointer_handle_frame,
    .axis_source = pointer_handle_axis_source,
    .axis_stop = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
    .axis_value120 = pointer_handle_axis_value120,
    .axis_relative_direction = pointer_handle_axis_relative_direction,
};

/* ---- keyboard callbacks ---- */
static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
                                   uint32_t format, int fd, uint32_t size) {
    wl_context *ctx = (wl_context *) data;
    wl_window *win = nullptr;
    for (auto w : ctx->windows)
        if (w->has_keyboard_focus)
            win = w;
    if (!win) {
        for (auto w : ctx->windows)
            if (w->has_pointer_focus)
                win = w;
    }
    if (!win)
        return;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map_shm = (char *) mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map_shm == MAP_FAILED) {
        close(fd);
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(ctx->xkb_ctx,
                                                           map_shm,
                                                           XKB_KEYMAP_FORMAT_TEXT_V1,
                                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        printf("Failed to compile xkb keymap\n");
    } else {
        if (win->keymap) xkb_keymap_unref(win->keymap);
        if (win->xkb_state) xkb_state_unref(win->xkb_state);
        win->keymap = keymap;
        win->xkb_state = xkb_state_new(win->keymap);
    }

    munmap(map_shm, size);
    close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
                                 uint32_t serial, struct wl_surface *surface,
                                 struct wl_array *keys) {
    //(void) wl_keyboard; (void) serial; (void) surface; (void) keys;
    auto ctx = (wl_context *) data;
    printf("keyboard: enter (focus)\n");
    for (auto w : ctx->windows) {
        if (w->surface == surface) {
            w->has_keyboard_focus = true;
            if (w->rw->on_keyboard_focus) {
                w->rw->on_keyboard_focus(w->rw, true);
            }
            if (w->on_render)
                w->on_render(w);
        }
    }
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
                                 uint32_t serial, struct wl_surface *surface) {
    //(void) wl_keyboard; (void) serial; (void) surface;
    printf("keyboard: leave (lost focus)\n");
    auto ctx = (wl_context *) data;
    for (auto w : ctx->windows) {
        if (w->surface == surface) {
            w->has_keyboard_focus = false;
            if (w->rw->on_keyboard_focus) {
                w->rw->on_keyboard_focus(w->rw, false);
            }
            if (w->on_render)
                w->on_render(w);
        }
    }
}


static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
                                uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    wl_context *ctx = (wl_context *) data;
    wl_window *win = nullptr;
    for (auto w : ctx->windows)
        if (w->has_keyboard_focus)
            win = w;
    if (!win) {
        for (auto w : ctx->windows)
            if (w->has_pointer_focus)
                win = w;
    }
    if (!win)
        return;
                                    
    //display *d = (display *) data;
    const char *st = (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? "pressed" : "released";
    printf("keyboard: key %u %s for %s", key, st, win->title.data());

    if (win->xkb_state) {
        // Wayland keycodes are +8 from evdev
        xkb_keysym_t sym = xkb_state_key_get_one_sym(win->xkb_state, key + 8);
        char buf[64];
        int n = xkb_keysym_get_name(sym, buf, sizeof(buf));
        if (n > 0) {
            printf(" -> %s", buf);
        } else {
            // try printable UTF-8
            char utf8[64];
            int len = xkb_keysym_to_utf8(sym, utf8, sizeof(utf8));
            if (len > 0) {
                printf(" -> '%s'", utf8);
            }
        }
    }
    printf("\n");

    if (win->on_render)
        win->on_render(win);
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t group) {
    (void)data; (void)wl_keyboard; (void)serial;
    (void)mods_depressed; (void)mods_latched; (void)mods_locked; (void)group;
    // Not printing modifiers in this minimal example
    // 
}

static void keyboard_handle_repeat_info(void *data,
                            		    struct wl_keyboard *wl_keyboard,
                            		    int32_t rate,
                            		    int32_t delay) {
                    		   
}


static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

/* ---- seat listener ---- */
static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    wl_context *d = (wl_context *) data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(d->pointer, &pointer_listener, d);
        if (d->shape_manager && d->pointer)
            d->shape_device = wp_cursor_shape_manager_v1_get_pointer(d->shape_manager, d->pointer);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->pointer) {
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->keyboard) {
        d->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(d->keyboard, &keyboard_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard) {
        wl_keyboard_destroy(d->keyboard);
        d->keyboard = NULL;
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

/* ---- xdg_wm_base ping handler ---- */
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping
};

/* ---- registry ---- */
static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface, uint32_t version) {
    wl_context *d = (wl_context *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        d->compositor = (wl_compositor *) wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        d->shm = (wl_shm *) wl_registry_bind(registry, id, &wl_shm_interface, 1);
        d->shm_format = WL_SHM_FORMAT_ARGB8888;
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        d->seat = (wl_seat *) wl_registry_bind(registry, id, &wl_seat_interface, 5);
        wl_seat_add_listener(d->seat, &seat_listener, d);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        d->wm_base = (xdg_wm_base *) wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(d->wm_base, &wm_base_listener, d);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        d->layer_shell = (zwlr_layer_shell_v1 *) wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface, 5);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        //d->output = (wl_output *) wl_registry_bind(registry, id, &wl_output_interface, 3);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        d->fractional_scale_manager = (wp_fractional_scale_manager_v1 *) wl_registry_bind(registry, id, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        d->viewporter = (wp_viewporter*) wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        d->shape_manager = (wp_cursor_shape_manager_v1 *) wl_registry_bind(registry, id, &wp_cursor_shape_manager_v1_interface, 1);
        if (d->shape_manager && d->pointer)
            d->shape_device = wp_cursor_shape_manager_v1_get_pointer(d->shape_manager, d->pointer);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t id) {
    (void)data; (void)registry; (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove
};

struct wl_context *wl_context_create(void) {
    wl_context *ctx = new wl_context;
    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        free(ctx);
        return NULL;
    }

    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, ctx);
    wl_display_roundtrip(ctx->display); // populate globals

    ctx->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    ctx->shm_format = WL_SHM_FORMAT_ARGB8888; // default; discover if needed
    return ctx;
}

void wl_window_destroy(struct wl_window *win) {
    if (!win) return;

    if (win->xdg_toplevel) xdg_toplevel_destroy(win->xdg_toplevel);
    if (win->xdg_surface) xdg_surface_destroy(win->xdg_surface);
    if (win->layer_surface) zwlr_layer_surface_v1_destroy(win->layer_surface);

    if (win->surface) wl_surface_destroy(win->surface);

    if (win->xkb_state) xkb_state_unref(win->xkb_state);
    if (win->keymap) xkb_keymap_unref(win->keymap);

    delete win;
}

void wl_context_destroy(struct wl_context *ctx) {
    if (!ctx) return;

    for (auto w : ctx->windows)
        wl_window_destroy(w); 

    if (ctx->keyboard) wl_keyboard_release(ctx->keyboard);
    if (ctx->pointer) wl_pointer_release(ctx->pointer);
    if (ctx->seat) wl_seat_release(ctx->seat);
    if (ctx->shm) wl_shm_destroy(ctx->shm);
    if (ctx->compositor) wl_compositor_destroy(ctx->compositor);
    if (ctx->wm_base) xdg_wm_base_destroy(ctx->wm_base);
    if (ctx->layer_shell) zwlr_layer_shell_v1_destroy(ctx->layer_shell);
    if (ctx->xkb_ctx) xkb_context_unref(ctx->xkb_ctx);

    wl_registry_destroy(ctx->registry);
    wl_display_disconnect(ctx->display);
    delete ctx;
}

int wake_pipe[2];

void windowing::add_fb(RawApp *app, int fd, std::function<void(PolledFunction pf)> func) {
    wl_context *ctx = nullptr;
    for (auto c : apps)
        if (c->id == app->id)
            ctx = c;
    if (!ctx)
        return;
    PolledFunction pf;
    pf.fd = fd;
    pf.func = func;
    ctx->polled_fds.push_back(pf);
}

void windowing::main_loop(RawApp *app) {
    wl_context *ctx = nullptr;
    for (auto c : apps)
        if (c->id == app->id)
            ctx = c;
    if (!ctx)
        return;

    bool need_flush = false;
    int fd = wl_display_get_fd(ctx->display);

    PolledFunction wayland_pf;
    wayland_pf.fd = fd;
    wayland_pf.func = [ctx, &need_flush](PolledFunction pf) {
        short re = pf.revents;

        if (re & POLLIN) {
            if (wl_display_prepare_read(ctx->display) == 0) {
                wl_display_read_events(ctx->display);
                wl_display_dispatch_pending(ctx->display);
            } else {
                wl_display_dispatch_pending(ctx->display);
            }
        }

        if (re & POLLOUT) {
            if (wl_display_flush(ctx->display) == 0)
                need_flush = false;
        }
    };

    PolledFunction wake_pf;
    wake_pf.fd = ctx->wake_pipe[0];
    wake_pf.func = [ctx](PolledFunction pf) {
        if (pf.revents & POLLIN) {
            char buf[64];
            read(ctx->wake_pipe[0], buf, sizeof buf);
            for (auto w : ctx->windows) {
                if (w->on_render) {
                    w->on_render(w);
                }
            }
            // wake simply interrupts the poll
        }
    };

    ctx->polled_fds.push_back(wayland_pf);
    ctx->polled_fds.push_back(wake_pf);

    while (ctx->running) {
        // Handle pre-poll Wayland events
        wl_display_dispatch_pending(ctx->display);

        if (wl_display_flush(ctx->display) < 0 && errno == EAGAIN)
            need_flush = true;

        // Build pollfds based on ctx->polled_fds
        std::vector<struct pollfd> pfds;
        pfds.reserve(ctx->polled_fds.size());

        for (auto &p : ctx->polled_fds) {
            short ev = POLLIN | POLLERR;
            if (p.fd == fd && need_flush)
                ev |= POLLOUT;
            pfds.push_back({ p.fd, ev, 0 });
        }

        // Block
        if (poll(pfds.data(), pfds.size(), -1) < 0)
            break;

        // Dispatch
        for (size_t i = 0; i < ctx->polled_fds.size(); i++) {
            auto &p = ctx->polled_fds[i];
            p.revents = pfds[i].revents;

            if (p.revents && p.func)
                p.func(p);  // call the polled handler
        }

        // ---- Application-level window cleanup ----

        for (int i = ctx->windows.size() - 1; i >= 0; i--) {
            auto win = ctx->windows[i];
            if (win->marked_for_closing) {
                wl_window_destroy(win);
                ctx->windows.erase(ctx->windows.begin() + i);
            }
        }

        bool any_keepers = false;
        for (auto w : ctx->windows) {
            if (w->keeper_of_life) {
                any_keepers = true;
                break;
            }
        }
        ctx->running = any_keepers;
    }
    /*
    while (ctx->running) {
        // Dispatch any already-queued events before blocking
        wl_display_dispatch_pending(ctx->display);

        // Try to flush before waiting
        if (wl_display_flush(ctx->display) < 0 && errno == EAGAIN)
            need_flush = true;

        short events = POLLIN | POLLERR;
        if (need_flush)
            events |= POLLOUT;

        struct pollfd pfds[2] = {
            { .fd = fd, .events = events },
            { .fd = ctx->wake_pipe[0], .events = POLLIN },
        };

        if (poll(pfds, 2, -1) < 0)
            break;

        if (pfds[1].revents & POLLIN) {
            char buf[64];
            read(ctx->wake_pipe[0], buf, sizeof buf);
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            if (wl_display_prepare_read(ctx->display) == 0) {
                wl_display_read_events(ctx->display);
                wl_display_dispatch_pending(ctx->display);
            } else {
                wl_display_dispatch_pending(ctx->display);
            }
        }

        if (pfds[0].revents & POLLOUT) {
            if (wl_display_flush(ctx->display) == 0)
                need_flush = false;
        }

        // Now handle your app-level window cleanup
        for (int i = ctx->windows.size() - 1; i >= 0; i--) {
            auto win = ctx->windows[i];
            if (win->marked_for_closing) {
                wl_window_destroy(win);
                ctx->windows.erase(ctx->windows.begin() + i);
            }
        }

        bool any_keepers_of_life = false;
        for (auto w : ctx->windows) {
            if (w->keeper_of_life) {
                any_keepers_of_life = true;
            }
        }
        ctx->running = any_keepers_of_life;  
        //notify(fz("ctx->running {}", ctx->running));
    }
    */

    //notify("app exiting");
    
    // 4. Cleanup
    wl_context_destroy(ctx);
}

RawApp *windowing::open_app() {
    auto ra = new RawApp;
    ra->id = unique_id++;
    
    struct wl_context *ctx = wl_context_create();
    ctx->id = ra->id;
    ctx->ra = ra;
    
    pipe2(ctx->wake_pipe, O_CLOEXEC | O_NONBLOCK);

    apps.push_back(ctx);
    
    return ra;
}

RawWindow *windowing::open_window(RawApp *app, WindowType type, RawWindowSettings settings) {
    wl_context *ctx = nullptr;
    for (auto c : apps)
        if (c->id == app->id)
            ctx = c;
    if (!ctx)
        return nullptr;
    
    auto rw = new RawWindow;
    rw->creator = app;
    rw->id = unique_id++;

    if (type == WindowType::NORMAL) {
        auto window = wl_window_create(ctx, settings.pos.w, settings.pos.h, settings.name.c_str());
        window->rw = rw;
        rw->cr = window->cr;
        window->id = rw->id;
        if (window->on_render)
            window->on_render(window);
        windows.push_back(window);
    }
    if (type == WindowType::DOCK) {
        auto window = wl_layer_window_create(ctx, settings.pos.w, settings.pos.h, ZWLR_LAYER_SHELL_V1_LAYER_TOP, settings.name.c_str());
        window->rw = rw;
        rw->cr = window->cr;
        window->id = rw->id;
        if (window->on_render)
            window->on_render(window);
        windows.push_back(window);        
    }
    auto window = windows[windows.size() - 1];
    
    return rw;
}

void windowing::wake_up(RawWindow *window) {
    wl_context *ctx = nullptr;
    for (auto c : apps)
        if (c->id == window->creator->id)
            ctx = c;
    if (!ctx)
        return;
    wl_window *win = nullptr;
    for (auto w : windows)
        if (w->id == window->id)
            win = w;
    if (!win)
        return;
    write(ctx->wake_pipe[1], "x", 1);
}

void windowing::redraw(RawWindow *window) {
    wl_context *ctx = nullptr;
    for (auto c : apps)
        if (c->id == window->creator->id)
            ctx = c;
    if (!ctx)
        return;
    wl_window *win = nullptr;
    for (auto w : windows)
        if (w->id == window->id)
            win = w;
    if (!win)
        return;
    write(ctx->wake_pipe[1], "x", 1);
    
    /*
    wl_event_loop* loop = wl_display_get_event_loop(ctx->display);

    wl_event_source* timer = wl_event_loop_add_timer(
        loop,
        [](void* data) -> int {
            auto win = (wl_window*)data;
            //win->on_render(win);
            return 0; // next timeout in ms
        },
        win);

    wl_event_source_timer_update(timer, 1);
    */
}

void windowing::close_window(RawWindow *window) {
    wl_context *ctx = nullptr;
    for (auto c : apps)
        if (c->id == window->creator->id)
            ctx = c;
    if (!ctx)
        return;
    wl_window *win = nullptr;
    for (auto w : windows)
        if (w->id == window->id)
            win = w;
    if (!win)
        return;
    win->marked_for_closing = true;
    write(ctx->wake_pipe[1], "x", 1);
}

void windowing::close_app(RawApp *app) {
    if (!app)
        return;
    wl_context *ctx = nullptr;
    for (auto c : apps)
        if (c->id == app->id)
            ctx = c;
    if (!ctx)
        return;
    for (auto w : ctx->windows) {
        windowing::close_window(w->rw); 
    }
    ctx->running = false;
    write(ctx->wake_pipe[1], "x", 1);
}


