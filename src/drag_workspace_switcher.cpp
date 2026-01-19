#include "drag_workspace_switcher.h"

#include "heart.h"
#include "container.h"
#include "titlebar.h"

// TODO: technically we have to open one per monitor
void drag_workspace_switcher::open() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            return;
        }
    }
    
    auto monitor = hypriso->monitor_from_cursor();
    auto c = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    c->custom_type = (int) TYPE::WORKSPACE_SWITCHER;
    c->pre_layout = [monitor](Container *actual_root, Container *c, const Bounds &bounds) {
        auto b = bounds_monitor(monitor);
        auto new_w = b.w * .45;
        auto new_h = 36;
        b.x += b.w * .5 - new_w * .5;
        b.w = new_w;
        b.h = new_h;
        c->wanted_bounds = b;
        c->real_bounds = c->wanted_bounds;
    };
    c->when_paint = [monitor](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (rid != monitor || stage != (int) STAGE::RENDER_POST_WINDOWS)
            return;
        renderfix;

        RGBA col = {.18, .18, .18, .9};
        rect(c->real_bounds, col, 3, 8 * s, 2.0, true); 
        auto b = c->real_bounds;
        b.y -= 20 * s;
        b.h += 20 * s;
        render_drop_shadow(monitor, 1.0, {0, 0, 0, .27}, 8 * s, 2.0, b);
        b.shrink(1.0); 
        border(b, {.3, .3, .3, 1}, 1.0f, 3, 8 * s, 2.0, true); 
        
        // uf427
        //auto icon = get_cached_texture(root, c, "drag_text_icon", "Segoe Fluent Icons", "\uE1F4", 
        auto icon = get_cached_texture(root, c, "drag_text_icon", "Segoe Fluent Icons", "\uf407", 
            {.8, .8, .8, 1}, 11 * s);
        draw_texture(*icon, c->real_bounds.x + 14 * s, center_y(c, icon->h), 1.0);

        auto t = get_cached_texture(root, c, "drag_text", mylar_font, "Drag a window here to move it to another workspace.", 
            {.8, .8, .8, 1}, 10 * s);
        draw_texture(*t, c->real_bounds.x + 14 * s + 10 * s + icon->w, center_y(c, t->h), 1.0);


    };
    damage_all();
}

void drag_workspace_switcher::close() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    damage_all();
}


