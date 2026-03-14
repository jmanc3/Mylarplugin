#include "screenshot.h"

#include "heart.h"

void actual_open_screenshot_tool() {
    auto tool = actual_root->child(FILL_SPACE, FILL_SPACE);
    tool->custom_type = (int) TYPE::SCREENSHOT_TOOL;
    
    tool->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);

        c->real_bounds = bounds_monitor(rid);
    };
    tool->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);

        if (stage != (int) STAGE::RENDER_PRE_CURSOR)
            return;
        hypriso->damage_entire(rid);
        renderfix
        
        rect(c->real_bounds, {0, 0, 0, .2});
    };
}

void screenshot_tool::open() {
    screenshot_tool::close();
    later_immediate([](Timer *) {
        actual_open_screenshot_tool();
        damage_all();
    });
}

void screenshot_tool::close() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::SCREENSHOT_TOOL) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
}


