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

// {"anchors":[{"x":0,"y":1},{"x":0.47500000000000003,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.2911752162835537,"y":0.9622916751437718},{"x":0.6883506970527843,"y":0.08506946563720702}]}
static std::vector<float> slidetopos = { 0, 0.0030000000000000027, 0.006000000000000005, 0.01100000000000001, 0.016000000000000014, 0.02200000000000002, 0.030000000000000027, 0.038000000000000034, 0.04800000000000004, 0.05900000000000005, 0.07099999999999995, 0.08399999999999996, 0.09899999999999998, 0.11499999999999999, 0.132, 0.15100000000000002, 0.17200000000000004, 0.19399999999999995, 0.21799999999999997, 0.243, 0.271, 0.30000000000000004, 0.33199999999999996, 0.366, 0.402, 0.44099999999999995, 0.483, 0.527, 0.575, 0.612, 0.636, 0.6579999999999999, 0.6799999999999999, 0.7, 0.72, 0.738, 0.756, 0.773, 0.789, 0.8049999999999999, 0.8200000000000001, 0.834, 0.847, 0.86, 0.873, 0.884, 0.895, 0.906, 0.916, 0.925, 0.9339999999999999, 0.943, 0.951, 0.959, 0.966, 0.972, 0.979, 0.985, 0.99, 0.995, 1 };

void screenshot_loop() {
    running = true;
    later(1000.0f / 24.0f, [](Timer *t) {
        t->keep_running = running;
        hypriso->screenshot_all();
    });
}

void overview::open(int monitor) {
    hypriso->whitelist_on = true;
    
    auto over = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    auto overview_data = new OverviewData;
    auto order = get_window_stacking_order();
    for (auto o : order) {
        if (hypriso->alt_tabbable(o) && get_monitor(o) == monitor) {
            overview_data->order.push_back(o);
        }
    }
    over->user_data = overview_data;
    over->custom_type = (int) TYPE::OVERVIEW;
    consume_everything(over);
    later_immediate([](Timer *) { hypriso->screenshot_all(); });
    screenshot_loop();
    auto creation_time = get_current_time_in_ms();
    
    over->when_paint = [monitor, creation_time](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS)
            return;
        renderfix

        auto data = (OverviewData *) c->user_data;
        
        auto scalar = ((float) (get_current_time_in_ms() - creation_time)) / 200.0f; 
        if (scalar > 1.0)
            scalar = 1.0;
        scalar = pull(slidetopos, scalar);

        auto reserved = bounds_reserved_monitor(rid);

        ExpoLayout layout;
        std::vector<ExpoCell *> cells;
        for (int i = 0; i < data->order.size(); i++) {
            auto o = data->order[i];
            auto size = hypriso->thumbnail_size(o);
            auto height = size.h * (1/s);
            auto width = size.w * (1/s);
            auto x = bounds_client(o).x;
            auto y = bounds_client(o).y;
            auto cell = new DemoCell(i, x, y, width, height);
            cells.push_back(cell);
        }

        float totalpad = 20;
        
        layout.setCells(cells);
        layout.setAreaSize(reserved.w - totalpad * 2, reserved.h - totalpad * 2);
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
            if (rect.y + rect.w > maxH) 
                maxH = rect.x + rect.h;
        }

        //notify(fz("{} {} {}", reserved.w, minX, maxW));
        auto overx = reserved.w - minX - maxW; 

        for (int i = 0; i < data->order.size(); i++) {
            auto o = data->order[i];
            auto cell = cells[i];
            auto rect = ((DemoCell *) cell)->result();
            auto b = Bounds(rect.x, rect.y, rect.w, rect.h);
            b.x += totalpad * s;
            b.y += totalpad * s;
            auto start_bounds = bounds_client(o);
            Bounds start = {start_bounds.x * s, start_bounds.y * s, start_bounds.w, start_bounds.h};
            auto size = hypriso->thumbnail_size(o);
            start.w = size.w * s;
            start.h = size.h * s;
            
            b.scale(s);
            b.x += (overx * s) * .5;
            hypriso->draw_thumbnail(data->order[i], lerp(start, b, scalar));
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

