#include "screenshot.h"

#include "heart.h"
#include "overview.h"

void actual_open_screenshot_tool() {
    auto tool = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    tool->custom_type = (int) TYPE::SCREENSHOT_TOOL;
    tool->automatically_paint_children = false;
    consume_everything(tool);

    auto type = tool->child(FILL_SPACE, FILL_SPACE);
    type->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        renderfix
        
        if (c->state.mouse_pressing) {
            rect(c->real_bounds, {0, 0, 1, 1});
        } if (c->state.mouse_hovering) {
            rect(c->real_bounds, {0, 1, 0, 1});
        } else {
            rect(c->real_bounds, {1, 0, 0, 1});
        }
    };
    
    tool->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);

        c->real_bounds = bounds_monitor(rid);

        auto type = c->children[0];
        type->real_bounds = c->real_bounds;
        type->real_bounds.w = 200;
        type->real_bounds.h = 50;
        type->real_bounds.x += c->real_bounds.w * .5 - type->real_bounds.w * .5;
        type->real_bounds.y += c->real_bounds.h * .9 - type->real_bounds.h * .5;
        type->wanted_bounds = type->real_bounds;
        type->wanted_bounds.x = 0;
        type->wanted_bounds.y = 0;
        layout(actual_root, type, type->real_bounds);
    };
    tool->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);

        if (stage != (int) STAGE::RENDER_PRE_CURSOR)
            return;
        c->automatically_paint_children = true;
        hypriso->damage_entire(rid);
        renderfix
        
        hypriso->draw_monitor(rid, c->real_bounds);
        rect(c->real_bounds, {0, 0, 0, .1});
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


