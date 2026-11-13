#include "drawing_internal.h"
#include "hypriso.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>

namespace drawing {
void Context::rounded_rectangle(Rect b, double radius) {
    constexpr double pi = std::numbers::pi;
    const double r = std::clamp(radius, 0.0, std::max(0.0, std::min(b.width, b.height) / 2));
    new_sub_path();
    arc(b.x + b.width - r, b.y + r, r, -pi / 2, 0);
    arc(b.x + b.width - r, b.y + b.height - r, r, 0, pi / 2);
    arc(b.x + r, b.y + b.height - r, r, pi / 2, pi);
    arc(b.x + r, b.y + r, r, pi, 3 * pi / 2);
    close_path();
}

void Context::set_color(const RGBA &color) { set_source_rgba(color.r, color.g, color.b, color.a); }

void Context::set_linear_gradient(double x0, double y0, double x1, double y1, const RGBA &from, const RGBA &to) {
    const double a[4] = {from.r, from.g, from.b, from.a};
    const double b[4] = {to.r, to.g, to.b, to.a};
    set_linear_gradient(x0, y0, x1, y1, a, b);
}

namespace {
const cairo_user_data_key_t token_key{};
std::mutex token_mutex;

void destroy_token(void *data) { delete static_cast<std::shared_ptr<const char> *>(data); }
} // namespace

std::shared_ptr<const char> surface_token(cairo_surface_t *surface) {
    std::lock_guard lock(token_mutex);
    auto token = static_cast<std::shared_ptr<const char> *>(cairo_surface_get_user_data(surface, &token_key));
    if (token)
        return *token;
    auto created = new std::shared_ptr<const char>(std::make_shared<const char>(0));
    if (cairo_surface_set_user_data(surface, &token_key, created, destroy_token) != CAIRO_STATUS_SUCCESS) {
        // Uncacheable (out of memory): a fresh token each time forces an upload.
        auto result = *created;
        delete created;
        return result;
    }
    return *created;
}

void invalidate_surface(cairo_surface_t *surface) {
    if (!surface)
        return;
    std::lock_guard lock(token_mutex);
    // Replacing the user data runs destroy_token on the old token, expiring it.
    cairo_surface_set_user_data(surface, &token_key, nullptr, nullptr);
}

// These caches are plain thread_local pointers on purpose: a thread_local with a
// non-trivial destructor registers a thread-exit destructor, and glibc refuses to
// unload this plugin while any live thread (e.g. the compositor thread, which
// reaches scratch_cairo() through get_cached_pango_font) has one pending. That
// left the old code mapped across "hyprctl plugin unload" and mixed it with the
// next load. Threads release them with cleanup_thread_resources().
namespace {
struct Scratch {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surface);
    ~Scratch() {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }
};
thread_local Scratch *scratch = nullptr;
thread_local uint32_t placeholder_pixel = 0;
thread_local Context *placeholder = nullptr;
}

cairo_t *scratch_cairo() {
    // ARGB32 keeps the font options (and therefore metrics) identical to the
    // window buffers the layouts were historically created against.
    if (!scratch)
        scratch = new Scratch;
    return scratch->cr;
}

Context *placeholder_context() {
    if (!placeholder)
        placeholder = create_cairo_context(reinterpret_cast<unsigned char *>(&placeholder_pixel), 1, 1, 4).release();
    return placeholder;
}

void cleanup_thread_resources() {
    delete placeholder;
    placeholder = nullptr;
    delete scratch;
    scratch = nullptr;
}

std::string layout_key(PangoLayout *layout) {
    std::string key;
    int length = 0;
    const char *text = pango_layout_get_text(layout);
    key.append(text ? text : "");
    key.push_back('\x1f');
    if (auto desc = pango_layout_get_font_description(layout)) {
        auto s = pango_font_description_to_string(desc);
        key.append(s);
        g_free(s);
    }
    key.push_back('\x1f');
    if (auto attrs = pango_layout_get_attributes(layout)) {
        auto s = pango_attr_list_to_string(attrs);
        key.append(s);
        g_free(s);
    }
    (void)length;
    char numbers[256];
    std::snprintf(numbers, sizeof numbers, "\x1f%d,%d,%d,%d,%d,%d,%d,%g,%d,%d,%d,%d,%d",
                  pango_layout_get_width(layout), pango_layout_get_height(layout), (int)pango_layout_get_wrap(layout),
                  (int)pango_layout_get_ellipsize(layout), (int)pango_layout_get_alignment(layout),
                  pango_layout_get_indent(layout), pango_layout_get_spacing(layout),
                  (double)pango_layout_get_line_spacing(layout), (int)pango_layout_get_justify(layout),
                  (int)pango_layout_get_justify_last_line(layout), (int)pango_layout_get_single_paragraph_mode(layout),
                  (int)pango_layout_get_auto_dir(layout), pango_layout_get_tabs(layout) != nullptr);
    key.append(numbers);
    return key;
}

bool layout_has_own_colors(PangoLayout *layout) {
    if (auto attrs = pango_layout_get_attributes(layout)) {
        auto s = pango_attr_list_to_string(attrs);
        bool colored = std::strstr(s, "foreground") != nullptr;
        g_free(s);
        if (colored)
            return true;
    }
    const char *text = pango_layout_get_text(layout);
    if (!text)
        return false;
    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c >= 0x1F000 || c == 0xFE0F)
            return true;
    }
    return false;
}

TextRaster rasterize_layout(PangoLayout *layout, bool rgba, const double color[4]) {
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    TextRaster result;
    result.rgba = rgba;
    if (ink.width <= 0 || ink.height <= 0)
        return result;
    // Keep bearings (including negative overhang) plus a filtering gutter.
    result.x = ink.x - 1;
    result.y = ink.y - 1;
    result.width = ink.width + 2;
    result.height = ink.height + 2;
    auto surface = cairo_image_surface_create(rgba ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_A8, result.width,
                                              result.height);
    auto cr = cairo_create(surface);
    if (rgba)
        cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    else
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, -result.x, -result.y);
    pango_cairo_show_layout(cr, layout);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const auto data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    if (!rgba) {
        result.pixels.resize(size_t(result.width) * result.height);
        for (int y = 0; y < result.height; ++y)
            std::memcpy(result.pixels.data() + size_t(y) * result.width, data + size_t(y) * stride, result.width);
    } else {
        result.pixels.resize(size_t(result.width) * result.height * 4);
        for (int y = 0; y < result.height; ++y) {
            auto row = reinterpret_cast<const uint32_t *>(data + size_t(y) * stride);
            for (int x = 0; x < result.width; ++x) {
                auto pixel = row[x];
                auto out = result.pixels.data() + (size_t(y) * result.width + x) * 4;
                out[0] = pixel >> 16;
                out[1] = pixel >> 8;
                out[2] = pixel;
                out[3] = pixel >> 24;
            }
        }
    }
    cairo_surface_destroy(surface);
    return result;
}
} // namespace drawing
