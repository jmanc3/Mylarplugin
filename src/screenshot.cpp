#include "screenshot.h"

#include "heart.h"
#include "overview.h"
#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

static bool showing = true;
static int mode = 0; // 1 rectangle, 2 window, 3 colorpick, 4 draw
static constexpr float DRAW_BRUSH_MIN = 2.0f;
static constexpr float DRAW_BRUSH_MAX = 40.0f;
static constexpr float DRAW_BRUSH_DEFAULT = 5.0f;
static constexpr float DRAW_ERASE_CURSOR_SCALE = 7.0f;

struct DrawPoint {
    float x = 0;
    float y = 0;
};

struct DrawStroke {
    std::vector<DrawPoint> points;
    RGBA color = {1, 0, 0, 1};

    bool empty() const { return points.empty(); }
    size_t size() const { return points.size(); }
    const DrawPoint &operator[](size_t i) const { return points[i]; }
    void push_back(DrawPoint p) { points.push_back(p); }
    void clear() { points.clear(); }
};
using DrawStrokes = std::vector<DrawStroke>;

// The colors offered next to the "Drawing" bubble; red is the default.
static const RGBA draw_palette[] = {
    {1, 0, 0, 1},
    {1, .55, 0, 1},
    {1, .85, 0, 1},
    {.12, .75, .25, 1},
    {.1, .45, 1, 1},
    {.62, .25, .95, 1},
};
static constexpr int DRAW_PALETTE_SIZE = sizeof(draw_palette) / sizeof(draw_palette[0]);

static DrawStrokes draw_strokes;
static bool draw_active = false;
static bool draw_erasing = false;
static float draw_brush_size = DRAW_BRUSH_DEFAULT;
static int draw_bubble_corner = 0;
static int draw_color_index = 0;
static bool draw_picking_color = false; // the current press landed on a color box, so it doesn't draw
static std::vector<Bounds> draw_color_boxes; // global logical bounds of the color boxes, from the last paint
static Bounds draw_copy_button; // global logical bounds of the "Copy (Ctrl+C)" button, from the last paint
static bool draw_pressing_copy = false; // the current press landed on the copy button
static std::vector<DrawStrokes> draw_undo;
static std::vector<DrawStrokes> draw_redo;
static constexpr size_t DRAW_HISTORY_LIMIT = 100;

static float draw_active_brush_size() {
    return draw_erasing ? draw_brush_size * DRAW_ERASE_CURSOR_SCALE : draw_brush_size;
}

static void draw_begin_action() {
    draw_undo.push_back(draw_strokes);
    if (draw_undo.size() > DRAW_HISTORY_LIMIT)
        draw_undo.erase(draw_undo.begin());
    draw_redo.clear();
}

static void draw_undo_action() {
    if (draw_undo.empty())
        return;

    draw_redo.push_back(std::move(draw_strokes));
    draw_strokes = std::move(draw_undo.back());
    draw_undo.pop_back();
}

static void draw_redo_action() {
    if (draw_redo.empty())
        return;

    draw_undo.push_back(std::move(draw_strokes));
    draw_strokes = std::move(draw_redo.back());
    draw_redo.pop_back();
}

static void draw_erase_at(float x, float y) {
    const float radius = std::max(1.0f, draw_active_brush_size() * .5f);
    const float radius2 = radius * radius;
    DrawStrokes result;

    for (const auto &stroke : draw_strokes) {
        if (stroke.empty())
            continue;

        DrawStroke piece;
        piece.color = stroke.color;
        auto flush = [&]() {
            if (piece.empty())
                return;
            result.push_back(piece);
            piece.clear();
        };
        auto sample = [&](float px, float py) {
            const float dx = px - x;
            const float dy = py - y;
            if (dx * dx + dy * dy <= radius2)
                flush();
            else
                piece.push_back({px, py});
        };

        sample(stroke[0].x, stroke[0].y);
        for (size_t i = 1; i < stroke.size(); i++) {
            const auto &a = stroke[i - 1];
            const auto &b = stroke[i];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const int steps = std::max(1, (int) std::ceil(dist));
            for (int j = 1; j <= steps; j++) {
                const float t = (float) j / (float) steps;
                sample(a.x + dx * t, a.y + dy * t);
            }
        }
        flush();
    }

    draw_strokes = std::move(result);
}

static int draw_color_box_at(float x, float y) {
    for (int i = 0; i < (int) draw_color_boxes.size(); i++) {
        auto b = draw_color_boxes[i];
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h)
            return i;
    }
    return -1;
}

static bool draw_copy_button_at(float x, float y) {
    auto b = draw_copy_button;
    return b.w > 0 && x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}

static void draw_start_stroke(float x, float y) {
    draw_begin_action();
    draw_active = true;
    draw_erasing = false;
    draw_strokes.push_back({});
    draw_strokes.back().color = draw_palette[draw_color_index];
    draw_strokes.back().push_back({x, y});
}

// Paints the strokes onto a PNG of the monitor (the drawing only exists on screen otherwise).
static void draw_strokes_onto_png(const std::string &path, const DrawStrokes &strokes, Bounds monitor, float scale, float brush) {
    if (strokes.empty())
        return;
    auto surface = cairo_image_surface_create_from_png(path.c_str());
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return;
    }
    auto cr = cairo_create(surface);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, std::max(1.0f, brush * scale));
    for (const auto &stroke : strokes) {
        if (stroke.empty())
            continue;
        cairo_set_source_rgba(cr, stroke.color.r, stroke.color.g, stroke.color.b, stroke.color.a);
        cairo_new_path(cr);
        cairo_move_to(cr, (stroke[0].x - monitor.x) * scale, (stroke[0].y - monitor.y) * scale);
        // A lone point still leaves a dot thanks to the round caps.
        cairo_line_to(cr, (stroke[0].x - monitor.x) * scale, (stroke[0].y - monitor.y) * scale);
        for (size_t i = 1; i < stroke.size(); i++)
            cairo_line_to(cr, (stroke[i].x - monitor.x) * scale, (stroke[i].y - monitor.y) * scale);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_destroy(surface);
}

// Ctrl+C: copies the selected rectangle (or the whole monitor without one) to the clipboard.
static void copy_screenshot_to_clipboard(bool has_selection, Bounds selection) {
    auto rid = hypriso->monitor_from_cursor();
    auto mb = bounds_monitor(rid);
    float s = scale(rid);
    Bounds region;
    if (has_selection) {
        region = fixed_box(selection.x, selection.y, selection.w, selection.h);
        region.x -= mb.x;
        region.y -= mb.y;
        region.scale(s);
        if (region.w < 1 || region.h < 1)
            region = Bounds();
    }
    DrawStrokes strokes;
    if (mode == 4)
        strokes = draw_strokes;
    float brush = draw_brush_size;
    static const std::string path = "/tmp/mylar-screenshot-clipboard.png";
    hypriso->save_monitor_to_png(rid, path, region, [strokes = std::move(strokes), mb, s, brush]() {
        draw_strokes_onto_png(path, strokes, mb, s, brush);
        system(fz("wl-copy --type image/png < '{}'", path).c_str());
    });
    notify("Screenshot copied to clipboard");
}

Container *label(Container *parent, std::string icon, std::string text) {
    static float label_h = 20;
    static float pad = 10;
    
    auto full = parent->child(0, 0);
    full->parent_bounds_limit_input_bounds = false;
    full->pre_layout = [icon, text](Container *actual_root, Container *c, const Bounds &b) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (rid != hypriso->monitor_from_cursor())
            return;
        auto info = gen_text_texture(icon_font, icon, label_h * s, {1, 1, 1, 1});
        free_text_texture(info.id); 
        auto info2 = gen_text_texture(set->font, text, label_h * s, {1, 1, 1, 1});
        free_text_texture(info2.id); 
        c->wanted_bounds.w = info.w + info2.w + pad * s;
        c->wanted_bounds.h = std::max(info.h, info2.h) + pad * s;
    };
    full->when_paint = [icon, text](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        renderfix

        if (c->state.mouse_pressing) {
            rect(c->real_bounds, {.4, .4, .4, .7}, 0, 0, 2.0, true);
        } else if (c->state.mouse_hovering) {
            rect(c->real_bounds, {.3, .3, .3, .7}, 0, 0, 2.0, true);
        } else {
            rect(c->real_bounds, {.2, .2, .2, .7}, 0, 0, 2.0, true);
        }
        float xoff = pad * s * 2;
        {
            auto info = gen_text_texture(icon_font, icon, label_h * s, {1, 1, 1, 1});
            draw_texture(info, {c->real_bounds.x + pad * s + xoff, 
                               c->real_bounds.y + c->real_bounds.h * .5 - info.h * .5, 
                               (double) info.w, (double) info.h});
            xoff += info.w;
            free_text_texture(info.id); 
        }
        xoff += 8 * s;
        {
            auto info = gen_text_texture(set->font, text, label_h * s, {1, 1, 1, 1});
            draw_texture(info, {c->real_bounds.x + pad * s + xoff, 
                               c->real_bounds.y + c->real_bounds.h * .5 - info.h * .5, 
                               (double) info.w, (double) info.h});
            free_text_texture(info.id); 
        }
    };
    return full;
}

void actual_open_screenshot_tool() {
    //setCursorImageUntilUnset("crosshair");
    auto tool = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    tool->custom_type = (int) TYPE::SCREENSHOT_TOOL;
    tool->automatically_paint_children = false;
    consume_everything(tool);

    auto type = tool->child(::hbox, FILL_SPACE, FILL_SPACE);
    type->spacing = 20;

    static Bounds rect_selection;
    rect_selection = Bounds();
    static bool rect_showing = false;
    rect_showing = false;
    draw_strokes.clear();
    draw_active = false;
    draw_erasing = false;
    draw_brush_size = DRAW_BRUSH_DEFAULT;
    draw_bubble_corner = 0;
    draw_color_index = 0;
    draw_picking_color = false;
    draw_color_boxes.clear();
    draw_copy_button = Bounds();
    draw_pressing_copy = false;
    draw_undo.clear();
    draw_redo.clear();
    
    auto select_box = tool->child(::hbox, FILL_SPACE, FILL_SPACE);
    select_box->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        
        if (stage != (int) STAGE::RENDER_PRE_CURSOR)
            return;
        if (mode == 4)
            return;
        renderfix
        auto mb = bounds_monitor(rid);
        mb.scale(s);
        mb.round();

        auto sel = c->real_bounds;
        auto sel_left = std::max(sel.x, mb.x);
        auto sel_top = std::max(sel.y, mb.y);
        auto sel_right = std::min(sel.x + sel.w, mb.x + mb.w);
        auto sel_bottom = std::min(sel.y + sel.h, mb.y + mb.h);
        const bool has_selection = sel_right > sel_left && sel_bottom > sel_top;

        if (has_selection) {
            // Darken everything outside the selected region.
            rect({mb.x, mb.y, mb.w, sel_top - mb.y}, {0, 0, 0, .3});
            rect({mb.x, sel_top, sel_left - mb.x, sel_bottom - sel_top}, {0, 0, 0, .3});
            rect({sel_right, sel_top, (mb.x + mb.w) - sel_right, sel_bottom - sel_top}, {0, 0, 0, .3});
            rect({mb.x, sel_bottom, mb.w, (mb.y + mb.h) - sel_bottom}, {0, 0, 0, .3});
        }

        border(c->real_bounds, {1, 1, 1, 1}, 1);
    };
    consume_everything(select_box);
    
    static Bounds initial_box;
    enum class SelectionDragType {
        NONE,
        MOVE,
        LEFT,
        RIGHT,
        TOP,
        BOTTOM,
        TOP_LEFT,
        TOP_RIGHT,
        BOTTOM_LEFT,
        BOTTOM_RIGHT,
    };
    static SelectionDragType selection_drag = SelectionDragType::NONE;
    auto cursor_from_selection_drag = [](SelectionDragType drag) -> const char * {
        switch (drag) {
            case SelectionDragType::LEFT:
                return "w-resize";
            case SelectionDragType::RIGHT:
                return "e-resize";
            case SelectionDragType::TOP:
                return "n-resize";
            case SelectionDragType::BOTTOM:
                return "s-resize";
            case SelectionDragType::TOP_LEFT:
                return "nw-resize";
            case SelectionDragType::TOP_RIGHT:
                return "ne-resize";
            case SelectionDragType::BOTTOM_LEFT:
                return "sw-resize";
            case SelectionDragType::BOTTOM_RIGHT:
                return "se-resize";
            default:
                return "crosshair";
        }
    };
    auto selection_drag_type_for_point = [](float mouse_x, float mouse_y) -> SelectionDragType {
        auto fixed = fixed_box(rect_selection.x, rect_selection.y, rect_selection.w, rect_selection.h);
        const float corner_hit = 14.0f;
        const float edge_hit = 12.0f;
        const float right = fixed.x + fixed.w;
        const float bottom = fixed.y + fixed.h;

        const bool near_left = std::abs(mouse_x - fixed.x) <= edge_hit;
        const bool near_right = std::abs(mouse_x - right) <= edge_hit;
        const bool near_top = std::abs(mouse_y - fixed.y) <= edge_hit;
        const bool near_bottom = std::abs(mouse_y - bottom) <= edge_hit;
        const bool inside_x = mouse_x >= fixed.x && mouse_x <= right;
        const bool inside_y = mouse_y >= fixed.y && mouse_y <= bottom;

        const bool near_top_left = std::abs(mouse_x - fixed.x) <= corner_hit && std::abs(mouse_y - fixed.y) <= corner_hit;
        const bool near_top_right = std::abs(mouse_x - right) <= corner_hit && std::abs(mouse_y - fixed.y) <= corner_hit;
        const bool near_bottom_left = std::abs(mouse_x - fixed.x) <= corner_hit && std::abs(mouse_y - bottom) <= corner_hit;
        const bool near_bottom_right = std::abs(mouse_x - right) <= corner_hit && std::abs(mouse_y - bottom) <= corner_hit;

        if (near_top_left) {
            return SelectionDragType::TOP_LEFT;
        }
        if (near_top_right) {
            return SelectionDragType::TOP_RIGHT;
        }
        if (near_bottom_left) {
            return SelectionDragType::BOTTOM_LEFT;
        }
        if (near_bottom_right) {
            return SelectionDragType::BOTTOM_RIGHT;
        }
        if (inside_y && near_left) {
            return SelectionDragType::LEFT;
        }
        if (inside_y && near_right) {
            return SelectionDragType::RIGHT;
        }
        if (inside_x && near_top) {
            return SelectionDragType::TOP;
        }
        if (inside_x && near_bottom) {
            return SelectionDragType::BOTTOM;
        }
        if (inside_x && inside_y) {
            return SelectionDragType::MOVE;
        }
        return SelectionDragType::NONE;
    };
    auto apply_select_drag = [](Container *actual_root) {
        float off_x = actual_root->mouse_current_x - actual_root->mouse_initial_x;
        float off_y = actual_root->mouse_current_y - actual_root->mouse_initial_y;
        auto copy = initial_box;
        const bool x_is_left = initial_box.x <= initial_box.w;
        const bool y_is_top = initial_box.y <= initial_box.h;

        auto set_left = [&](float value) {
            if (x_is_left) {
                copy.x = value;
            } else {
                copy.w = value;
            }
        };
        auto set_right = [&](float value) {
            if (x_is_left) {
                copy.w = value;
            } else {
                copy.x = value;
            }
        };
        auto set_top = [&](float value) {
            if (y_is_top) {
                copy.y = value;
            } else {
                copy.h = value;
            }
        };
        auto set_bottom = [&](float value) {
            if (y_is_top) {
                copy.h = value;
            } else {
                copy.y = value;
            }
        };

        if (selection_drag == SelectionDragType::MOVE) {
            copy.x += off_x;
            copy.y += off_y;
            copy.w += off_x;
            copy.h += off_y;
        } else if (selection_drag == SelectionDragType::LEFT) {
            set_left((x_is_left ? initial_box.x : initial_box.w) + off_x);
        } else if (selection_drag == SelectionDragType::RIGHT) {
            set_right((x_is_left ? initial_box.w : initial_box.x) + off_x);
        } else if (selection_drag == SelectionDragType::TOP) {
            set_top((y_is_top ? initial_box.y : initial_box.h) + off_y);
        } else if (selection_drag == SelectionDragType::BOTTOM) {
            set_bottom((y_is_top ? initial_box.h : initial_box.y) + off_y);
        } else if (selection_drag == SelectionDragType::TOP_LEFT) {
            set_left((x_is_left ? initial_box.x : initial_box.w) + off_x);
            set_top((y_is_top ? initial_box.y : initial_box.h) + off_y);
        } else if (selection_drag == SelectionDragType::TOP_RIGHT) {
            set_right((x_is_left ? initial_box.w : initial_box.x) + off_x);
            set_top((y_is_top ? initial_box.y : initial_box.h) + off_y);
        } else if (selection_drag == SelectionDragType::BOTTOM_LEFT) {
            set_left((x_is_left ? initial_box.x : initial_box.w) + off_x);
            set_bottom((y_is_top ? initial_box.h : initial_box.y) + off_y);
        } else if (selection_drag == SelectionDragType::BOTTOM_RIGHT) {
            set_right((x_is_left ? initial_box.w : initial_box.x) + off_x);
            set_bottom((y_is_top ? initial_box.h : initial_box.y) + off_y);
        }

        rect_selection = copy;
    };
    auto update_select_cursor = [selection_drag_type_for_point, cursor_from_selection_drag](Container *actual_root) {
        if (!rect_showing || mode != 1) {
            return;
        }
        auto drag = selection_drag_type_for_point(actual_root->mouse_current_x, actual_root->mouse_current_y);
        setCursorImageUntilUnset(cursor_from_selection_drag(drag));
    };

    select_box->when_mouse_enters_container = [update_select_cursor](Container *actual_root, Container *c) {
        consume_event(actual_root, c);
        update_select_cursor(actual_root);
    };
    select_box->when_mouse_motion = [update_select_cursor](Container *actual_root, Container *c) {
        consume_event(actual_root, c);
        update_select_cursor(actual_root);
    };
    select_box->when_mouse_leaves_container = [](Container *actual_root, Container *c) {
        consume_event(actual_root, c);
        if (mode >= 1 && mode != 4)
            setCursorImageUntilUnset("crosshair");
    };
    select_box->when_drag_start = [selection_drag_type_for_point, cursor_from_selection_drag](Container *actual_root, Container *c) {
        initial_box = rect_selection;
        selection_drag = selection_drag_type_for_point(actual_root->mouse_initial_x, actual_root->mouse_initial_y);
        if (selection_drag == SelectionDragType::NONE) {
            selection_drag = SelectionDragType::MOVE;
        }
        setCursorImageUntilUnset(cursor_from_selection_drag(selection_drag));
    };
    select_box->when_drag = [apply_select_drag](Container *actual_root, Container *c) {
        apply_select_drag(actual_root);
        consume_event(actual_root, c);
    };
    select_box->when_drag_end = [apply_select_drag, update_select_cursor](Container *actual_root, Container *c) {
        apply_select_drag(actual_root);
        selection_drag = SelectionDragType::NONE;
        update_select_cursor(actual_root);
        consume_event(actual_root, c);
    };
    
    

    {
        auto ch = label(type, "\uEC4E", "Full screen");
        ch->when_clicked = paint {
            auto rid = hypriso->monitor_from_cursor();
            hypriso->save_monitor_to_png(rid, "/tmp/out.png");
            notify("Saved to: /tmp/out.png");
            later_immediate([](Timer *) {
                screenshot_tool::close();
            });
        };
    }
    {
        auto ch = label(type, "\uF407", "Rectangle");
        ch->when_clicked = paint {
            mode = 1;
            setCursorImageUntilUnset("crosshair");
        };
    }
    {
        auto ch = label(type, "\uECE9", "Window");
        ch->when_clicked = paint {
            showing = false;
            hypriso->whitelist_on = false;
            mode = 2;
            setCursorImageUntilUnset("crosshair");
        }; 
    }
    {
        auto ch = label(type, "\uEF3C", "Colorpick");
        ch->when_clicked = paint {
            mode = 3;
            setCursorImageUntilUnset("crosshair");
        }; 
    }
    {
        auto ch = label(type, "\uE790", "Draw");
        ch->when_clicked = paint {
            showing = true;
            mode = 4;
            rect_showing = false;
            rect_selection = Bounds();
            draw_active = false;
            draw_erasing = false;
            draw_bubble_corner = 0;
            unsetCursorImage();
            set_cursor_hidden_for_screenshot_draw(true);
        };
    }
    
    //label(type, "\uF408", "Freeform");
    
    tool->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);

        c->real_bounds = bounds_monitor(rid);
        c->wanted_bounds = c->real_bounds;

        if (rid != hypriso->monitor_from_cursor())
            return;
        
        auto type = c->children[0];
        auto fixed_w = (type->wanted_pad.x + type->wanted_pad.w + (type->spacing * type->children.size() - 1));
        auto fixed_h = type->wanted_pad.y + type->wanted_pad.h;
        float calc_w = 0;
        float calc_h = 0;
        for (auto ch : type->children) {
            if (ch->pre_layout) {
                ch->pre_layout(actual_root, ch, b);
                calc_w += ch->wanted_bounds.w;
                if (calc_h > ch->wanted_bounds.h)
                    calc_h = ch->wanted_bounds.h;
            }
        }
        auto final_w = fixed_w + calc_w;
        auto final_h = fixed_h + calc_h;
        type->wanted_bounds = Bounds(
            c->real_bounds.x + c->real_bounds.w * .5 - final_w * .5, 
            c->real_bounds.y + c->real_bounds.h * .86 - final_h * .5, 
            final_w, final_h);
        type->real_bounds = type->wanted_bounds;
        layout(actual_root, type, type->real_bounds);
        type->exists = showing && mode != 1 && mode != 3 && mode != 4;

        auto select_box = c->children[1];
        auto fixed = fixed_box(rect_selection.x, rect_selection.y, rect_selection.w, rect_selection.h);
        select_box->wanted_bounds = fixed;
        select_box->real_bounds = fixed;
        select_box->exists = mode != 4;
        if (rect_showing) {
            layout(actual_root, select_box, select_box->real_bounds);
        }
    };

    tool->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);

        if (stage != (int) STAGE::RENDER_PRE_CURSOR)
            return;
        
        renderfix
        if (showing) {
            hypriso->draw_monitor(rid, c->real_bounds);
            /*
            if (mode == 3) {
                auto br = bounds_monitor(rid);
                auto mou = mouse();
                const double preferredPreviewSize = 80.0 * s;
                const double sampleSize           = 19.0;
                const double monitorX             = br.x * s;
                const double monitorY             = br.y * s;
                const double monitorW             = br.w * s;
                const double monitorH             = br.h * s;
                const double mouseX               = mou.x * s;
                const double mouseY               = mou.y * s;
                const double previewSize          = std::min({preferredPreviewSize, monitorW, monitorH});
                const double halfPreview          = previewSize * 0.5;

                // Convert global cursor position into monitor-local framebuffer pixels.
                const double mouseLocalX = (mou.x - br.x) * s;
                const double mouseLocalY = (mou.y - br.y) * s;
                const double fbW         = monitorW;
                const double fbH         = monitorH;
                const double halfSample  = sampleSize * 0.5;

                // Keep UV sample size fixed and clamp origin so it never shrinks at edges.
                const double uvX = std::clamp(mouseLocalX - halfSample, 0.0, std::max(0.0, fbW - sampleSize));
                const double uvY = std::clamp(mouseLocalY - halfSample, 0.0, std::max(0.0, fbH - sampleSize));

                // Center preview on cursor, but keep it fully inside the monitor.
                const double drawX = std::clamp(mouseX - halfPreview, monitorX, std::max(monitorX, monitorX + monitorW - previewSize));
                const double drawY = std::clamp(mouseY - halfPreview, monitorY, std::max(monitorY, monitorY + monitorH - previewSize));
                Bounds       drawBounds{drawX, drawY, previewSize, previewSize};
                drawBounds.round();

                hypriso->draw_monitor(rid, drawBounds, {uvX, uvY, sampleSize, sampleSize});

                // Border around the full zoom preview uses currently hovered pixel color.
                RGBA hovered = hypriso->colorpick_monitor(rid, mou);
                auto bb = drawBounds;
                bb.shrink(std::max(1.0, std::round(1.0 * s)));
                bb.round();
                border(bb, hovered, std::max(1.0, std::round(1.0 * s)));

                // Black border around the exact hovered source pixel inside the zoom preview.
                const double clampedMouseLocalX = std::clamp(mouseLocalX, 0.0, std::max(0.0, fbW - 1.0));
                const double clampedMouseLocalY = std::clamp(mouseLocalY, 0.0, std::max(0.0, fbH - 1.0));
                const double samplePixelSize    = previewSize / sampleSize;
                const double relX               = (clampedMouseLocalX - uvX) / sampleSize;
                const double relY               = (clampedMouseLocalY - uvY) / sampleSize;
                Bounds pixelBounds{
                    drawBounds.x + relX * previewSize - samplePixelSize * 0.5,
                    drawBounds.y + relY * previewSize - samplePixelSize * 0.5,
                    samplePixelSize,
                    samplePixelSize
                };
                pixelBounds.round();
                border(pixelBounds, {0, 0, 0, 1}, 1);
            }
            */
            if (mode != 1 && mode != 3 && mode != 4)
                rect(c->real_bounds, {0, 0, 0, .1});

            if (mode == 4) {
                auto mb = bounds_monitor(rid);

                auto to_render = [&](float x, float y) {
                    return Bounds{(x - mb.x) * s, (y - mb.y) * s, 0, 0};
                };
                auto diameter = std::max(1.0f, draw_brush_size * s);
                RGBA stamp_color = draw_palette[draw_color_index];
                auto stamp = [&](float x, float y) {
                    auto p = to_render(x, y);
                    Bounds b{p.x - diameter * .5f, p.y - diameter * .5f, diameter, diameter};
                    b.round();
                    rect(b, stamp_color, 0, b.w * .5f);
                };

                for (const auto &stroke : draw_strokes) {
                    if (stroke.empty())
                        continue;
                    stamp_color = stroke.color;
                    stamp(stroke[0].x, stroke[0].y);
                    for (size_t i = 1; i < stroke.size(); i++) {
                        const auto &a = stroke[i - 1];
                        const auto &b = stroke[i];
                        const float dx = b.x - a.x;
                        const float dy = b.y - a.y;
                        const float dist = std::sqrt(dx * dx + dy * dy);
                        const int steps = std::max(1, (int) std::ceil(dist / std::max(1.0f, draw_brush_size * .25f)));
                        for (int j = 1; j <= steps; j++) {
                            const float t = (float) j / (float) steps;
                            stamp(a.x + dx * t, a.y + dy * t);
                        }
                    }
                }

                const bool cursor_here = rid == hypriso->monitor_from_cursor();

                if (cursor_here) {
                    const float margin = 18.0f * s;
                    auto text = gen_text_texture(set->font, "Drawing", 14.0f * s, {.96, .97, 1, .96});
                    Bounds bubble{0, 0, (float) text.w + 26.0f * s, std::max((float) text.h + 14.0f * s, 32.0f * s)};
                    bubble.round();

                    // Six color boxes in a pill beside the bubble, on the side facing the screen's center.
                    const float box = std::round(bubble.h - 12.0f * s);
                    const float box_gap = std::round(6.0f * s);
                    const float palette_pad = std::round((bubble.h - box) * .5f);
                    const float palette_gap = std::round(8.0f * s);
                    Bounds palette{0, 0, palette_pad * 2 + box * DRAW_PALETTE_SIZE + box_gap * (DRAW_PALETTE_SIZE - 1), bubble.h};

                    // A "Copy (Ctrl+C)" button past the color boxes, on the far side from the bubble.
                    auto copy_text = gen_text_texture(set->font, "Copy (Ctrl+C)", 14.0f * s, {.96, .97, 1, .96});
                    Bounds copy_btn{0, 0, std::round((float) copy_text.w + 26.0f * s), bubble.h};

                    auto place_bubble = [&](int corner) {
                        const bool right_side = corner == 0 || corner == 1;
                        const float y = std::round((corner == 1 || corner == 2) ? (mb.h * s - bubble.h - margin) : margin);
                        if (right_side) {
                            bubble.x = std::round(mb.w * s - bubble.w - margin);
                            palette.x = bubble.x - palette_gap - palette.w;
                            copy_btn.x = palette.x - palette_gap - copy_btn.w;
                        } else {
                            bubble.x = std::round(margin);
                            palette.x = bubble.x + bubble.w + palette_gap;
                            copy_btn.x = palette.x + palette.w + palette_gap;
                        }
                        bubble.y = y;
                        palette.y = y;
                        copy_btn.y = y;
                    };
                    place_bubble(draw_bubble_corner);

                    // Only dodge the mouse once it is right on the bubble, so the color boxes beside it stay reachable.
                    auto mou = mouse();
                    const double mx = (mou.x - mb.x) * s;
                    const double my = (mou.y - mb.y) * s;
                    if (mx >= bubble.x && mx <= bubble.x + bubble.w && my >= bubble.y && my <= bubble.y + bubble.h) {
                        draw_bubble_corner = (draw_bubble_corner + 1) % 4;
                        place_bubble(draw_bubble_corner);
                    }

                    rect(bubble, {.08, .08, .1, .84}, 0, bubble.h * .5f, 2, true);
                    border(bubble, {1, 1, 1, .16}, std::max(1.0f, s), 0, bubble.h * .5f, 2);
                    if (text.id != -1) {
                        draw_texture(text, {bubble.x + (bubble.w - text.w) * .5, bubble.y + (bubble.h - text.h) * .5,
                                            (double) text.w, (double) text.h});
                        free_text_texture(text.id);
                    }

                    const bool copy_hovered = mx >= copy_btn.x && mx <= copy_btn.x + copy_btn.w && my >= copy_btn.y && my <= copy_btn.y + copy_btn.h;
                    draw_copy_button = {mb.x + copy_btn.x / s, mb.y + copy_btn.y / s, copy_btn.w / s, copy_btn.h / s};
                    RGBA copy_bg = copy_hovered ? RGBA(.2, .2, .24, .92) : RGBA(.08, .08, .1, .84);
                    if (copy_hovered && draw_pressing_copy)
                        copy_bg = RGBA(.28, .28, .33, .95);
                    rect(copy_btn, copy_bg, 0, copy_btn.h * .5f, 2, true);
                    border(copy_btn, {1, 1, 1, copy_hovered ? .32f : .16f}, std::max(1.0f, s), 0, copy_btn.h * .5f, 2);
                    if (copy_text.id != -1) {
                        draw_texture(copy_text, {copy_btn.x + (copy_btn.w - copy_text.w) * .5, copy_btn.y + (copy_btn.h - copy_text.h) * .5,
                                                 (double) copy_text.w, (double) copy_text.h});
                        free_text_texture(copy_text.id);
                    }

                    rect(palette, {.08, .08, .1, .84}, 0, palette.h * .5f, 2, true);
                    border(palette, {1, 1, 1, .16}, std::max(1.0f, s), 0, palette.h * .5f, 2);
                    draw_color_boxes.clear();
                    for (int i = 0; i < DRAW_PALETTE_SIZE; i++) {
                        Bounds b{palette.x + palette_pad + i * (box + box_gap), palette.y + palette_pad, box, box};
                        b.round();
                        // Remember where the box is in global logical coordinates for clicks.
                        draw_color_boxes.push_back({mb.x + b.x / s, mb.y + b.y / s, b.w / s, b.h / s});
                        const bool hovered = mx >= b.x && mx <= b.x + b.w && my >= b.y && my <= b.y + b.h;
                        Bounds swatch = b;
                        if (!hovered && i != draw_color_index)
                            swatch.shrink(std::round(2.0f * s));
                        rect(swatch, draw_palette[i], 0, 4.0f * s);
                        if (i == draw_color_index) {
                            Bounds ring = b;
                            ring.grow(std::round(3.0f * s));
                            border(ring, {1, 1, 1, .95}, std::max(1.0f, std::round(2.0f * s)), 0, 6.0f * s);
                        }
                    }
                }

                // The brush cursor goes on top. A soft dark shadow and outline keep it visible on white backgrounds.
                if (cursor_here) {
                    auto p = to_render(actual_root->mouse_current_x, actual_root->mouse_current_y);
                    const float cursor_diameter = std::max(1.0f, draw_active_brush_size() * s);
                    Bounds preview{p.x - cursor_diameter * .5f, p.y - cursor_diameter * .5f, cursor_diameter, cursor_diameter};
                    preview.round();
                    shadow(preview, {0, 0, 0, .45}, preview.w * .5f, 2.0f, std::max(4.0f, 6.0f * s));
                    RGBA fill = draw_erasing ? RGBA(1, 1, 1, .18) : draw_palette[draw_color_index];
                    fill.a = .18;
                    rect(preview, fill, 0, preview.w * .5f);
                    Bounds outer = preview;
                    outer.grow(std::max(1.0f, s));
                    border(outer, {0, 0, 0, .6}, std::max(1.0f, s), 0, outer.w * .5f);
                    border(preview, {1, 1, 1, .95}, std::max(1.0f, s), 0, preview.w * .5f);
                }
            }
        }

        hypriso->damage_entire(rid);
        
        if (rid != hypriso->monitor_from_cursor())
            return;

        if (showing)
            c->automatically_paint_children = true;
    };
    tool->after_paint = [](Container *actual_root, Container *c) {
        c->automatically_paint_children = false;
    };
    tool->when_drag_end_is_click = false;
    tool->when_mouse_down = [](Container *actual_root, Container *c) {
        consume_event(actual_root, c);
        if (mode != 4)
            return;

        draw_picking_color = false;
        draw_pressing_copy = false;
        if (draw_copy_button_at(actual_root->mouse_initial_x, actual_root->mouse_initial_y)) {
            // Copies on release (see when_clicked), so the press never starts a stroke.
            draw_picking_color = true;
            draw_pressing_copy = c->state.mouse_button_pressed == BTN_LEFT;
            damage_all();
            return;
        }
        int box = draw_color_box_at(actual_root->mouse_initial_x, actual_root->mouse_initial_y);
        if (box != -1) {
            draw_picking_color = true;
            if (c->state.mouse_button_pressed == BTN_LEFT)
                draw_color_index = box;
            damage_all();
            return;
        }
        if (c->state.mouse_button_pressed == BTN_LEFT) {
            draw_start_stroke((float) actual_root->mouse_initial_x, (float) actual_root->mouse_initial_y);
        } else if (c->state.mouse_button_pressed == BTN_RIGHT) {
            draw_begin_action();
            draw_active = false;
            draw_erasing = true;
            draw_erase_at((float) actual_root->mouse_initial_x, (float) actual_root->mouse_initial_y);
        }
        damage_all();
    };
    tool->when_fine_scrolled = [](Container *actual_root, Container *c, double scroll_x, double scroll_y, bool came_from_touchpad) {
        consume_event(actual_root, c);
        if (mode != 4)
            return;
        if (heart::shift_pressed())
            return; // Shift + scroll zooms instead of resizing the brush.
        draw_brush_size = std::clamp(draw_brush_size + (float) scroll_y * .1f, DRAW_BRUSH_MIN, DRAW_BRUSH_MAX);
        damage_all();
    };
    tool->when_clicked = paint {
        if (mode == 4) {
            consume_event(actual_root, c);
            draw_active = false;
            draw_erasing = false;
            draw_picking_color = false;
            const bool copy = draw_pressing_copy && draw_copy_button_at(actual_root->mouse_current_x, actual_root->mouse_current_y);
            draw_pressing_copy = false;
            if (copy) {
                copy_screenshot_to_clipboard(false, Bounds());
                later_immediate([](Timer *) {
                    screenshot_tool::close();
                });
            }
            return;
        }
        if (mode == 2) {
            auto cid = hypriso->window_from_mouse();
            hypriso->bring_to_front(cid);
            later_immediate([cid](Timer *) {
                screenshot_tool::close();
                damage_all();
                
                later_immediate([cid](Timer *) {
                    hypriso->screenshot_deco(cid);
                    later_immediate([cid](Timer *) {
                        hypriso->save_window_to_png(cid, true, "/tmp/out.png");
                        notify("Saved to: /tmp/out.png");
                    });
                });
            });
        }
        if (mode == 3) {
            auto rid = hypriso->monitor_from_cursor();
            RGBA color = hypriso->colorpick_monitor(rid, mouse());
            auto out = fz("{:.2f}, {:.2f}, {:.2f}, {:.2f}", color.r, color.g, color.b, color.a);
            system(fz("wl-copy \"{}\" &", out).c_str());
            //notify();
        }

        later_immediate([](Timer *) {
            screenshot_tool::close();
        });
    };
    tool->when_drag_start = [](Container *actual_root, Container *c) {
        if (mode == 4) {
            consume_event(actual_root, c);
            if (draw_picking_color)
                return;
            if (c->state.mouse_button_pressed == BTN_RIGHT) {
                draw_erasing = true;
                draw_active = false;
                draw_erase_at((float) actual_root->mouse_initial_x, (float) actual_root->mouse_initial_y);
                damage_all();
                return;
            }
            if (!draw_active && c->state.mouse_button_pressed == BTN_LEFT)
                draw_start_stroke((float) actual_root->mouse_initial_x, (float) actual_root->mouse_initial_y);
            return;
        }
        if (mode == 0) {
            mode = 1;
            setCursorImageUntilUnset("crosshair");
        }
        if (mode == 1) {
            rect_showing = true;
            rect_selection = Bounds(actual_root->mouse_current_x, actual_root->mouse_current_y, 
                                    actual_root->mouse_current_x, actual_root->mouse_current_y);
        }
    };
    tool->when_drag = [](Container *actual_root, Container *c) {
        if (mode == 4) {
            consume_event(actual_root, c);
            if (draw_erasing)
                draw_erase_at((float) actual_root->mouse_current_x, (float) actual_root->mouse_current_y);
            else if (draw_active && !draw_strokes.empty())
                draw_strokes.back().push_back({(float) actual_root->mouse_current_x, (float) actual_root->mouse_current_y});
            damage_all();
            return;
        }
        if (mode == 1) {
            rect_selection = Bounds(rect_selection.x, rect_selection.y, 
                                    actual_root->mouse_current_x, actual_root->mouse_current_y);
            
        }
    };
    tool->when_drag_end = [](Container *actual_root, Container *c) {
        if (mode == 4) {
            consume_event(actual_root, c);
            if (draw_erasing)
                draw_erase_at((float) actual_root->mouse_current_x, (float) actual_root->mouse_current_y);
            else if (draw_active && !draw_strokes.empty())
                draw_strokes.back().push_back({(float) actual_root->mouse_current_x, (float) actual_root->mouse_current_y});
            draw_active = false;
            draw_erasing = false;
            draw_picking_color = false;
            draw_pressing_copy = false;
            damage_all();
            return;
        }
        if (mode == 1) {
            rect_selection = Bounds(rect_selection.x, rect_selection.y, 
                                    actual_root->mouse_current_x, actual_root->mouse_current_y);
        }
    };
    
    tool->when_key_event = [](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        actual_root->consumed_event = true;
        
        if (key == KEY_ESC && !pressed) {
            if (mode == 4 && !draw_strokes.empty()) {
                draw_begin_action();
                draw_strokes.clear();
                draw_active = false;
                draw_erasing = false;
                damage_all();
            } else {
                later_immediate([](Timer *) {
                    screenshot_tool::close();
                });
            }
        }
        if (pressed && key == KEY_C && heart::ctrl_pressed()) {
            copy_screenshot_to_clipboard(mode == 1 && rect_showing, rect_selection);
            later_immediate([](Timer *) {
                screenshot_tool::close();
            });
            return;
        }
        if (mode == 4 && pressed && heart::ctrl_pressed()) {
            if (key == KEY_Z) {
                if (heart::shift_pressed())
                    draw_redo_action();
                else
                    draw_undo_action();
                damage_all();
            } else if (key == KEY_Y) {
                draw_redo_action();
                damage_all();
            }
        }
        if (key == KEY_ENTER) {
            if (mode == 1 && !pressed && rect_showing) {
                auto rid = hypriso->monitor_from_cursor();
                auto fixed = fixed_box(rect_selection.x, rect_selection.y, rect_selection.w, rect_selection.h);
                fixed.scale(scale(rid));
                hypriso->save_monitor_to_png(rid, "/tmp/out.png", fixed);
                notify("Saved to: /tmp/out.png");
                later_immediate([](Timer *) {
                    screenshot_tool::close();
                });
            }
            if (mode == 0 && !pressed) {
                auto rid = hypriso->monitor_from_cursor();
                hypriso->save_monitor_to_png(rid, "/tmp/out.png");
                notify("Saved to: /tmp/out.png");
                later_immediate([](Timer *) {
                    screenshot_tool::close();
                });
            }
            if (mode == 3 && !pressed) {
                later_immediate([](Timer *) {
                    screenshot_tool::close();
                });
            }
        }
    };
}

void screenshot_tool::open(bool draw) {
    showing = true;
    mode = draw ? 4 : 0;
    
    screenshot_tool::close();
    heart::set_force_meta_open(true);
    later(300, [draw](Timer *) {
        for (auto m : actual_monitors) {
            int monitor = *datum<int>(m, "cid");
            hypriso->screenshot_monitor(monitor);
        }
        hypriso->whitelist_on = true;
        actual_open_screenshot_tool();
        // A fullscreen window may have hidden the cursor. Modes that set
        // their own cursor take precedence over this left_ptr fallback.
        set_input_takeover("screenshot", true);
        if (draw) {
            unsetCursorImage();
            set_cursor_hidden_for_screenshot_draw(true);
        }
        damage_all();
    });
}

void screenshot_tool::close() {
    if (!overview::is_showing())
        hypriso->whitelist_on = false;
    set_cursor_hidden_for_screenshot_draw(false);
    unsetCursorImage();
    
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::SCREENSHOT_TOOL) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    heart::set_force_meta_open(false);
    heart::set_zoom(1.0);
    set_input_takeover("screenshot", false);
}

bool screenshot_tool::is_drawing() {
    if (mode != 4)
        return false;

    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::SCREENSHOT_TOOL)
            return true;
    }
    return false;
}
