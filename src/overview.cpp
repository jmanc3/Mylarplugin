#include "overview.h"

#include "heart.h"
#include "hypriso.h"
#include "layout_thumbnails.h"

struct OverviewData : UserData {
    std::vector<int> order;
};

static bool running = false;

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
    
    over->when_paint = [monitor](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS)
            return;
        renderfix

        auto data = (OverviewData *) c->user_data;
        std::vector<Item> items;
        for (auto o : data->order) {
            Item item;
            auto size = hypriso->thumbnail_size(o);
            item.height = size.h * s;
            item.width = size.w * s;
            items.push_back(item);
        }

        auto reserved = bounds_reserved_monitor(rid);
        LayoutParams params {
            .availableWidth = (int) reserved.w - 40,
            .availableHeight = (int) reserved.h - 40,
            .horizontalSpacing = (int) (12 * s),
            .verticalSpacing = (int) (12 * s),
            .margin = (int) (40 * s),
            .maxThumbWidth = (int) (350 * s * .85),
            .densityPresets = {
                { 4, (int) (200 * s * .85) },
                { 9, (int) (166 * s * .85)},
                { 16, (int) (133 * s * .85) },
                { INT_MAX, (int) (100 * s * .85) }
            }
        };

        auto result = layoutOverview(params, items);

        for (int i = 0; i < result.items.size(); i++) {
            hypriso->draw_thumbnail(data->order[i], result.items[i].scale(s));
        }

        hypriso->damage_entire(monitor);
    };
    over->pre_layout = [monitor](Container *actual_root, Container *c, const Bounds &b) {
        c->real_bounds = bounds_reserved_monitor(monitor);
    };
    hypriso->damage_entire(monitor);
}

void overview::close() {
    running = false;
    hypriso->whitelist_on = false;
    auto m = actual_root;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            delete c;
            m->children.erase(m->children.begin() + i);
        }
    }
    damage_all();
}

void overview::click(int id, int button, int state, float x, float y) {
    overview::close();
    damage_all();
}

