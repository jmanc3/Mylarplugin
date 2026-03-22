#include "screenshot.h"

#include "heart.h"
#include "overview.h"
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static bool showing = true;
static int mode = 0; // 1 rectangle, 2 window

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
    
    auto select_box = tool->child(::hbox, FILL_SPACE, FILL_SPACE);
    select_box->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        renderfix
        border(c->real_bounds, {1, 1, 1, 1}, 1);
    };
    consume_everything(select_box);
    
    static Bounds initial_box;
    select_box->when_drag_start = paint {
        initial_box = rect_selection;
    };
    select_box->when_drag = [](Container *actual_root, Container *c) {
        float off_x = actual_root->mouse_current_x - actual_root->mouse_initial_x;
        float off_y = actual_root->mouse_current_y - actual_root->mouse_initial_y;
        auto copy = initial_box;
        copy.x += off_x;
        copy.y += off_y;
        copy.w += off_x;
        copy.h += off_y;
        rect_selection = copy;
    };
    select_box->when_drag_end = select_box->when_drag;
    
    

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
            hypriso->whitelist_on = false;
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
    //label(type, "\uF408", "Freeform");
    
    static bool rect_showing = false;
    rect_showing = false;
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
        type->exists = showing && mode != 1;

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
        if (showing && mode != 1) {
            hypriso->draw_monitor(rid, c->real_bounds);
            rect(c->real_bounds, {0, 0, 0, .1});
        }
        if (rect_showing && false) {
            auto fixed = fixed_box(rect_selection.x, rect_selection.y, rect_selection.w, rect_selection.h);
            fixed.scale(s);
            fixed.round();
            border(fixed, {1, 1, 1, .8}, 1);
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
        if (!showing) {
            if (mode == 2) {
                auto cid = hypriso->window_from_mouse();
                hypriso->bring_to_front(cid);
                later_immediate([cid](Timer *) {
                    screenshot_tool::close();
                    damage_all();
                    
                    later_immediate([cid](Timer *) {
                        hypriso->screenshot_deco(cid);
                        hypriso->save_window_to_png(cid, true, "/tmp/out.png");
                        notify("Saved to: /tmp/out.png");
                    });
                });
            }
        }
    };
    tool->when_drag_start = [](Container *actual_root, Container *c) {
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


