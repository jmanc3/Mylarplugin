#include "screenshot.h"

#include "heart.h"
#include "overview.h"
#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static bool showing = true;
static int mode = 0; // 1 rectangle, 2 window, 3 colorpick

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
        auto info = gen_text_texture("Segoe Fluent Icons", icon, label_h * s, {1, 1, 1, 1});
        free_text_texture(info.id); 
        auto info2 = gen_text_texture(mylar_font, text, label_h * s, {1, 1, 1, 1});
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
            auto info = gen_text_texture("Segoe Fluent Icons", icon, label_h * s, {1, 1, 1, 1});
            draw_texture(info, {c->real_bounds.x + pad * s + xoff, 
                               c->real_bounds.y + c->real_bounds.h * .5 - info.h * .5, 
                               (double) info.w, (double) info.h});
            xoff += info.w;
            free_text_texture(info.id); 
        }
        xoff += 8 * s;
        {
            auto info = gen_text_texture(mylar_font, text, label_h * s, {1, 1, 1, 1});
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
    type->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        renderfix
        rect(c->real_bounds, {1, 0, 0, 1});
    };
    type->spacing = 20;

    static Bounds rect_selection;
    rect_selection = Bounds();
    static bool rect_showing = false;
    rect_showing = false;
    
    auto select_box = tool->child(::hbox, FILL_SPACE, FILL_SPACE);
    select_box->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
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
        if (mode >= 1)
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
        auto ch = label(type, "\uECE9", "Colorpick");
        ch->when_clicked = paint {
            mode = 3;
            setCursorImageUntilUnset("crosshair");
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
        type->exists = showing && mode != 1 && mode != 3;

        auto select_box = c->children[1];
        auto fixed = fixed_box(rect_selection.x, rect_selection.y, rect_selection.w, rect_selection.h);
        select_box->wanted_bounds = fixed;
        select_box->real_bounds = fixed;
        select_box->exists = true;
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
            if (mode != 1 && mode != 3)
                rect(c->real_bounds, {0, 0, 0, .1});
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
    tool->when_clicked = paint {
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
            notify(fz("{} {} {} {}", color.r, color.g, color.b, color.a));
        }

        later_immediate([](Timer *) {
            screenshot_tool::close();
        });
    };
    tool->when_drag_start = [](Container *actual_root, Container *c) {
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
        if (mode == 1) {
            rect_selection = Bounds(rect_selection.x, rect_selection.y, 
                                    actual_root->mouse_current_x, actual_root->mouse_current_y);
            
        }
    };
    tool->when_drag_end = [](Container *actual_root, Container *c) {
        if (mode == 1) {
            rect_selection = Bounds(rect_selection.x, rect_selection.y, 
                                    actual_root->mouse_current_x, actual_root->mouse_current_y);
        }
    };
    
    tool->when_key_event = [](Container *root, Container* c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
        if (key == KEY_ESC && !pressed) {
            later_immediate([](Timer *) {
                screenshot_tool::close();
            });
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

void screenshot_tool::open() {
    showing = true;
    mode = 0;
    
    screenshot_tool::close();
    heart::set_force_meta_open(true);
    later_immediate([](Timer *) {
        for (auto m : actual_monitors) {
            int monitor = *datum<int>(m, "cid");
            hypriso->screenshot_monitor(monitor);
        }
        hypriso->whitelist_on = true;
        actual_open_screenshot_tool();
        damage_all();
    });
}

void screenshot_tool::close() {
    if (!overview::is_showing())
        hypriso->whitelist_on = false;
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
}
