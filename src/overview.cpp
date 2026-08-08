#include "overview.h"
#include "container.h"
#include "drag_workspace_switcher.h"
#include "first.h"
#include "heart.h"
#include "hypriso.h"
#include "layout_thumbnails.h"

bool screenshotting_wallpaper = false;
bool running = false;
float openess = 0.0f;

static void paint_workspace(int monitor_id, int rendering_workspace_id, float openess) {
    auto mb = bounds_monitor(monitor_id);
    auto s = scale(monitor_id);
    
    mb.scale(s);

    rect(mb, RGBA(0, 0, 0, .2));

    std::vector<int> clients_on_workspace;
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::CLIENT) {
            auto cid = *datum<int>(c, "cid");
            auto client_workspace_id = hypriso->get_active_workspace_id_client(cid);
            if (client_workspace_id == rendering_workspace_id) {
                clients_on_workspace.push_back(cid);
            }
        }
    }
    
    auto reserved = bounds_reserved_monitor(monitor_id);
    
    ExpoLayout layout;
    std::vector<ExpoCell *> cells;
    for (auto o : clients_on_workspace) {
        auto size = hypriso->thumbnail_size_deco(o);
        auto height = size.h * (1/s) + titlebar_h;
        auto width = size.w * (1/s);
        auto x = bounds_client(o).x - reserved.x;
        auto y = bounds_client(o).y - reserved.y;
        auto cell = new DemoCell(o, x, y, width, height);
        cells.push_back(cell);
    }

    float pad = 120;

    layout.setCells(cells);
    layout.setAreaSize(reserved.w - reserved.x - pad, reserved.h - reserved.y - pad);
    layout.calculate();

    int minX = INT_MAX;
    int minY = INT_MAX;
    int maxW = 0;
    int maxH = 0;
    for (int i = 0; i < cells.size(); i++) {
        auto democell = ((DemoCell *) cells[i]);
        auto rect = democell->result();
        if (rect.x < minX)
            minX = rect.x;
        if (rect.y < minY)
            minY = rect.y;
        if (rect.x + rect.w > maxW)
            maxW = rect.x + rect.w;
        if (rect.y + rect.h > maxH)
            maxH = rect.y + rect.h;
    }
    auto overx = reserved.w - minX - maxW;
    auto overy = reserved.h - minY - maxH;

    for (int i = 0; i < cells.size(); i++) {
        auto democell = ((DemoCell *) cells[i]);
        int cid = democell->persistentKey();

        auto size = hypriso->thumbnail_size_deco(cid);
        auto b = bounds_client(cid);
        b.scale(s);
        b.x -= size.x * s;
        b.y -= size.y * s;
        b.w = size.w;
        b.h = size.h;
        auto r = democell->result();
        auto final_b = lerp(b, Bounds(r.x + overx * (1 / s), r.y + overy * (1 / s), r.w, r.h).scale(s), openess);
        hypriso->draw_deco_thumbnail(cid, final_b);
    }
}

void create_overview_for_monitor(int monitor) {
    auto over = actual_root->child(FILL_SPACE, FILL_SPACE);
    //openess = 1.0;
    animate(&openess, 1.0, 200, over->lifetime);
    over->custom_type = (int) TYPE::OVERVIEW;
    over->pre_layout = [monitor](Container *root, Container *c, const Bounds &b) {
        c->wanted_bounds = bounds_reserved_monitor(monitor);
        c->real_bounds = c->wanted_bounds;
        damage_all();
    };
    over->when_paint = [](Container *root, Container *c) {
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS)
            return;

        paint_workspace(rid, hypriso->get_active_workspace_id(rid), openess);
    };
    over->when_clicked = [](Container *root, Container *c) {
        later_immediate([](Timer *) {
            overview::close();
        });
    };
}

void overview::open(int monitor) {
    hypriso->whitelist_on = true;
    running = true;
    for (auto m : actual_monitors) {
        auto mid = *datum<int>(m, "cid");
        create_overview_for_monitor(mid);
    }
    later(1000.0f / hypriso->fps(monitor), [](Timer *t) {
        t->keep_running = false;
        for (int i = actual_root->children.size() - 1; i >= 0; i--) {
            auto c = actual_root->children[i];
            if (c->custom_type == (int) TYPE::CLIENT) {
                auto cid = *datum<int>(c, "cid");
                hypriso->screenshot_deco(cid);
            }
        }
 
        for (auto c : actual_root->children) 
            if (c->custom_type == (int) TYPE::OVERVIEW)
                t->keep_running = true;
        damage_all();
    });
    drag_workspace_switcher::open();
    drag_workspace_switcher::force_hold_open(true);
}

void overview_actual_close() {
    hypriso->whitelist_on = false;
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
    request_refresh();
}

void overview::close(bool focus) {
    auto m = actual_root;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            if (!is_being_animating(&openess) || !is_being_animating_to(&openess, 0.0))
                animate(&openess, 0.0, 200, c->lifetime, [](bool) {
                    overview_actual_close();
                });
        }
    }
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

