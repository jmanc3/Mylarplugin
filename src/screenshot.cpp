#include "screenshot.h"

#include "heart.h"
#include "overview.h"

void label(Container *parent, std::string icon, std::string text) {
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
}

void actual_open_screenshot_tool() {
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

    label(type, "\uEC4E", "Full screen");
    label(type, "\uF407", "Rectangle");
    label(type, "\uECE9", "Window");
    label(type, "\uF408", "Freeform");

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
    };
    tool->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);

        if (stage != (int) STAGE::RENDER_PRE_CURSOR)
            return;
        
        renderfix
        hypriso->draw_monitor(rid, c->real_bounds);
        rect(c->real_bounds, {0, 0, 0, .1});
        hypriso->damage_entire(rid);
        
        if (rid != hypriso->monitor_from_cursor())
            return;
 
        c->automatically_paint_children = true;
    };
    tool->after_paint = [](Container *actual_root, Container *c) {
        c->automatically_paint_children = false;
    };
}

void screenshot_tool::open() {
    screenshot_tool::close();
    heart::set_force_meta_open(true);
    later_immediate([](Timer *) {
        for (auto m : actual_monitors) {
            hypriso->screenshot_monitor(*datum<int>(m, "cid"));
        }
        hypriso->whitelist_on = true;
        actual_open_screenshot_tool();
        damage_all();
    });
}

void screenshot_tool::close() {
    if (!overview::is_showing())
        hypriso->whitelist_on = false;
    
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


