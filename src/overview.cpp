#include "overview.h"

#include "alt_tab.h"
#include "heart.h"

void overview::open(int monitor) {
    alt_tab::show();
    
    auto over = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    over->custom_type = (int) TYPE::OVERVIEW;
    over->when_paint = [monitor](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        renderfix
        
        rect(c->real_bounds, {0, 0, 0, .1}, 0, 0, 2.0, false);
        hypriso->damage_entire(monitor);
    };
    over->pre_layout = [monitor](Container *actual_root, Container *c, const Bounds &b) {
        c->real_bounds = bounds_reserved_monitor(monitor);
    };
}

void overview::close() {
    auto m = actual_root;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            delete c;
            m->children.erase(m->children.begin() + i);
        }
    }
}

void overview::click(int id, int button, int state, float x, float y) {
    overview::close();
    hypriso->damage_entire(hypriso->monitor_from_cursor());
}

