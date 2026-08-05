#include "overview.h"
#include "container.h"
#include "drag_workspace_switcher.h"
#include "first.h"
#include "heart.h"
#include "hypriso.h"

bool screenshotting_wallpaper = false;
bool running = false;
float openess = 0.0f;

void create_overview_for_monitor(int monitor) {
    auto over = actual_root->child(FILL_SPACE, FILL_SPACE);
    over->custom_type = (int) TYPE::OVERVIEW;
    over->pre_layout = [monitor](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds = bounds_reserved_monitor(monitor);
        c->real_bounds = c->wanted_bounds;
    };
    over->when_paint = [monitor](Container *root, Container *c) {
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS)
            return;
        renderfix

        rect(c->real_bounds, RGBA(1, 0, 1, 1));
        
        auto text = std::to_string(hypriso->get_active_workspace(monitor));
        auto tex = gen_text_texture(mylar_font, text, 160 * s, RGBA(0, 0, 0, 1));
        draw_texture(tex, 100, 100);
        free_text_texture(tex.id);
    };
}

void overview::open(int monitor) {
    openess = 1.0;
    running = true;
    for (auto m : actual_monitors) {
        auto mid = *datum<int>(m, "cid");
        create_overview_for_monitor(mid);
    }
    later(1000.0f / hypriso->fps(monitor), [](Timer *t) {
        t->keep_running = false;
        for (auto c : actual_root->children) 
            if (c->custom_type == (int) TYPE::OVERVIEW)
                t->keep_running = true;
        damage_all();
    });
    drag_workspace_switcher::open();
    drag_workspace_switcher::force_hold_open(true);
}

void overview::close(bool focus) {
    openess = 0.0;
    running = false;

    auto m = actual_root;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            delete c;
            m->children.erase(m->children.begin() + i);
        }
    }
    drag_workspace_switcher::close();
}

void overview::instant_close() {
    overview::close();
}

void overview::click(int id, int button, int state, float x, float y) {

}

void overview::overwrite_openess(float a) {

}

void overview::fake_paint(int id) {

}

void overview::should_draw(bool state) {

}
void overview::should_force_paint(bool state) {

}

bool overview::is_showing() {
    return running;
}

float overview::get_openess() {
    return openess;
}

