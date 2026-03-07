#include "coverflow.h"

#include "heart.h"
#include <algorithm>

#define LAST_TIME_ACTIVE "last_time_active"

static bool render_covers = true;
static float full = 1100;

static void paint_cover(Container *actual_root, Container *c) {
    auto root = get_rendering_root();
    if (!root) return;
    
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
 
    auto cid = *datum<int>(c, "cid");
    auto selected = *datum<bool>(c, "selected");
    
    hypriso->draw_thumbnail(cid, c->real_bounds, 10 * s);
    
    if (selected) {
        auto b = c->real_bounds;
        b.grow(3 * s);
        b.round();
        border(b, {.1, .6, .84, .8}, 1 * s, 0, 10 * s);
    }
}

static void fill_coverflow(Container *c, int monitor) {
    c->custom_type = (int) TYPE::COVERFLOW;
    *datum<int>(c, "monitor") = monitor;
    *datum<float>(c, "scroll") = 0.0f;
    c->automatically_paint_children = false;

    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto monitor = *datum<int>(c, "monitor");
        c->wanted_bounds = bounds_monitor(monitor);
        c->real_bounds = c->wanted_bounds;
        auto s = scale(monitor);

        std::vector<int> stack;
        for (auto o : get_window_stacking_order()) {
            if (hypriso->alt_tabbable(o)) {
                stack.push_back(o);
            }
        }
        
        merge_create<int>(c, stack, [](Container *c) {
            return *datum<int>(c, "cid");
        }, [](Container *parent, int cid) {
            auto ch = parent->child(FILL_SPACE, FILL_SPACE);
            *datum<int>(ch, "cid") = cid;
            *datum<bool>(ch, "selected") = false;
            ch->when_paint = paint_cover;
        });

        for (int i = 0; i < c->children.size(); i++) {
            auto ch = c->children[i];
            int cid = *datum<int>(ch, "cid");
            auto b = hypriso->thumbnail_size(cid);
            ch->wanted_bounds.w = b.w * .5;
            ch->wanted_bounds.h = b.h * .5;
        }

        static float spacing = 20;
        int offset = 0;
        float scroll = *datum<float>(c, "scroll");

        float total_w = 0;
        for (int i = 0; i < c->children.size(); i++) {
            auto ch = c->children[i];
            total_w += (ch->wanted_bounds.w + spacing) * s;
        }
        auto scale = total_w / full;
        if (scale > 1) {
            scroll *= scale;
        }

        for (int index = 0; index < stack.size(); index++) {
            for (int i = c->children.size() - 1; i >= 0; i--) {
                if (stack[index] == (*datum<int>(c->children[i], "cid"))) {
                    *datum<int>(c->children[i], "stacking_index") = index;
                    if (auto cidtainer = get_cid_container(stack[index])) {
                        *datum<long>(c->children[i], LAST_TIME_ACTIVE) = *datum<long>(cidtainer, LAST_TIME_ACTIVE);
                    }
                }
            }
        }
        

        std::sort(c->children.begin(), c->children.end(), [](Container* a, Container* b) { 
            auto a_active = *datum<long>(a, LAST_TIME_ACTIVE);
            auto b_active = *datum<long>(b, LAST_TIME_ACTIVE);
            if (a_active == b_active) {
                auto a_index = *datum<int>(a, "stacking_index");
                auto b_index = *datum<int>(b, "stacking_index");
                return b_index < a_index;
            } else {
                return a_active > b_active;
            }
        });

        float first_off = 0;
        if (!c->children.empty()) {
            first_off = -c->children[0]->real_bounds.w * .5;
        }
        

        for (int i = 0; i < c->children.size(); i++) {
            auto ch = c->children[i];
            auto x = c->real_bounds.x + c->real_bounds.w * .5 * s;
            auto y = c->real_bounds.y + c->real_bounds.h * .5 * s;
            ch->real_bounds = Bounds(x + offset + scroll + first_off, y - ch->wanted_bounds.h * .5, ch->wanted_bounds.w, ch->wanted_bounds.h);
            offset += ch->wanted_bounds.w + spacing;
        }

        bool allow = true;
        for (int i = c->children.size() - 1; i >= 0; i--) {
            auto ch = c->children[i];
            if (bounds_contains(ch->real_bounds, c->real_bounds.x + c->real_bounds.w * .5 * s, c->real_bounds.y + c->real_bounds.h * .5 * s)) {
                if (allow) {
                    allow = false;
                    *datum<bool>(ch, "selected") = true;
                }
            } else {
                *datum<bool>(ch, "selected") = false;
            }
        }
    };

    c->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        auto monitor = *datum<int>(c, "monitor");

        if (stage != (int) STAGE::RENDER_LAST_MOMENT && rid != monitor)
            return;
        renderfix
        if (!render_covers)
            return;

        c->automatically_paint_children = true;

        hypriso->draw_wallpaper(rid, c->real_bounds);
        rect(c->real_bounds, {0, 0, 0, .6}); 

        {
            auto bar = c->real_bounds;
            float h = bar.h * .3;
            bar.x += bar.w * .5;
            bar.y += bar.h * .5 - h * .5;
            bar.w = 1;
            bar.h = h;
            {
                auto black = bar;
                black.grow(1); 
                rect(black, {0, 0, 0, .4}, 0, 0, 2.0f, false); 
            }
            rect(bar, {1, 1, 1, 1}, 0, 0, 2.0f, true); 
        }
    };
    c->after_paint = [](Container *actual_root, Container *c) {
        c->automatically_paint_children = false;

        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        auto monitor = *datum<int>(c, "monitor");

        if (stage != (int) STAGE::RENDER_LAST_MOMENT && rid != monitor)
            return;
        renderfix


    };
}
                           
void coverflow::open() {
    later_immediate([](Timer *) {
        auto monitor = hypriso->monitor_from_cursor();
        hypriso->screenshot_wallpaper(monitor);
        hypriso->screenshot_all();
        auto cover = actual_root->child(FILL_SPACE, FILL_SPACE);
        fill_coverflow(cover, monitor);
        
        later(1000.0f / 30.0f, [](Timer *t) {
            t->keep_running = true;
            bool found = false;
            for (auto c : actual_root->children)
                if (c->custom_type == (int) TYPE::COVERFLOW)
                    found = true;
            if (!found)
                t->keep_running = false;
            render_covers = false;
            hypriso->screenshot_all();
            render_covers = true;
        });
    });
}

void coverflow::close(bool focus) {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::COVERFLOW) {
            if (focus) {
                for (auto ch : c->children) {
                    if (*datum<bool>(ch, "selected")) {
                        auto cid = *datum<int>(ch, "cid");
                        later_immediate([cid](Timer *) {
                            hypriso->bring_to_front(cid);
                        });
                    }
                }
            }
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    damage_all();
}

void coverflow::scroll(float x, float y) {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::COVERFLOW) {
            auto scroll = *datum<float>(c, "scroll");
            scroll += x;
            if (scroll > 0)
                scroll = 0;
            *datum<float>(c, "scroll") = scroll;
        }
    }
}

