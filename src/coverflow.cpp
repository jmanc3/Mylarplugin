#include "coverflow.h"

#include "heart.h"

static void fill_coverflow(Container *c, int monitor) {
    c->custom_type = (int) TYPE::COVERFLOW;
    *datum<int>(c, "monitor") = monitor;

    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto monitor = *datum<int>(c, "monitor");
        c->wanted_bounds = bounds_monitor(monitor);
        c->real_bounds = c->wanted_bounds;
    };

    c->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        auto monitor = *datum<int>(c, "monitor");

        if (stage != (int) STAGE::RENDER_LAST_MOMENT && rid != monitor)
            return;
        renderfix

        hypriso->draw_wallpaper(rid, c->real_bounds);
        rect(c->real_bounds, {0, 0, 0, .6}); 
    };
}
                           
void coverflow::open() {
    later_immediate([](Timer *) {
        auto monitor = hypriso->monitor_from_cursor();
        hypriso->screenshot_wallpaper(monitor);
        auto cover = actual_root->child(FILL_SPACE, FILL_SPACE);
        fill_coverflow(cover, monitor);
    });
}

void coverflow::close(bool focus) {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::COVERFLOW) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    damage_all();
}

void coverflow::scroll(float x, float y) {
    
}

