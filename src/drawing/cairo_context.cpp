#include "drawing/context.h"

#include <stdexcept>

namespace drawing {
namespace {
// Software fallback for machines without EGL / OpenGL ES 3.
class CairoContext final : public Context {
    cairo_t *cr_;

  public:
    explicit CairoContext(cairo_t *cr) : cr_(cr) {}
    ~CairoContext() override { cairo_destroy(cr_); }
    void save() override { cairo_save(cr_); }
    void restore() override { cairo_restore(cr_); }
    void translate(double x, double y) override { cairo_translate(cr_, x, y); }
    void scale(double x, double y) override { cairo_scale(cr_, x, y); }
    void new_path() override { cairo_new_path(cr_); }
    void new_sub_path() override { cairo_new_sub_path(cr_); }
    void move_to(double x, double y) override { cairo_move_to(cr_, x, y); }
    void line_to(double x, double y) override { cairo_line_to(cr_, x, y); }
    void arc(double x, double y, double radius, double start, double end) override {
        cairo_arc(cr_, x, y, radius, start, end);
    }
    void close_path() override { cairo_close_path(cr_); }
    void rectangle(double x, double y, double width, double height) override {
        cairo_rectangle(cr_, x, y, width, height);
    }
    void fill() override { cairo_fill(cr_); }
    void fill_preserve() override { cairo_fill_preserve(cr_); }
    void stroke() override { cairo_stroke(cr_); }
    void stroke_preserve() override { cairo_stroke_preserve(cr_); }
    void clip_path() override { cairo_clip(cr_); }
    void reset_clip() override { cairo_reset_clip(cr_); }
    void paint_source(double alpha) override {
        if (alpha >= 1)
            cairo_paint(cr_);
        else
            cairo_paint_with_alpha(cr_, alpha);
    }
    void set_source_rgba(double r, double g, double b, double a) override { cairo_set_source_rgba(cr_, r, g, b, a); }
    void set_linear_gradient(double x0, double y0, double x1, double y1, const double from[4],
                             const double to[4]) override {
        auto gradient = cairo_pattern_create_linear(x0, y0, x1, y1);
        cairo_pattern_add_color_stop_rgba(gradient, 0, from[0], from[1], from[2], from[3]);
        cairo_pattern_add_color_stop_rgba(gradient, 1, to[0], to[1], to[2], to[3]);
        cairo_set_source(cr_, gradient);
        cairo_pattern_destroy(gradient);
    }
    void set_source_surface(cairo_surface_t *surface, double x, double y) override {
        cairo_set_source_surface(cr_, surface, x, y);
    }
    void set_line_width(double width) override { cairo_set_line_width(cr_, width); }
    double get_line_width() override { return cairo_get_line_width(cr_); }
    void set_line_cap(cairo_line_cap_t cap) override { cairo_set_line_cap(cr_, cap); }
    void set_line_join(cairo_line_join_t join) override { cairo_set_line_join(cr_, join); }
    void set_dash(const double *dashes, int count, double offset) override {
        cairo_set_dash(cr_, dashes, count, offset);
    }
    void set_fill_rule(cairo_fill_rule_t rule) override { cairo_set_fill_rule(cr_, rule); }
    void set_operator(cairo_operator_t op) override { cairo_set_operator(cr_, op); }
    void push_group() override { cairo_push_group(cr_); }
    void pop_group_to_source() override { cairo_pop_group_to_source(cr_); }
    void show_layout(PangoLayout *layout) override { pango_cairo_show_layout(cr_, layout); }
    void layout_path(PangoLayout *layout) override { pango_cairo_layout_path(cr_, layout); }
    void begin_frame() override {
        cairo_save(cr_);
        cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr_);
        cairo_restore(cr_);
    }
    void flush() override { cairo_surface_flush(cairo_get_target(cr_)); }
};
} // namespace

std::unique_ptr<Context> create_cairo_context(unsigned char *data, int width, int height, int stride) {
    if (!data || width <= 0 || height <= 0 || stride < cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width))
        throw std::invalid_argument("Invalid drawing buffer");
    auto surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width, height, stride);
    auto cr = cairo_create(surface);
    cairo_surface_destroy(surface);
    const auto status = cairo_status(cr);
    if (status != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        throw std::runtime_error(cairo_status_to_string(status));
    }
    return std::make_unique<CairoContext>(cr);
}
} // namespace drawing
