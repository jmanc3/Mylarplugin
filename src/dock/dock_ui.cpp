#include "dock/dock_ui.h"

#include "dock/dock.h"
#include "client/windowing.h"
#include "heart.h"
#include "pango_font_cache.h"

#include <algorithm>
#include <cmath>
#include <pango/pango-layout.h>
#include <pango/pangocairo.h>

RGBA accent = RGBA(.0, .52, .9, 1);
RGBA border_color = RGBA(0.7, 0.7, 0.7, 1.0);

float get_icon_size(float dpi) {
    return 28 * dpi;
}

RawWindowSettings make_icon_anchored_popup_settings(Container *icon,
                                                   float dpi,
                                                   int popup_w,
                                                   int popup_h) {
    const int icon_x = (int) std::round(icon->real_bounds.x / dpi);
    const int icon_y = (int) std::round(icon->real_bounds.y / dpi);
    const int icon_w = std::max(1, (int) std::round(icon->real_bounds.w / dpi));
    const int icon_h = std::max(1, (int) std::round(icon->real_bounds.h / dpi));

    RawWindowSettings settings;
    settings.pos.w = popup_w;
    settings.pos.h = popup_h;
    settings.name = "Popup";
    settings.popup.use_explicit_anchor_rect = true;
    settings.popup.anchor_rect_x = icon_x;
    settings.popup.anchor_rect_y = icon_y;
    settings.popup.anchor_rect_w = icon_w;
    settings.popup.anchor_rect_h = icon_h;
    settings.popup.anchor = RawWindowSettings::PopupAnchor::BOTTOM;
    settings.popup.gravity = RawWindowSettings::PopupGravity::BOTTOM;
    settings.popup.use_offset = true;
    settings.popup.offset_y = -8;
    settings.popup.constraint_adjustment =
        RawWindowSettings::POPUP_CONSTRAINT_SLIDE_X |
        RawWindowSettings::POPUP_CONSTRAINT_SLIDE_Y |
        RawWindowSettings::POPUP_CONSTRAINT_FLIP_X |
        RawWindowSettings::POPUP_CONSTRAINT_FLIP_Y;
    return settings;
}

Bounds draw_text(drawing::Context *cr, int x, int y, std::string text, int size, bool draw, std::string font,
                 int wrap, int h, RGBA color, bool bold, int align) {
    auto layout = get_cached_pango_font(cr, set->font, size, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);

    pango_layout_set_text(layout, text.data(), text.size());
    pango_layout_set_alignment(layout, (PangoAlignment) align);
    if (wrap == -1) {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_NONE);
        pango_layout_set_width(layout, -1);
        pango_layout_set_height(layout, -1);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_NONE);
    } else {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width(layout, wrap * PANGO_SCALE);
        pango_layout_set_height(layout, h);
        if (h != -1)
            pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_MIDDLE);
    }
    set_argb(cr, color);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cr->move_to(std::round(x), std::round(y));
        cr->show_layout(layout);
    }
    return Bounds(ink.width, ink.height, logical.width, logical.height);
}

Bounds draw_text(drawing::Context *cr, int x, int y, std::string text, int size, bool draw, std::string font,
                 int wrap, int h, RGBA color) {
    auto layout = get_cached_pango_font(cr, set->font, size, PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_text(layout, text.data(), text.size());
    if (wrap == -1) {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_NONE);
        pango_layout_set_width(layout, -1);
        pango_layout_set_height(layout, -1);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_NONE);
    } else {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width(layout, wrap);
        pango_layout_set_height(layout, h);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_MIDDLE);
    }
    cr->set_source_rgba(color.r, color.g, color.b, color.a);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cr->move_to(std::round(x), std::round(y));
        cr->show_layout(layout);
    }
    return Bounds(ink.width, ink.height, logical.width, logical.height);
}

Bounds draw_text(drawing::Context *cr, Container *c, std::string text, int size, bool draw, std::string font,
                 int wrap, int h) {
    if (!cr)
        return {0, 0, 0, 0};
    auto layout = get_cached_pango_font(cr, set->font, size, PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_text(layout, text.data(), text.size());
    if (wrap == -1) {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_NONE);
        pango_layout_set_width(layout, -1);
        pango_layout_set_height(layout, -1);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_NONE);
    } else {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width(layout, wrap);
        pango_layout_set_height(layout, h);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_MIDDLE);
    }
    cr->set_source_rgba(1, 1, 1, 1);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cr->move_to(std::round(center_x(c, logical.width)), std::round(center_y(c, logical.height)));
        cr->show_layout(layout);
    }
    return Bounds(ink.width, ink.height, logical.width, logical.height);
}

void drawRoundedRect(drawing::Context *cr, double x, double y, double width, double height,
                     double radius, double stroke_width) {
    double half_stroke = stroke_width / 2.0;
    double adjusted_radius = std::fmin(radius, std::fmin(width, height) / 2.0);
    double inner_width = width - stroke_width;
    double inner_height = height - stroke_width - 1;

    if (inner_width <= 0 || inner_height <= 0)
        return;

    double adjusted_x = x + half_stroke;
    double adjusted_y = y + half_stroke;

    cr->new_path();

    cr->move_to(adjusted_x + adjusted_radius, adjusted_y);
    cr->line_to(adjusted_x + inner_width - adjusted_radius, adjusted_y);
    cr->arc(adjusted_x + inner_width - adjusted_radius, adjusted_y + adjusted_radius,
              adjusted_radius, -M_PI / 2, 0);
    cr->line_to(adjusted_x + inner_width, adjusted_y + inner_height - adjusted_radius);
    cr->arc(adjusted_x + inner_width - adjusted_radius, adjusted_y + inner_height - adjusted_radius,
              adjusted_radius, 0, M_PI / 2);
    cr->line_to(adjusted_x + adjusted_radius, adjusted_y + inner_height);
    cr->arc(adjusted_x + adjusted_radius, adjusted_y + inner_height - adjusted_radius,
              adjusted_radius, M_PI / 2, M_PI);
    cr->line_to(adjusted_x, adjusted_y + adjusted_radius);
    cr->arc(adjusted_x + adjusted_radius, adjusted_y + adjusted_radius,
              adjusted_radius, M_PI, 3 * M_PI / 2);

    cr->close_path();
    cr->set_line_width(stroke_width);
}

// Rounded outline shared by popup painting and clipping. The compositor shadow takes
// an integer pixel radius; use the same rounding here.
static void popup_background_path(drawing::Context *cr, const Bounds &bounds, double dpi, double inset) {
    const double radius = std::min(std::round(dock::popup_corner_radius * dpi), std::floor(std::min(bounds.w, bounds.h) * .5));
    const double r = std::max(0.0, radius - inset);
    const double left = bounds.x + inset;
    const double top = bounds.y + inset;
    const double right = bounds.x + bounds.w - inset;
    const double bottom = bounds.y + bounds.h - inset;
    cr->new_path();
    cr->arc(right - r, top + r, r, -M_PI / 2, 0);
    cr->arc(right - r, bottom - r, r, 0, M_PI / 2);
    cr->arc(left + r, bottom - r, r, M_PI / 2, M_PI);
    cr->arc(left + r, top + r, r, M_PI, 3 * M_PI / 2);
    cr->close_path();
}

void paint_popup_background(drawing::Context *cr, const Bounds &bounds, double dpi,
                            RGBA background, RGBA outline) {
    if (bounds.w <= 0 || bounds.h <= 0)
        return;
    const double stroke = std::min(dpi, std::min(bounds.w, bounds.h));
    cr->save();
    set_argb(cr, background);
    popup_background_path(cr, bounds, dpi, 0);
    cr->fill();
    set_argb(cr, outline);
    cr->set_line_width(stroke);
    // Inset the stroke and its radius equally so its outer edge matches the fill.
    popup_background_path(cr, bounds, dpi, stroke * .5);
    cr->stroke();
    cr->restore();
}

void clip_to_popup_background(drawing::Context *cr, const Bounds &bounds, double dpi) {
    if (bounds.w <= 0 || bounds.h <= 0)
        return;
    // Stay inside the outline so clipped content never paints over the border.
    popup_background_path(cr, bounds, dpi, std::min(dpi, std::min(bounds.w, bounds.h)));
    cr->clip_path();
}

void paint_slider(Container *root, Container *c, drawing::Context *cr, float dpi, float value, float meter) {
    static constexpr float dotr = 17;
    static constexpr float thickness = 8;
    float final_thickness = std::round(thickness * dpi);
    float final_dotr = std::round(dotr * dpi);
    {
        auto b = c->real_bounds;
        b.y += b.h * .5 - final_thickness * .5;
        b.h = final_thickness;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, final_thickness * .45, 1.0);
        set_argb(cr, {0, 0, 0, .1});
        cr->fill();

        double meter_w = b.w * std::clamp(value, 0.0f, 1.0f) * std::clamp(meter, 0.0f, 1.0f);
        if (meter_w > 0.5) {
            cr->save();
            drawRoundedRect(cr, b.x, b.y, b.w, b.h, final_thickness * .45, 1.0);
            cr->clip_path();
            // A pill whose right end sits at the peak; when it is shorter than it is tall,
            // its left end slides out of the track and is clipped away.
            double pill_w = std::max(meter_w, (double) b.h);
            double pill_x = b.x + meter_w - pill_w;
            double rad = b.h * .5;
            cr->new_path();
            cr->arc(pill_x + pill_w - rad, b.y + rad, rad, -M_PI / 2, M_PI / 2);
            cr->arc(pill_x + rad, b.y + rad, rad, M_PI / 2, 3 * M_PI / 2);
            cr->close_path();
            auto color = accent;
            color.a = .55;
            set_argb(cr, color);
            cr->fill();
            cr->restore();
        }
    }

    {
        auto b = c->real_bounds;
        b.x += b.w * value - final_dotr * .5;
        b.w = final_dotr;
        b.y += b.h * .5 - final_dotr * .5;
        b.h = final_dotr;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, final_dotr * .45, 1.0);
        set_argb(cr, {1, 1, 1, 1});
        cr->fill();
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, final_dotr * .45, 1.0);
        auto bigger = b;
        bigger.scale_from_center(1.4);
        bool dot_intersected = bounds_contains(bigger, root->mouse_current_x, root->mouse_current_y);
        if ((c->state.mouse_hovering || c->state.mouse_pressing) && dot_intersected) {
            b.scale_from_center(.7);
        } else {
            b.scale_from_center(.55);
        }
        set_argb(cr, {0, 0, 0, .1});
        cr->stroke();
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, final_dotr * .45, 1.0);
        set_argb(cr, accent);
        cr->fill();
    }
}

Container *make_self_sizing_slider(Container *root,
                                   std::function<std::string(Container *)> left_text,
                                   std::function<std::string(Container *)> right_text,
                                   std::function<void(Container *, float)> on_value_change,
                                   std::function<float(Container *)> get_value,
                                   std::function<MylarWindow *(Dock *)> get_window) {
    static float height = 32;
    auto slider_parent = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto left = slider_parent->child(FILL_SPACE, FILL_SPACE);
    auto slider = slider_parent->child(FILL_SPACE, FILL_SPACE);
    auto right = slider_parent->child(FILL_SPACE, FILL_SPACE);

    slider_parent->pre_layout = [get_window](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        c->wanted_bounds.h = height * dpi;
    };
    left->pre_layout = [get_window](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        c->wanted_bounds.h = height * dpi;
        c->wanted_bounds.w = height * dpi;
    };
    left->when_paint = [left_text, get_window](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        auto cr = window->raw_window->drawing_context;
        auto left = left_text(c);
        auto b = draw_text(cr, 0, 0, left, 12 * dpi, false, icon_font);
        draw_text(cr,
            c->real_bounds.x + c->real_bounds.w * .5 - b.w * .5,
            c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, left, 12 * dpi, true, icon_font, -1, -1, {0, 0, 0, 1});
    };
    right->pre_layout = [get_window](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        c->wanted_bounds.h = height * dpi;
        c->wanted_bounds.w = height * dpi * 1.2;
    };
    right->when_paint = [right_text, get_window](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        auto cr = window->raw_window->drawing_context;
        auto right = right_text(c);
        auto b = draw_text(cr, 0, 0, right, 12 * dpi, false);
        draw_text(cr,
            c->real_bounds.x + c->real_bounds.w - b.w,
            c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, right, 12 * dpi, true, set->font, -1, -1, {0, 0, 0, 1});
    };
    slider->when_paint = [get_window, get_value](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        auto cr = window->raw_window->drawing_context;

        paint_slider(root, c, cr, dpi, get_value(c));
    };
    slider->when_clicked = [on_value_change](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        on_value_change(c, scalar);
    };
    slider->when_drag_start = [on_value_change](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        on_value_change(c, scalar);
    };
    slider->when_drag = [on_value_change](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        on_value_change(c, scalar);
    };
    slider->when_drag_end = [on_value_change](Container *root, Container *c) {
        float scalar = (root->mouse_current_x - c->real_bounds.x) / c->real_bounds.w;
        on_value_change(c, scalar);
    };
    slider->when_fine_scrolled = [get_value, on_value_change](Container *root, Container *c, double scroll_x, double scroll_y, bool came_from_touchpad) {
        auto value = get_value(c) * 100.0f;
        if (came_from_touchpad) {
            value += scroll_y * .25;
        } else {
            // When changing volume with mouse, round to next, 0, 5, 3, or 7
            int full = std::round(value);
            int last_digit;
            do {
                if (scroll_y > 0) {
                    full++;
                } else if (scroll_y < 0) {
                    full--;
                }
                last_digit = full % 10;
            } while (!(last_digit == 0 || last_digit == 5));

            value = ((double) full);
        }
        if (value > 100)
            value = 100;
        if (value < 0)
            value = 0;
        on_value_change(c, value * .01);
    };
    return slider_parent;
}

Container *make_self_sizing_label(Container *root, std::string text, int size,
                                  std::function<MylarWindow *(Dock *)> get_window) {
    auto label = root->child(FILL_SPACE, FILL_SPACE);
    label->pre_layout = [text, size, get_window](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        auto cr = window->raw_window->drawing_context;
        auto bounds = draw_text(cr, c, text, size * dpi, false);
        c->wanted_bounds.h = bounds.h;
    };
    label->when_paint = [text, size, get_window](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = get_window(dock);
        auto dpi = window->raw_window->dpi;
        auto cr = window->raw_window->drawing_context;
        draw_text(cr, c->real_bounds.x, c->real_bounds.y, text, size * dpi, true, set->font, -1, -1, {0, 0, 0, 1});
    };
    return label;
}
