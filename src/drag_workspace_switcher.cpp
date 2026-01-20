#include "drag_workspace_switcher.h"

#include "heart.h"
#include "container.h"
#include "titlebar.h"
#include "hypriso.h"
#include <fcntl.h>
#include <unistd.h>

static bool switcher_showing = false;
static bool hold_open = false;

// {"anchors":[{"x":0,"y":1},{"x":0.4,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.25099658672626207,"y":0.7409722222222223},{"x":0.6439499918619792,"y":0.007916683620876747}]}
static std::vector<float> slidetopos2 = { 0, 0.017000000000000015, 0.03500000000000003, 0.05400000000000005, 0.07199999999999995, 0.09199999999999997, 0.11099999999999999, 0.132, 0.15200000000000002, 0.17400000000000004, 0.19599999999999995, 0.21899999999999997, 0.242, 0.266, 0.29100000000000004, 0.31699999999999995, 0.344, 0.372, 0.4, 0.43000000000000005, 0.46099999999999997, 0.494, 0.527, 0.563, 0.6, 0.626, 0.651, 0.675, 0.6970000000000001, 0.719, 0.739, 0.758, 0.777, 0.794, 0.8109999999999999, 0.8260000000000001, 0.841, 0.855, 0.868, 0.881, 0.892, 0.903, 0.914, 0.923, 0.9319999999999999, 0.9410000000000001, 0.948, 0.955, 0.962, 0.968, 0.973, 0.978, 0.983, 0.986, 0.99, 0.993, 0.995, 0.997, 0.998, 0.999, 1 };

// {"anchors":[{"x":0,"y":1},{"x":0.30000000000000004,"y":0.675},{"x":1,"y":0}],"controls":[{"x":0.20596153063651845,"y":0.9462499830457899},{"x":0.4476281973031851,"y":0.06847220526801215}]}
static std::vector<float> snapback = { 0, 0.0050000000000000044, 0.010000000000000009, 0.017000000000000015, 0.02400000000000002, 0.03300000000000003, 0.04300000000000004, 0.05400000000000005, 0.06699999999999995, 0.08099999999999996, 0.09599999999999997, 0.11399999999999999, 0.134, 0.15600000000000003, 0.18200000000000005, 0.20999999999999996, 0.243, 0.281, 0.32499999999999996, 0.387, 0.43999999999999995, 0.486, 0.527, 0.563, 0.596, 0.626, 0.654, 0.679, 0.7030000000000001, 0.725, 0.745, 0.764, 0.782, 0.798, 0.8140000000000001, 0.8280000000000001, 0.842, 0.855, 0.867, 0.878, 0.888, 0.898, 0.908, 0.917, 0.925, 0.933, 0.94, 0.947, 0.953, 0.959, 0.964, 0.969, 0.974, 0.978, 0.982, 0.986, 0.989, 0.993, 0.995, 0.998, 1 };

template<class T>
void merge_create(Container *parent, std::vector<T> to_be_represented, std::function<T (Container *)> converter, std::function<void (Container *, T)> creator) {
    // Get rid of containers that no longer are represented
    for (int i = parent->children.size() - 1; i >= 0; i--) {
        auto ch = parent->children[i];
        bool found = false;
        T ct = converter(ch);
        for (auto t : to_be_represented)
            if (t == ct)
                found = true;
        if (!found) {
            delete ch;
            parent->children.erase(parent->children.begin() + i);
        }
    }

    // Create container if doesn't exist yet
    for (auto t : to_be_represented) {
        bool found = false;
        for (auto c : parent->children) {
            if (t == converter(c)) {
                found = true;
                break;
            }
        }
        if (!found) {
            creator(parent, t);
        }
    }
}

void layout_spaces(Container *actual_root, Container *parent, int monitor) {
    auto openess = *datum<float>(parent, "openess");
    auto b = parent->real_bounds;
    int spacing = 13;
    if (openess != 0.0) {
        b.shrink(30);
    }
    int pen_x = b.x + spacing;
    int pen_y = b.y + spacing;
    auto thumb_h = b.h - 50;
    auto monb = bounds_monitor(monitor);
    auto thumb_w = thumb_h * (monb.w / monb.h);
    for (int i = 0; i < parent->children.size(); i++) {
        auto ch = parent->children[i];
        ch->wanted_bounds = Bounds(pen_x, pen_y, thumb_w, thumb_h);
        ch->real_bounds = ch->wanted_bounds;
        pen_x += thumb_w + spacing;
    }
}

void drag_switcher_actual_open() {
    hold_open = false;
    auto monitor = hypriso->monitor_from_cursor();
    auto c = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    *datum<float>(c, "openess") = 0.0;
    auto peaking_amount = datum<float>(c, "peaking_amount");
    *peaking_amount = 0.0;
    animate(peaking_amount, 1.0, 200.0, c->lifetime, nullptr, [](float a) {
        return pull(snapback, a);
    });
    c->custom_type = (int) TYPE::WORKSPACE_SWITCHER;
    c->pre_layout = [monitor](Container *actual_root, Container *c, const Bounds &bounds) {
        auto openess = *datum<float>(c, "openess");
        auto peaking_amount = *datum<float>(c, "peaking_amount");
        
        auto b = bounds_monitor(monitor);
        auto new_h = 30;
        {
            auto new_w = b.w * .45;
            b.x += b.w * .5 - new_w * .5;
            b.w = new_w;
        }
        {
            b.h = 140;
        }

        b.y = (-b.h * (1.0 - openess)) + (new_h * peaking_amount) - (8 * openess);
        if (openess != 0.0) {
            b.grow(new_h);
        }

        merge_create<int>(c, hypriso->get_workspace_ids(monitor), [](Container *c) {
            return *datum<int>(c, "workspace");
        }, [monitor](Container *parent, int space) {
            auto c = parent->child(FILL_SPACE, FILL_SPACE);
            *datum<int>(c, "workspace") = space;
            c->when_paint = [monitor](Container *actual_root, Container *c) {
                auto root = get_rendering_root();
                if (!root) return;
                auto [rid, s, stage, active_id] = roots_info(actual_root, root);
                if (rid != monitor || stage != (int) STAGE::RENDER_LAST_MOMENT)
                    return;
                renderfix

                render_drop_shadow(monitor, 1.0, {0, 0, 0, .1}, 8 * s, 2.0, c->real_bounds);
                hypriso->draw_wallpaper(monitor, c->real_bounds, 8 * s);
                
                auto b = c->real_bounds;
                b.shrink(2.0);
                b.round();
                if (c->state.mouse_hovering) {
                    border(b, {1, 1, 1, .2}, 1.0, 0, 8 * s, 2.0, false); 
                } else {
                    border(b, {1, 1, 1, .04}, 1.0, 0, 8 * s, 2.0, false); 
                }
            };
            c->when_clicked = paint {
                auto space = *datum<int>(c, "workspace");
                hypriso->move_to_workspace_id(space);
                notify("hello");

            };
        });

        c->wanted_bounds = b;
        c->real_bounds = c->wanted_bounds;

        layout_spaces(actual_root, c, monitor);
    };
    c->when_paint = [monitor](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (rid != monitor || stage != (int) STAGE::RENDER_LAST_MOMENT)
            return;
        auto openess = *datum<float>(c, "openess");
        auto peaking_amount = *datum<float>(c, "peaking_amount");
        auto backup = c->real_bounds;
        request_damage(actual_root, c);
        defer(c->real_bounds = backup);
        auto new_h = 30;
        if (openess != 0.0) {
            c->real_bounds.shrink(new_h); 
        }
        renderfix;

        RGBA col = {.18, .18, .18, .9f * peaking_amount};
        auto b = c->real_bounds;
        render_drop_shadow(monitor, 1.0, {0, 0, 0, .27f * peaking_amount}, 8 * s, 2.0, b);
        if (openess == 0.0) {
            rect(b, col, 3, 8 * s * (1 - openess), 2.0, true, 1.0); 
        } else {
            rect(b, col, 0, 8 * s * openess, 2.0, true, 1.0); 
        }
        b.shrink(1.0); 
        border(b, {.3, .3, .3, 1 * peaking_amount}, 1.0f, 3, 8 * s, 2.0, false); 

        auto icon = get_cached_texture(root, c, "drag_text_icon", "Segoe Fluent Icons", "\uf407", {.8, .8, .8, 1}, 13);
        draw_texture(*icon, c->real_bounds.x + 14 * s, c->real_bounds.y + c->real_bounds.h - icon->h * 1.60, peaking_amount);

        auto t = get_cached_texture(root, c, "drag_text", mylar_font, "Drag a window here to move it to another workspace.", 
            {.8, .8, .8, 1}, 13);
        draw_texture(*t, 
            c->real_bounds.x + 14 * s + 10 * s + icon->w, 
            c->real_bounds.y + c->real_bounds.h - t->h * 1.30, 
            peaking_amount);
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

static void actual_drag_workspace_switcher_close() {
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

void drag_workspace_switcher::close() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            auto peaking_amount = datum<float>(c, "peaking_amount");
            auto openess = datum<float>(c, "openess");
            animate(openess, 0.0, 200.0, c->lifetime,
                [](bool normal_end) {
                    actual_drag_workspace_switcher_close();
                }, [](float a) {
                    return pull(snapback, a);
                });
            animate(peaking_amount, 0.0, 200.0, c->lifetime, nullptr, [](float a) {
                return pull(snapback, a);
            });
        }
    }
}


void drag_workspace_switcher::click(int id, int button, int state, float x, float y) {

}

void drag_workspace_switcher::on_mouse_move(int x, int y) {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            if (bounds_contains(c->real_bounds, x, y) || hold_open) {
                if (!c->state.mouse_hovering) {
                    auto openess = datum<float>(c, "openess");
                    animate(openess, 1.0, 180.0, c->lifetime, nullptr, [](float a) {
                        return pull(slidetopos2, a);
                    });
                }
                c->state.mouse_hovering = true;
            } else {
                if (c->state.mouse_hovering) {
                    auto openess = datum<float>(c, "openess");
                    animate(openess, 0.0, 200.0, c->lifetime, nullptr, [](float a) {
                        return pull(snapback, a);
                    });
                }
                c->state.mouse_hovering = false;
            }
            request_damage(actual_root, c);
        }
    }
}

void drag_workspace_switcher::force_hold_open(bool state) {
    hold_open = state;
    auto m = mouse();
    drag_workspace_switcher::on_mouse_move(m.x, m.y);
}
