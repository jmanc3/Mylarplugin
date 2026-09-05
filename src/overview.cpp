#include "overview.h"
#include "container.h"
#include "dock.h"
#include "drag_workspace_switcher.h"
#include "first.h"
#include "heart.h"
#include "hypriso.h"
#include "layout_thumbnails.h"
#include "titlebar.h"
#include "desktop_gesture.h"
#include "show_desktop.h"
#include "spring.h"

#include <linux/input-event-codes.h>

bool screenshotting_wallpaper = false;
bool running = false;
float openess = 0.0f;
float overview_open_time_ms = 700;
static bool initialized = false;
static unsigned int lifecycle = 0;
static unsigned int animation_generation = 0;
static bool animating = false;
static float animation_target = 0.0f;
static int overview_monitor = -1;

struct WindowOption {
    int cid;
    Bounds b;
};
std::vector<WindowOption> window_options;

static void paint_workspace(int monitor_id, int rendering_workspace_id, float openess) {
    window_options.clear();
    auto mb = bounds_monitor(monitor_id);
    auto s = scale(monitor_id);
    
    mb.scale(s);
    
    auto wallpaper_bounds = mb;
    //hypriso->draw_wallpaper(monitor_id, wallpaper_bounds);
    rect(wallpaper_bounds, {0, 0, 0, .7f}, 0, 0, 2.0, true);

    //rect(mb, RGBA(.16, .16, .16, 1.0));
    wallpaper_bounds.scale_from_center(1.0 - (.2 * openess));
    hypriso->draw_wallpaper(monitor_id, wallpaper_bounds, 14 * s * openess);
    auto b = wallpaper_bounds;
    render_drop_shadow(monitor_id, 1, {.1, .1, .1, openess}, 14 * s * openess, 2.0, b, 50 * s);
    b.shrink(2);
    border(b, {1, 1, 1, .05}, 1, 0, 14 * s); 


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
        auto b = bounds_client(o);
        auto size = hypriso->thumbnail_size_deco(o);
        auto extents = extents_client(o);
        auto height = size.h;
        auto width = size.w;
        auto x = b.x - extents.left - reserved.x;
        auto y = b.y - extents.top - reserved.y;
        auto cell = new DemoCell(o, x, y, width, height);
        cells.push_back(cell);
    }

    float pad = 120;

    layout.setCells(cells);
    layout.setAreaSize(reserved.w - reserved.x - pad, reserved.h - reserved.y - pad);
    if (set->overview_layout_type == "Grid") {
        layout.calculateGrid();
    } else {
        layout.calculate();
    }

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

    auto monitor_name = hypriso->monitor_name(monitor_id);
    
    for (int i = 0; i < cells.size(); i++) {
        auto democell = ((DemoCell *) cells[i]);
        int cid = democell->persistentKey();
        Bounds b = *datum<Bounds>(get_cid_container(cid), "overview_offset");
        if (!(b.x == 0 && b.y == 0)) {
            cells.erase(cells.begin() + i);
            cells.push_back(democell);
            break;
        }
    }
     

    for (int i = 0; i < cells.size(); i++) {
        auto democell = ((DemoCell *) cells[i]);
        int cid = democell->persistentKey();

        auto size = hypriso->thumbnail_size_deco(cid);
        auto extents = extents_client(cid);
        auto b = bounds_client(cid);
        b.x -= extents.left;
        b.y -= extents.top;
        b.w = size.w;
        b.h = size.h;
        b.scale(s);

        if (hypriso->is_hidden(cid)) {
            auto bounds = dock::get_location(monitor_name, cid);
            bounds.y += wallpaper_bounds.h;
            bounds.scale(s);
            b = lerp(bounds, b, openess);
        }
        
        auto r = democell->result();
        auto small_bounds = Bounds(r.x, r.y, r.w, r.h).scale(s);
        small_bounds.x += overx * (1/s);
        small_bounds.y += overy * (1/s);
        auto final_b = lerp(b, small_bounds, openess);

        auto offset = *datum<Bounds>(get_cid_container(cid), "overview_offset");
        final_b.x += offset.x * s;
        final_b.y += offset.y * s;
        
        hypriso->draw_deco_thumbnail(cid, final_b);
        window_options.push_back({cid, final_b.scale(1/s)});
    }
}

static void possibly_send_cid_to_workspace(int cid) {
    auto monitor = hypriso->monitor_from_cursor();
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            for (auto ch : c->children) {
                if (bounds_contains(ch->real_bounds, actual_root->mouse_current_x, actual_root->mouse_current_y)) {
                    auto space = *datum<int>(ch, "workspace");
                    if (space == -1) {
                        // next avaialable
                        auto spaces = hypriso->get_workspaces(monitor);
                        int next = 1;
                        if (!spaces.empty())
                            next = spaces[spaces.size() - 1] + 1;
                        later_immediate([monitor, cid, next](Timer *) {                                
                            //hypriso->set_hidden(cid, false);
                            hypriso->move_to_workspace(cid, next, false);
                            hypriso->bring_to_front(cid, false);

                            int active = hypriso->get_active_workspace_id(monitor);
                            for (auto o : hypriso->get_workspace_ids(monitor)) {
                                hypriso->screenshot_space(monitor, o);
                            }
                        });
                    } else {
                        later_immediate([monitor, cid, space](Timer *) {
                            auto before = hypriso->get_active_workspace_id(monitor);
                            hypriso->bring_to_front(cid, false);
                            hypriso->move_to_workspace(cid, hypriso->space_id_to_raw(space), false);
                            
                            int active = hypriso->get_active_workspace_id(monitor);
                            for (auto o : hypriso->get_workspace_ids(monitor)) {
                                if (active != o)
                                hypriso->screenshot_space(monitor, o);
                            }
                        });
                    }

                    return;
                }
            }
        }
    }
}

void create_overview_for_monitor(int monitor) {
    auto over = actual_root->child(FILL_SPACE, FILL_SPACE);
    over->custom_type = (int) TYPE::OVERVIEW;
    over->when_drag_end_is_click = false;
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
    
    static int cid_target;    
    cid_target = -1;
    over->when_drag_start = [](Container *root, Container *c) {
        cid_target = -1;
        for (auto o : window_options) {
            if (bounds_contains(o.b, root->mouse_current_x, root->mouse_current_y)) {
                cid_target = o.cid;
                break;
            }
        }
        if (cid_target != -1)
            *datum<Bounds>(get_cid_container(cid_target), "overview_offset") = Bounds(0, 0, root->mouse_current_x, root->mouse_current_y);
    };
    over->when_drag = [](Container *root, Container *c) {
        if (cid_target != -1) {
            auto b = datum<Bounds>(get_cid_container(cid_target), "overview_offset");
            b->x = root->mouse_current_x - b->w;
            b->y = root->mouse_current_y - b->h;
        }
    };
    over->when_drag_end = [](Container *root, Container *c) {
        if (cid_target != -1) {
            *datum<Bounds>(get_cid_container(cid_target), "overview_offset") = Bounds(0, 0, root->mouse_current_x, root->mouse_current_y);
            possibly_send_cid_to_workspace(cid_target);
        }
        cid_target = -1;
    };
    over->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed == BTN_LEFT) {
            for (auto o : window_options) {
                if (bounds_contains(o.b, root->mouse_current_x, root->mouse_current_y)) {
                    hypriso->bring_to_front(o.cid, true);
                    hypriso->set_hidden(o.cid, false, false);
                }
            }
            later_immediate([](Timer *) {
                overview::close();
            });
        } else if (c->state.mouse_button_pressed == BTN_RIGHT) {
            for (auto o : window_options) {
                if (bounds_contains(o.b, root->mouse_current_x, root->mouse_current_y)) {
                    titlebar::titlebar_right_click(o.cid); 
                }
            }
        } else if (c->state.mouse_button_pressed == BTN_MIDDLE) {
            for (auto o : window_options) {
                if (bounds_contains(o.b, root->mouse_current_x, root->mouse_current_y)) {
                    close_window(o.cid);
                }
            }
        }
    };
}

bool screenshots(int monitor) {
    bool ret = false;
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::CLIENT) {
            auto cid = *datum<int>(c, "cid");
            hypriso->screenshot_deco(cid);
        }
    }

    // TODO: every monitor needs it's own screenshot
    screenshotting_wallpaper = true;
    hypriso->screenshot_wallpaper(monitor);
    screenshotting_wallpaper = false;

    for (auto c : actual_root->children) 
        if (c->custom_type == (int) TYPE::OVERVIEW)
            ret = true;
    damage_all();
    return ret;
}

static void set_input_bypass(bool bypass) {
    if (hypriso->input_bypass_whitelist == bypass)
        return;
    hypriso->input_bypass_whitelist = bypass;
    hypriso->simulateMouseMovement();
}

static void hold_overview_open() {
    drag_workspace_switcher::open();
    // drag_workspace_switcher::force_hold_open(true);
    set_input_bypass(false);
}

static bool initialize_overview(int monitor) {
    if (running) {
        if (initialized)
            hold_overview_open();
        return true;
    }
    if (show_desktop::is_opened())
        return false;

    // Reserve ownership before deferred rendering setup can run.
    running = true;
    overview_monitor = monitor;
    const auto generation = ++lifecycle;
    later_immediate([monitor, generation](Timer *) {
        if (!running || generation != lifecycle)
            return;
        screenshots(monitor);
        hypriso->whitelist_on = true;
        for (auto m : actual_monitors) {
            auto mid = *datum<int>(m, "cid");
            create_overview_for_monitor(mid);
        }
        initialized = true;
        hold_overview_open();
        request_refresh();
    });

    later(1000.0f / hypriso->fps(monitor), [monitor, generation](Timer *t) {
        t->keep_running = running && generation == lifecycle;
        if (t->keep_running && initialized)
            t->keep_running = screenshots(monitor);
    });
    return true;
}

void overview_actual_close() {
    const bool was_initialized = initialized;
    lifecycle++;
    animation_generation++;
    animating = false;
    initialized = false;
    openess = 0.0;
    running = false;
    drag_workspace_switcher::close();
    window_options.clear();

    auto m = actual_root;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            delete c;
            m->children.erase(m->children.begin() + i);
        }
    }
    if (was_initialized) {
        hypriso->whitelist_on = false;
        hypriso->input_bypass_whitelist = false;
        hypriso->simulateMouseMovement();
    }
    damage_all();
    request_refresh();
}

static void animate_overview(float target, float velocity, SpringParams params, bool gesture_release = false) {
    const auto generation = ++animation_generation;
    animating = false;
    if (openess == target) {
        if (target == 0.0f)
            overview_actual_close();
        return;
    }

    animating = true;
    animation_target = target;
    const auto initial_progress = openess;
    later((1000.0f / hypriso->fps(overview_monitor)) * .8,
          [generation, initial_progress, target, velocity, params, gesture_release, start = 0L](Timer *t) mutable {
        t->keep_running = running && generation == animation_generation;
        if (!t->keep_running || !initialized)
            return;
        const auto now = get_current_time_in_ms();
        if (start == 0)
            start = now;
        const auto state = springEvaluate((now - start) / 1000.0, initial_progress, target, velocity, params);
        const bool finished = gesture_release
            ? (target == 0.0f ? state.value <= .001 : state.value >= .999)
            : (std::abs(state.value - target) <= .001 && std::abs(state.velocity) <= .001);
        openess = finished ? target : static_cast<float>(state.value);
        if (target == 0.0f && openess < .3f)
            set_input_bypass(true);
        if (finished) {
            animating = false;
            t->keep_running = false;
            if (target == 0.0f)
                overview_actual_close();
        }
        request_refresh();
    });
}

void overview::open(int monitor) {
    if (!initialize_overview(monitor))
        return;
    if (!animating || animation_target != 1.0f)
        animate_overview(1.0f, 0.0f, {overview_open_time_ms / 2000.0, .97});
}

void overview::close(bool focus) {
    drag_workspace_switcher::close();
    if (!running || (animating && animation_target == 0.0f))
        return;
    if (initialized)
        set_input_bypass(true);
    animate_overview(0.0f, 0.0f, {overview_open_time_ms / 2000.0, 1.0});
}

void overview::begin_gesture(int monitor) {
    if (!initialize_overview(monitor))
        return;
    animation_generation++;
    animating = false;
    openess = std::clamp(openess, 0.0f, 1.0f);
    request_refresh();
}

void overview::end_gesture(long start, long end, float y_offset) {
    if (!running)
        return;
    if (openess < .01f) {
        overview_actual_close();
    } else if (openess > .99f) {
        overwrite_openess(1.0f);
    } else {
        const auto release = desktop_gesture::release(start, end, -y_offset, openess);
        animate_overview(release.target, release.velocity, {.3, 1.0}, true);
    }
}

void overview::instant_close() {
    if (running)
        overview_actual_close();
}

void overview::click(int id, int button, int state, float x, float y) {

}

void overview::overwrite_openess(float a) {
    if (!running)
        return;
    animation_generation++;
    animating = false;
    openess = std::clamp(a, 0.0f, 1.0f);
    if (initialized)
        set_input_bypass(false);
    request_refresh();
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
