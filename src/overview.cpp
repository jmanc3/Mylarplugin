#include "overview.h"

#include "heart.h"
#include "hypriso.h"
#include "layout_thumbnails.h"
#include <climits>
#include <cmath>

struct OverviewData : UserData {
    std::vector<int> order;
};

static bool running = false;
static float overview_anim_time = 370.0f;

// {"anchors":[{"x":0,"y":1},{"x":0.4,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.25099658672626207,"y":0.7409722222222223},{"x":0.6439499918619792,"y":0.007916683620876747}]}
static std::vector<float> slidetopos2 = { 0, 0.017000000000000015, 0.03500000000000003, 0.05400000000000005, 0.07199999999999995, 0.09199999999999997, 0.11099999999999999, 0.132, 0.15200000000000002, 0.17400000000000004, 0.19599999999999995, 0.21899999999999997, 0.242, 0.266, 0.29100000000000004, 0.31699999999999995, 0.344, 0.372, 0.4, 0.43000000000000005, 0.46099999999999997, 0.494, 0.527, 0.563, 0.6, 0.626, 0.651, 0.675, 0.6970000000000001, 0.719, 0.739, 0.758, 0.777, 0.794, 0.8109999999999999, 0.8260000000000001, 0.841, 0.855, 0.868, 0.881, 0.892, 0.903, 0.914, 0.923, 0.9319999999999999, 0.9410000000000001, 0.948, 0.955, 0.962, 0.968, 0.973, 0.978, 0.983, 0.986, 0.99, 0.993, 0.995, 0.997, 0.998, 0.999, 1 };

void screenshot_loop() {
    running = true;
    later(1000.0f / 24.0f, [](Timer *t) {
        t->keep_running = running;
        hypriso->screenshot_all();
    });
}

static bool screenshotting_wallpaper = false;

void paint_over_wallpaper(Container *actual_root, Container *c, int monitor, long creation_time) {
    if (screenshotting_wallpaper)
        return;
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_POST_WINDOWS || monitor != rid)
        return;
    renderfix
    auto m = bounds_monitor(monitor);
    m.scale(s);
    auto rawmon = m;
    rect(c->real_bounds, {.14, .14, .14, 1});

    auto scalar = ((float) (get_current_time_in_ms() - creation_time)) / overview_anim_time; 
    if (scalar > 1.0)
        scalar = 1.0;
    scalar = pull(slidetopos2, scalar);

    float padamount = .17;
    float padx = m.w * padamount;
    float pady = m.h * padamount;
    m.x += padx * .5;
    m.w -= padx;
    m.y += pady * .5;
    m.h -= pady;
    m = lerp(rawmon, m, scalar);
    hypriso->draw_wallpaper(monitor, m, 14 * s * scalar);
    auto b = m;
    render_drop_shadow(rid, 1, {0, 0, 0, .3}, 14 * s * scalar, 2.0, b);
    b.shrink(2);
    border(b, {1, 1, 1, .1}, 1, 0, 14 * s); 
}

void overview::open(int monitor) {
    hypriso->whitelist_on = true;
    
    auto over = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    auto overview_data = new OverviewData;
    auto order = get_window_stacking_order();
    for (auto o : order) {
        if (hypriso->alt_tabbable(o) && get_monitor(o) == monitor && hypriso->get_active_workspace_id_client(o) == hypriso->get_active_workspace_id(monitor)) {
            overview_data->order.push_back(o);
        }
    }
    over->user_data = overview_data;
    over->custom_type = (int) TYPE::OVERVIEW;
    consume_everything(over);
    later_immediate([monitor](Timer *) { 
        screenshotting_wallpaper = true;
        hypriso->screenshot_wallpaper(monitor);
        screenshotting_wallpaper = false;
        hypriso->screenshot_all(); 
    });
    screenshot_loop();
    auto creation_time = get_current_time_in_ms();
    
    over->when_paint = [monitor, creation_time](Container *actual_root, Container *c) {
        paint_over_wallpaper(actual_root, c, monitor, creation_time);
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS || monitor != rid)
            return;
        renderfix
        hypriso->damage_entire(monitor);
        
        //return;

        auto data = (OverviewData *) c->user_data;
        
        auto scalar = ((float) (get_current_time_in_ms() - creation_time)) / overview_anim_time; 
        if (scalar > 1.0)
            scalar = 1.0;
        scalar = pull(slidetopos2, scalar);

        auto reserved = bounds_reserved_monitor(rid);

        ExpoLayout layout;
        std::vector<ExpoCell *> cells;
        for (int i = 0; i < data->order.size(); i++) {
            auto o = data->order[i];
            auto size = hypriso->thumbnail_size(o);
            auto height = size.h * (1/s);
            auto width = size.w * (1/s);
            auto x = bounds_client(o).x - reserved.x;
            auto y = bounds_client(o).y - reserved.y;
            auto cell = new DemoCell(i, x, y, width, height);
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
        for (int i = 0; i < data->order.size(); i++) {
            auto cell = cells[i];
            auto rect = ((DemoCell *) cell)->result();
            if (rect.x < minX) 
                minX = rect.x;
            if (rect.y < minY) 
                minY = rect.y;                 
            if (rect.x + rect.w > maxW) 
                maxW = rect.x + rect.w;
            if (rect.y + rect.h > maxH) 
                maxH = rect.y + rect.h;
        }

        //notify(fz("{} {} {}", reserved.w, minX, maxW));
        auto overx = reserved.w - minX - maxW; 
        auto overy = reserved.h - minY - maxH; 

        for (int i = 0; i < data->order.size(); i++) {
            auto o = data->order[i];
            auto cell = cells[i];
            auto rect = ((DemoCell *) cell)->result();
            auto b = Bounds(rect.x, rect.y, rect.w, rect.h);
            b.x += reserved.x;
            b.y += reserved.y;
            auto start_bounds = bounds_client(o);
            Bounds start = {start_bounds.x * s, start_bounds.y * s, start_bounds.w, start_bounds.h};
            auto size = hypriso->thumbnail_size(o);
            start.w = size.w * s;
            start.h = size.h * s;
            
            b.scale(s);
            b.x += (overx * s) * .5;
            b.y += (overy * s) * .5;
            float alpha = 1.0;
            if (hypriso->is_hidden(o))
                alpha = scalar;
            render_drop_shadow(rid, 1, {0, 0, 0, .25}, 6 * s, 2.0, lerp(start, b, scalar));
            hypriso->draw_thumbnail(data->order[i], lerp(start, b, scalar), 6 * s, 2.0f, 0, alpha);
        }

        hypriso->damage_entire(monitor);
    };
    over->pre_layout = [monitor](Container *actual_root, Container *c, const Bounds &b) {
        c->real_bounds = bounds_reserved_monitor(monitor);
    };
    hypriso->damage_entire(monitor);
    hypriso->all_lose_focus();
}

void overview::close() {
    running = false;
    hypriso->whitelist_on = false;
    auto m = actual_root;
    bool removed = false;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            removed = true;
            delete c;
            m->children.erase(m->children.begin() + i);
        }
    }
    damage_all();
    if (removed)
        later_immediate([](Timer *) {
            hypriso->all_gain_focus();
        });
}

void overview::click(int id, int button, int state, float x, float y) {
    overview::close();
    damage_all();
}

