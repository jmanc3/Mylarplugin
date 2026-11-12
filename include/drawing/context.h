#pragma once

#include <cairo.h>
#include <pango/pangocairo.h>
#include <memory>

struct RGBA;

namespace drawing {
struct Rect { double x, y, width, height; };

// Stateful immediate-mode drawing in device pixels. Method names and semantics
// mirror Cairo so painting code reads the same as before:
//  - save/restore preserve drawing state (clip, transform, source), not the path
//  - fill, stroke and clip consume the path; *_preserve keep it
//  - push_group isolates drawing; pop_group_to_source makes the layer the source
//  - paint_source covers the current clip with the current source
//
// Text is the only thing rasterized by Cairo/Pango (OpenGL backend: once per
// cache miss, uploaded as a texture). Every shape, color, gradient, image and
// composite is drawn by the backend itself.
class Context {
  public:
    virtual ~Context() = default;

    void rounded_rectangle(Rect bounds, double radius);

    virtual void save() = 0;
    virtual void restore() = 0;
    virtual void translate(double x, double y) = 0;
    virtual void scale(double x, double y) = 0;

    virtual void new_path() = 0;
    virtual void new_sub_path() = 0;
    virtual void move_to(double x, double y) = 0;
    virtual void line_to(double x, double y) = 0;
    virtual void arc(double x, double y, double radius, double start, double end) = 0;
    virtual void close_path() = 0;
    virtual void rectangle(double x, double y, double width, double height) = 0;

    virtual void fill() = 0;
    virtual void fill_preserve() = 0;
    virtual void stroke() = 0;
    virtual void stroke_preserve() = 0;
    // cairo_clip. (Not named clip(): heart.h has a function-like clip macro.)
    virtual void clip_path() = 0;
    virtual void reset_clip() = 0;
    // cairo_paint / cairo_paint_with_alpha. (Not named paint(): heart.h has a paint macro.)
    virtual void paint_source(double alpha = 1) = 0;

    void set_color(const RGBA &color);
    virtual void set_source_rgba(double r, double g, double b, double a) = 0;
    void set_source_rgb(double r, double g, double b) { set_source_rgba(r, g, b, 1); }
    // Two-stop linear gradient in user coordinates (colors are not premultiplied).
    void set_linear_gradient(double x0, double y0, double x1, double y1, const RGBA &from, const RGBA &to);
    virtual void set_linear_gradient(double x0, double y0, double x1, double y1, const double from[4],
                                     const double to[4]) = 0;
    // Like cairo_set_source_surface: the surface becomes the source, its top
    // left corner at (x, y) in user space. The surface must stay alive while it
    // is the source. If its pixels are changed in place, call invalidate_surface.
    virtual void set_source_surface(cairo_surface_t *surface, double x, double y) = 0;

    virtual void set_line_width(double width) = 0;
    virtual double get_line_width() = 0;
    virtual void set_line_cap(cairo_line_cap_t cap) = 0;
    virtual void set_line_join(cairo_line_join_t join) = 0;
    virtual void set_dash(const double *dashes, int count, double offset) = 0;
    virtual void set_fill_rule(cairo_fill_rule_t rule) = 0;
    // CLEAR, SOURCE, OVER and ADD are supported; anything else acts as OVER.
    virtual void set_operator(cairo_operator_t op) = 0;

    virtual void push_group() = 0;
    virtual void pop_group_to_source() = 0;

    // Like pango_cairo_show_layout: draws at the current point with the current color.
    virtual void show_layout(PangoLayout *layout) = 0;
    // Like pango_cairo_layout_path: appends the glyph outlines at the current point.
    virtual void layout_path(PangoLayout *layout) = 0;

    // Called once per frame by the windowing layer.
    virtual void begin_frame() {}
    virtual void flush() = 0;
};

// Software fallback. The caller owns the ARGB32 buffer and keeps it alive until
// the context is destroyed.
std::unique_ptr<Context> create_cairo_context(unsigned char *data, int width, int height, int stride);

// Tells every backend cache that the surface's pixels changed in place.
void invalidate_surface(cairo_surface_t *surface);

// A thread-local Cairo context for creating/updating Pango layouts and measuring
// text. Never draw into it.
cairo_t *scratch_cairo();

// A thread-local 1x1 throwaway drawing context. Windows hand it out until their
// real context exists, so measuring code never sees a null context.
Context *placeholder_context();
} // namespace drawing
