#include "overview.h"

#include "heart.h"
#include "hypriso.h"
#include "layout_thumbnails.h"
#include <climits>
#include <math.h>

struct OverviewData : UserData {
    std::vector<int> order;
};

struct ThumbData : UserData {
    int cid;
};

static bool running = false;
static float overview_anim_time = 285.0f;

// {"anchors":[{"x":0,"y":1},{"x":0.4,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.25099658672626207,"y":0.7409722222222223},{"x":0.6439499918619792,"y":0.007916683620876747}]}
static std::vector<float> slidetopos2 = { 0, 0.017000000000000015, 0.03500000000000003, 0.05400000000000005, 0.07199999999999995, 0.09199999999999997, 0.11099999999999999, 0.132, 0.15200000000000002, 0.17400000000000004, 0.19599999999999995, 0.21899999999999997, 0.242, 0.266, 0.29100000000000004, 0.31699999999999995, 0.344, 0.372, 0.4, 0.43000000000000005, 0.46099999999999997, 0.494, 0.527, 0.563, 0.6, 0.626, 0.651, 0.675, 0.6970000000000001, 0.719, 0.739, 0.758, 0.777, 0.794, 0.8109999999999999, 0.8260000000000001, 0.841, 0.855, 0.868, 0.881, 0.892, 0.903, 0.914, 0.923, 0.9319999999999999, 0.9410000000000001, 0.948, 0.955, 0.962, 0.968, 0.973, 0.978, 0.983, 0.986, 0.99, 0.993, 0.995, 0.997, 0.998, 0.999, 1 };

void screenshot_loop(std::vector<int> order) {
    running = true;
    later(1000.0f / 24.0f, [](Timer *t) {
        t->keep_running = running;
        hypriso->screenshot_all();
    });
    later(1000.0f / 60.0f, [order](Timer *t) {
        t->keep_running = running;
        for (int i = 0; i < 3; i++) {
            if (i < order.size()) {
                hypriso->screenshot(order[i]);
            }
        }
    });
}

static bool screenshotting_wallpaper = false;

void fadeout_docks(Container *actual_root, Container *c, int monitor, long creation_time) {
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_POST_WALLPAPER || monitor != rid)
        return;
    renderfix
    
    auto scalar = ((float) (get_current_time_in_ms() - creation_time)) / overview_anim_time; 
    if (scalar > 1.0)
        scalar = 1.0;
    scalar = pull(slidetopos2, scalar);
    
    auto m = bounds_monitor(monitor);
    m.scale(s);
    auto rawmon = m;
    //rect(rawmon, {.14, .14, .14, 1 * scalar}, 0, 0, 2.0, false);
    hypriso->draw_wallpaper(monitor, m);
    rect(m, {0, 0, 0, .5}, 0, 0, 2.0, true);
}

void paint_over_wallpaper(Container *actual_root, Container *c, int monitor, long creation_time) {
    if (screenshotting_wallpaper)
        return;
    fadeout_docks(actual_root, c, monitor, creation_time);
    
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_POST_WINDOWS || monitor != rid)
        return;
    renderfix
    auto m = bounds_monitor(monitor);
    m.scale(s);
    auto rawmon = m;

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
    render_drop_shadow(rid, 1, {.1, .1, .1, .33}, 14 * s * scalar, 2.0, b, 50 * s);
    b.shrink(2);
    border(b, {1, 1, 1, .05}, 1, 0, 14 * s); 
}

static void paint_option(Container *actual_root, Container *c, int monitor) {
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_POST_WINDOWS || monitor != rid)
        return;
    renderfix
    auto cid = *datum<int>(c, "cid");
    hypriso->draw_thumbnail(cid, c->real_bounds);
    
    //rect(c->real_bounds, {1, 0, 0, 1});
}

static void create_option(int cid, Container *parent, int monitor) {
    auto c = parent->child(::absolute, FILL_SPACE, FILL_SPACE);
    *datum<int>(c, "cid") = cid;
    c->when_paint = [monitor](Container *actual_root, Container *c) {
        paint_option(actual_root, c, monitor);
    };
    c->receive_events_even_if_obstructed_by_one = true;
    c->when_drag_end_is_click = false;
    c->when_mouse_enters_container = paint {
    };
    c->when_clicked = paint {
        //notify("here");
        auto cid = *datum<int>(c, "cid");
        hypriso->bring_to_front(cid, true);
        later_immediate([](Timer *) {
            overview::close();
        });
    };
    c->when_drag_start = paint {
    };
    c->when_drag = paint {
    };
    c->when_drag_end = paint {
    };
}

static void layout_options(Container *actual_root, Container *c, const Bounds &b, long creation_time, float overx, float overy) {
    for (auto ch : c->children) {
        auto cid = *datum<int>(ch, "cid");
        auto final_bounds = *datum<Bounds>(ch, "final_bounds");
        final_bounds.x += overx * .5;
        final_bounds.y += overy * .5;
        auto bounds = real_bounds_client(cid);
        auto scalar = ((float) (get_current_time_in_ms() - creation_time)) / overview_anim_time; 
        if (scalar > 1.0)
            scalar = 1.0;
        scalar = pull(slidetopos2, scalar);
        auto lerped = lerp(bounds, final_bounds, scalar); 

        ch->wanted_bounds = lerped;
        ch->real_bounds = ch->wanted_bounds;
        ::layout(actual_root, ch, ch->real_bounds);
    }
}

void actual_open(int monitor) {
    hypriso->whitelist_on = true;
    
    auto over = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    auto overview_data = new OverviewData;
    auto order = get_window_stacking_order();
    for (auto o : order) {
        if (hypriso->alt_tabbable(o) && get_monitor(o) == monitor && hypriso->get_active_workspace_id_client(o) == hypriso->get_active_workspace_id(monitor)) {
            overview_data->order.push_back(o);
            hypriso->set_hidden(o, true);
        }
    }
    over->user_data = overview_data;
    over->custom_type = (int) TYPE::OVERVIEW;
    //consume_everything(over);
    over->when_mouse_down = nullptr;
    over->when_clicked = nullptr;
    over->when_mouse_up = nullptr;
    screenshot_loop(overview_data->order);
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
        
        return;

        auto data = (OverviewData *) c->user_data;
        
        auto scalar = ((float) (get_current_time_in_ms() - creation_time)) / overview_anim_time; 
        if (scalar > 1.0)
            scalar = 1.0;
        scalar = pull(slidetopos2, scalar);

        auto reserved = bounds_reserved_monitor(rid);
        reserved.y += 100;
        reserved.h -= 100;

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
            //if (hypriso->is_hidden(o))
                //alpha = scalar;
            render_drop_shadow(rid, 1, {0, 0, 0, .2}, 6 * s, 2.0, lerp(start, b, scalar), 4 * s);
            hypriso->draw_thumbnail(data->order[i], lerp(start, b, scalar), 6 * s, 2.0f, 0, alpha);
        }

        hypriso->damage_entire(monitor);
    };
    over->pre_layout = [monitor, creation_time](Container *actual_root, Container *c, const Bounds &b) {
        c->real_bounds = bounds_reserved_monitor(monitor);

        auto order = get_window_stacking_order();
        std::reverse(order.begin(), order.end());
        for (int i = c->children.size() - 1; i >= 0; i--) {
            auto child = c->children[i];
            auto cid = *datum<int>(child, "cid");
            bool found = false;
            for (auto option : order) {
                if (option == cid)
                    found = true;
            }
            if (!found) {
                delete child;
                c->children.erase(c->children.begin() + i);
                request_damage(actual_root, c);
            }
        }

        auto workspace_monitor = hypriso->get_active_workspace_id(monitor);

        // add if doesn't exist yet
        for (int i = order.size() - 1; i >= 0; i--) {
            auto option = order[i];
            if (hypriso->alt_tabbable(option) && hypriso->get_active_workspace_id_client(option) == workspace_monitor) {
                bool found = false;
                for (auto child : c->children) {
                    if (option == *datum<int>(child, "cid")) {
                        found = true;
                    }
                }
                if (!found) {
                    create_option(option, c, monitor);
                    request_damage(actual_root, c);
                }
            }
        }

        auto reserved = bounds_reserved_monitor(monitor);

        ExpoLayout layout;
        auto s = scale(monitor);
        std::vector<ExpoCell *> cells;
        for (int i = 0; i < c->children.size(); i++) {
            auto o = *datum<int>(c->children[i], "cid");
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
        for (int i = 0; i < cells.size(); i++) {
            auto cell = cells[i];
            auto rect = ((DemoCell *) cell)->result();
            *datum<Bounds>(c->children[i], "final_bounds") = Bounds(rect.x, rect.y, rect.w, rect.h);
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

        layout_options(actual_root, c, b, creation_time, overx, overy);
    };
    hypriso->damage_entire(monitor);
    hypriso->all_lose_focus();
}

void overview::open(int monitor) {
    if (running)
        return;
    later_immediate([monitor](Timer *) {
        screenshotting_wallpaper = true;
        hypriso->screenshot_wallpaper(monitor);
        screenshotting_wallpaper = false;
        hypriso->screenshot_all(); 

        actual_open(monitor);
    });
}

void overview::close() {
    running = false;
    hypriso->whitelist_on = false;
    auto m = actual_root;
    bool removed = false;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            auto o_data = (OverviewData *) c->user_data;
            for (auto o : o_data->order) {
                later(20, [o](Timer *) {
                    hypriso->set_hidden(o, false);
                });
            }
            
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
    return;
    if (state == 0) {
        overview::close();
        damage_all();
    }
}

