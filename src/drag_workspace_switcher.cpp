#include "drag_workspace_switcher.h"

#include "heart.h"
#include "container.h"
#include "titlebar.h"
#include "hypriso.h"
#include <fcntl.h>

static bool switcher_showing = false;

void drag_switcher_actual_open() {
    auto monitor = hypriso->monitor_from_cursor();
    auto c = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    *datum<float>(c, "openess") = 0.0;
    c->custom_type = (int) TYPE::WORKSPACE_SWITCHER;
    c->pre_layout = [monitor](Container *actual_root, Container *c, const Bounds &bounds) {
        auto openess = *datum<float>(c, "openess");
        
        auto b = bounds_monitor(monitor);
        auto new_w = b.w * .45;
        auto new_h = 36;
        new_h += 135.0f * openess;
        b.x += b.w * .5 - new_w * .5;
        b.w = new_w;
        b.h = new_h;

        if (openess != 0.0)
            b.grow(30);

        c->wanted_bounds = b;
        c->real_bounds = c->wanted_bounds;
        if (openess != 0.0 && openess  != 1.0) {
            request_damage(actual_root, c);
        }
    };
    c->when_paint = [monitor](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (rid != monitor || stage != (int) STAGE::RENDER_LAST_MOMENT)
            return;
        auto openess = *datum<float>(c, "openess");
        auto backup = c->real_bounds;
        defer(c->real_bounds = backup);
        if (openess != 0.0) {
            c->real_bounds.shrink(30); 
            c->real_bounds.y += 15 * openess;
            c->real_bounds.h -= 15 * openess;
        }
        renderfix;

        RGBA col = {.18, .18, .18, .9};
        auto b = c->real_bounds;
        b.y -= 20 * s * (1.0 - openess);
        b.h += 20 * s * (1.0 - openess);
        render_drop_shadow(monitor, 1.0, {0, 0, 0, .27}, 8 * s, 2.0, b);
        if (openess == 0.0) {
            rect(b, col, 3, 8 * s * (1 - openess), 2.0, true); 
        } else {
            rect(b, col, 0, 8 * s * openess, 2.0, true); 
        }
        b.shrink(1.0); 
        border(b, {.3, .3, .3, 1}, 1.0f, 3, 8 * s, 2.0, true); 
        
        // uf427
        //auto icon = get_cached_texture(root, c, "drag_text_icon", "Segoe Fluent Icons", "\uE1F4", 
        auto icon = get_cached_texture(root, c, "drag_text_icon", "Segoe Fluent Icons", "\uf407", 
            {.8, .8, .8, 1}, 14);
        draw_texture(*icon, c->real_bounds.x + 14 * s, 
            c->real_bounds.y + c->real_bounds.h - icon->h * 1.75, 1.0);

        auto t = get_cached_texture(root, c, "drag_text", mylar_font, "Drag a window here to move it to another workspace.", 
            {.8, .8, .8, 1}, 14);
        draw_texture(*t, 
            c->real_bounds.x + 14 * s + 10 * s + icon->w, 
            c->real_bounds.y + c->real_bounds.h - t->h * 1.45, 
            1.0);


    };
    c->when_mouse_motion = paint {
        request_damage(root, c);
    };
    damage_all();
}

// TODO: technically we have to open one per monitor
void drag_workspace_switcher::open() {
    if (switcher_showing) 
        return;
    switcher_showing = true;

    later_immediate([](Timer *) {
        auto mon = hypriso->monitor_from_cursor();
        hypriso->screenshot_wallpaper(mon);
        drag_switcher_actual_open();
    });
}

void drag_workspace_switcher::close() {
    defer(switcher_showing = false);
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    damage_all();    
}

void drag_workspace_switcher::click(int id, int button, int state, float x, float y) {

}

void drag_workspace_switcher::on_mouse_move(int x, int y) {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            if (bounds_contains(c->real_bounds, x, y)) {
                if (!c->state.mouse_hovering) {
                    auto openess = datum<float>(c, "openess");
                    animate(openess, 1.0, 100.0, c->lifetime, nullptr, nullptr);
                }
                c->state.mouse_hovering = true;
            } else {
                if (c->state.mouse_hovering) {
                    auto openess = datum<float>(c, "openess");
                    animate(openess, 0.0, 100.0, c->lifetime, nullptr, nullptr);
                }
                c->state.mouse_hovering = false;
            }
            request_damage(actual_root, c);
        }
    }
}

