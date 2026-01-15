/**
 * 
 * Event pumper and distributor
 * 
 */

#include "heart.h"

#include "container.h"
#include "hypriso.h"
#include "titlebar.h"
#include "events.h"
#include "icons.h"
#include "hotcorners.h"
#include "alt_tab.h"
#include "drag.h"
#include "resizing.h"
#include "dock.h"
#include "snap_preview.h"
#include "popup.h"
#include "quick_shortcut_menu.h"
#include "settings.h"
#include "snap_assist.h"
#include "overview.h"

#include "process.hpp"
#include <cstdio>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <wayland-server-protocol.h>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

#include <algorithm>
#include <linux/input-event-codes.h>
#include <thread>

static bool META_PRESSED = false;
static float zoom_factor = 1.0;
static long zoom_nicely_ended_time = 0;
static bool zoom_needs_speed_update = true;

static bool mouse_down = false;
//static bool and_on_desktop = false;

std::unordered_map<std::string, WindowRestoreLocation> restore_infos;

std::unordered_map<std::string, Datas> datas;

std::vector<Container *> actual_monitors; // actually just root of all
Container *actual_root = new Container;

void draw_text(std::string text, int x, int y);
void apply_restore_info(int id);

static RGBA color_sel_color() {
    static RGBA default_color("00000088");
    return hypriso->get_varcolor("plugin:mylardesktop:sel_color", default_color);
}

static RGBA color_sel_border_color() {
    static RGBA default_color("00000088");
    return hypriso->get_varcolor("plugin:mylardesktop:sel_border_color", default_color);
}

static void any_container_closed(Container *c) {
    remove_data(c->uuid); 
}

static void set_exists(Container *c, bool state) {
    c->exists = state; 
}

static bool on_mouse_move(int id, float x, float y) {
    second::layout_containers();
    auto mou = mouse();
    x = mou.x;
    y = mou.y;
    snap_preview::on_mouse_move(x, y); 

    if (drag::dragging()) {
        drag::motion(drag::drag_window());
        return true;
    }
    if (resizing::resizing()) {
        resizing::motion(resizing::resizing_window());
        return true;
    }
    
    //notify(fz("{} {}", x, y));
    int active_mon = hypriso->monitor_from_cursor();
    {
        auto m = actual_root;
        auto cid = *datum<int>(m, "cid");
        auto bounds = bounds_monitor(cid);
        auto [rid, s, stage, active_id] = from_root(m);
        Event event(x - bounds.x, y - bounds.y);
        //notify(fz("{} {}                       ", event.x, event.y));
        
        move_event(m, event);
    }

    bool consumed = false;
    {
        auto root = actual_root;
        if (root->consumed_event) {
            consumed = true;
            root->consumed_event = false;
        }
    }

    if (!consumed && !mouse_down) {
        auto current = get_current_time_in_ms();
        auto time_since = (current - zoom_nicely_ended_time);
        if (zoom_factor == 1.0 && time_since > 500)
            hotcorners::motion(id, x, y);
    }

    return consumed;
}

static void create_root_popup() {
    auto m = mouse();
    std::vector<PopOption> root;
    {
        PopOption pop;
        pop.text = "Configure Display Settings...";   
        pop.on_clicked = []() {
            settings::start();
        };
        root.push_back(pop);
    }
 
    PopOption pop;
    pop.seperator = true;
    root.push_back(pop);        

    {
        PopOption pop;
        pop.text = "Log out";   
        pop.on_clicked = []() {
            hypriso->logout();
        };
        root.push_back(pop);
    }

    popup::open(root, m.x - 1, m.y + 1);
}

static bool on_mouse_press(int id, int button, int state, float x, float y) {
    mouse_down = state;
    second::layout_containers();
    
    auto mou = mouse();
    x = mou.x;
    y = mou.y;
    auto pierced = pierced_containers(actual_root, x, y);
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
       auto child = actual_root->children[i];
       if (child->custom_type == (int) TYPE::OUR_POPUP) {
           bool was_pierced = false;
           for (auto p : pierced)
               if (p == child)
                   was_pierced = true;
           if (!was_pierced) {
               delete child;
               actual_root->children.erase(actual_root->children.begin() + i);
           }
       }
    }

    snap_assist::click(id, button, state, x, y);
    overview::click(id, button, state, x, y);
    
    bool consumed = false;
    if (drag::dragging() && !state) {
        drag::end(drag::drag_window());
    }
    if (resizing::resizing() && !state) {
        resizing::end(resizing::resizing_window());
    }
 
    if (alt_tab::showing()) {
        for (auto c : actual_root->children) {
            if (c->custom_type == (int)TYPE::ALT_TAB) {
                if (!bounds_contains(c->real_bounds, x, y)) {
                    alt_tab::close(true); 
                }
            }
        }
    }
 
    {
        auto m = actual_root; 
        auto cid = *datum<int>(m, "cid");
        auto bounds = bounds_monitor(cid);
        auto [rid, s, stage, active_id] = from_root(m);
        Event event(x - bounds.x, y - bounds.y, button, state);
        mouse_event(m, event);
    }

    {
        auto root = actual_root;
        if (root->consumed_event) {
           consumed = true;
           root->consumed_event = false;
        } 
    }
    return consumed;
}

static bool on_scrolled(int id, int source, int axis, int direction, double delta, int discrete, bool from_mouse) {
    auto m = mouse();
    int active_mon = hypriso->monitor_from_cursor();
    //auto s = scale(active_mon);
    Event event;
    event.x = m.x;
    event.y = m.y;
    event.scroll = true;
    event.axis = axis;
    event.direction = direction;
    event.delta = delta;
    event.descrete = discrete;
    event.from_mouse = from_mouse;
    second::layout_containers();
    {
        auto m = actual_root;
        auto cid = *datum<int>(m, "cid");
        auto bounds = bounds_monitor(cid);
        auto [rid, s, stage, active_id] = from_root(m);
        event.x -= bounds.x;
        event.y -= bounds.y;
        //Event event(x - bounds.x, y - bounds.y, button, state);
        mouse_event(m, event);
    }

    bool consumed = false;
    {
        auto root = actual_root; 
        if (root->consumed_event) {
            consumed = true;
            root->consumed_event = false;
        }
    }

    bool current_was_mouse = source == WL_POINTER_AXIS_SOURCE_WHEEL;
    auto current = get_current_time_in_ms();
    auto time_since = (current - zoom_nicely_ended_time);
    if (META_PRESSED && time_since > 1000) {
        auto dtdt = (delta * .05);
        if (!current_was_mouse) {
            dtdt *= .35; // slow down on touchpads
        }
        auto before_zf = zoom_factor;
        zoom_factor -= dtdt; 
        if (zoom_factor < 1.0)
            zoom_factor = 1.0;
        if (zoom_factor > 10.0)
            zoom_factor = 10.0;
        bool nicely_ended = false;
        bool last_event = false;
        if (before_zf > 1.0 && zoom_factor <= 1.0)
            last_event = true;
        bool likely = zoom_factor < 1.2; // This is good enough for touchpad but not scroll wheel
        if (current_was_mouse && last_event) {
            likely = true;
        }

        if (delta > 0 && likely) { // Recognize likely attempted to end zoom and do it cleanly for user
           hypriso->overwrite_animation_speed(4.0);
           zoom_factor = 1.0; 
           zoom_nicely_ended_time = get_current_time_in_ms();
           nicely_ended = true;
           zoom_needs_speed_update = true;
        }
        if (!nicely_ended) {
            static bool previous_was_mouse = true;
            if (previous_was_mouse != current_was_mouse || zoom_needs_speed_update) {
                previous_was_mouse = current_was_mouse;
                if (current_was_mouse) {
                    hypriso->overwrite_animation_speed(2.5);
                } else {
                    hypriso->overwrite_animation_speed(.04);
                }
            }
        }
            
        hypriso->set_zoom_factor(zoom_factor);
        return true;
    }
    if (time_since < 500) // consume scrolls which are likely referring to the zoom effect and not to the window focused
        return true;

    return consumed;
}

void toggle_layout() {
    auto s = hypriso->get_active_workspace_id(hypriso->monitor_from_cursor());
    auto tiling = hypriso->is_space_tiling(s);
    hypriso->set_space_tiling(s, !tiling);
    std::vector<int> order = get_window_stacking_order();
    for (auto o : order) {
        if (hypriso->get_active_workspace_id_client(o) == s) {
            if (hypriso->alt_tabbable(o)) {
                if (tiling) {
                    // change to float if not already
                    if (!hypriso->is_floating(o)) {
                        hypriso->set_float_state(o, true);

                        //apply_restore_info(o);
                        if (auto c = get_cid_container(o)) {
                            if (*datum<bool>(c, "snapped"))
                                hypriso->should_round(o, false);

                            auto p = *datum<Bounds>(c, "pre_mode_change_position");
                            auto now = bounds_client(o);
                            hypriso->move_resize(o, now.x, now.y, now.w, now.h, true);
                            hypriso->move_resize(o, p.x, p.y, p.w, p.h, false);
                        }
                    }
                } else {
                    // change to tiling if not already
                    if (auto c = get_cid_container(o)) {
                        *datum<Bounds>(c, "pre_mode_change_position") = bounds_client(o);
                    }
                    hypriso->set_float_state(o, false);
                    hypriso->should_round(o, true);
                }
            }
        }
    } 
}

static bool on_key_press(int id, int key, int state, bool update_mods) {
    bool consume = quick_shortcut_menu::on_key_press(id, key, state, update_mods);
    if (consume)
        return consume;
    
    static bool alt_held = false;
    if (key == KEY_LEFTALT || key == KEY_RIGHTALT) {
        alt_held = state;
    }
    static bool shift_held = false;
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
        shift_held = state;
    }
    if (alt_held && shift_held) {
        META_PRESSED = true;
    } else {
        META_PRESSED = false;
    }
    if (alt_held) {
        if (key == KEY_TAB) {
            if (state) {
                alt_tab::show();
                if (shift_held) {
                    alt_tab::move(-1);
                } else {
                    alt_tab::move(1);
                }
            }
        }
    }
    bool alt_showing = alt_tab::showing();
    if (!alt_held && alt_showing) {
       alt_tab::close(true); 
    }
    
    if (key == KEY_TAB && state == 0) {
        //hypriso->no_render = !hypriso->no_render;
        //nz(fz(
          //"rendering {}", !hypriso->no_render  
        //));
    }

    if (key == KEY_ESC && state == 0) {
        if (drag::dragging()) {
            drag::end(drag::dragging());
        }
        if (resizing::resizing()) {
            resizing::end(resizing::resizing_window());
        }
        if (alt_tab::showing()) {
            alt_tab::close(false);
        }
        for (auto c : actual_root->children) {
            if (c->custom_type == (int) TYPE::OUR_POPUP) {
                popup::close(c->uuid);
            }
        }
        snap_assist::close();
        overview::close();
        META_PRESSED = false;
        zoom_factor = 1.0;
    }

    if (alt_held && key == KEY_SPACE && state == 1) {
        toggle_layout();
    }

    return false;
}

SnapLimits get_snap_limits(int monitor, SnapPosition wanted_pos) {
    SnapLimits limits = {.5f, .5f, .5f};
    
    for (auto ch : actual_root->children) {
        if (ch->custom_type == (int) TYPE::CLIENT) {
            auto cid = *datum<int>(ch, "cid");
            if (monitor != get_monitor(cid))
                continue;
            
            auto snapped = *datum<bool>(ch, "snapped");
            if (!snapped)
                continue;

            auto snap_type = *datum<int>(ch, "snap_type");
            if (snap_type == (int) SnapPosition::MAX)
                break; // if we find a max, no snap possability

            auto other_cdata = (ClientInfo*) ch->user_data; 

            Bounds reserved = bounds_reserved_monitor(monitor);
            std::vector<int> groups;
            groups.push_back(cid);
            for (auto g : other_cdata->grouped_with)
                groups.push_back(g);

            if (!groupable(wanted_pos, groups))
                break;
            
            for (auto g : groups) {
                auto b = bounds_client_final(g);
                auto g_snap_type = (SnapPosition) *datum<int>(get_cid_container(g), "snap_type");
                bool has_decos = hypriso->has_decorations(g);
                if (g_snap_type == SnapPosition::TOP_LEFT) {
                    if (has_decos)
                        limits.left_middle = (b.h + titlebar_h) / reserved.h;
                    else
                        limits.left_middle = (b.h) / reserved.h;
                    limits.middle_middle = (b.w) / reserved.w;
                } else if (g_snap_type == SnapPosition::TOP_RIGHT) {
                    if (has_decos)
                        limits.right_middle = (b.h + titlebar_h) / reserved.h;
                    else
                        limits.right_middle = (b.h) / reserved.h;
                    limits.middle_middle = 1.0 - ((b.w) / reserved.w);
                } else if (g_snap_type == SnapPosition::BOTTOM_LEFT) {
                    if (has_decos)
                        limits.left_middle = (b.y - titlebar_h) / reserved.h;
                    else
                        limits.left_middle = (b.y) / reserved.h;
                    limits.middle_middle = (b.w) / reserved.w;
                } else if (g_snap_type == SnapPosition::BOTTOM_RIGHT) {
                    if (has_decos)
                        limits.right_middle = (b.y - titlebar_h) / reserved.h;
                    else
                        limits.right_middle = (b.y) / reserved.h;
                    limits.middle_middle = 1.0 - ((b.w) / reserved.w);
                } else if (g_snap_type == SnapPosition::LEFT) {
                    limits.middle_middle = (b.w) / reserved.w;
                } else if (g_snap_type == SnapPosition::RIGHT) {
                    limits.middle_middle = 1.0 - ((b.w) / reserved.w);
                }
            }
            if (limits.left_middle == .5 && limits.right_middle != .5)
                limits.left_middle = limits.right_middle; 
            if (limits.right_middle == .5 && limits.left_middle != .5)
                limits.right_middle = limits.left_middle; 

            break;
         }
    }

    return limits;
}

Bounds snap_position_to_bounds_limited(int mon, SnapPosition pos, SnapLimits limits) {
    Bounds screen = bounds_reserved_monitor(mon);

    float x = screen.x;
    float y = screen.y;
    float w = screen.w;
    float h = screen.h;

    Bounds out = {x, y, w, h};

    if (pos == SnapPosition::MAX) {
        return {x, y, w, h};
    } else if (pos == SnapPosition::LEFT) {
        return {x, y, w * limits.middle_middle, h};
    } else if (pos == SnapPosition::RIGHT) {
        return {x + w * limits.middle_middle, y, w - (w * limits.middle_middle), h};
    } else if (pos == SnapPosition::TOP_LEFT) {
        return {x, y, w * limits.middle_middle, h * limits.left_middle};
    } else if (pos == SnapPosition::TOP_RIGHT) {
        return {x + w * limits.middle_middle, y, w - w * limits.middle_middle, h * limits.right_middle};
    } else if (pos == SnapPosition::BOTTOM_LEFT) {
        return {x, y + h * limits.left_middle, w * limits.middle_middle, h - h * limits.left_middle};
    } else if (pos == SnapPosition::BOTTOM_RIGHT) {
        return {x + w * limits.middle_middle, y + h * limits.right_middle, w - w * limits.middle_middle, h - h * limits.right_middle};
    }

    return out;
}

SnapPosition mouse_to_snap_position(int mon, int x, int y) {
    Bounds pos = bounds_reserved_monitor(mon);

    const float sideThreshX = pos.w * 0.05f;
    const float sideThreshY = pos.h * 0.05f;
    const float rightEdge = pos.x + pos.w;
    const float bottomEdge = pos.y + pos.h;
    float edgeThresh = 10.0;
    bool on_top_edge = y < pos.y + edgeThresh;
    bool on_bottom_edge = y > bottomEdge - edgeThresh;
    edgeThresh = 50.0;
    bool on_left_edge = x < pos.x + edgeThresh;
    bool on_right_edge = x > rightEdge - edgeThresh;
    bool on_left_side = x < pos.x + sideThreshX;
    bool on_right_side = x > rightEdge - sideThreshX;
    bool on_top_side = y < pos.y + sideThreshY;
    bool on_bottom_side = y > bottomEdge - sideThreshY;

    if ((on_top_edge && on_left_side) || (on_left_edge && on_top_side)) {
        return SnapPosition::TOP_LEFT;
    } else if ((on_top_edge && on_right_side) || (on_right_edge && on_top_side)) {
        return SnapPosition::TOP_RIGHT;
    } else if ((on_bottom_edge && on_left_side) || (on_left_edge && on_bottom_side)) {
        return SnapPosition::BOTTOM_LEFT;
    } else if ((on_bottom_edge && on_right_side) || (on_right_edge && on_bottom_side)) {
        return SnapPosition::BOTTOM_RIGHT;
    } else if (on_top_edge) {
        return SnapPosition::MAX;
    } else if (on_left_edge) {
        return SnapPosition::LEFT;
    } else if (on_right_edge) {
        return SnapPosition::RIGHT;
    } else if (on_bottom_edge) {
        return SnapPosition::MAX;
    } else {
        return SnapPosition::NONE;
    }

    return SnapPosition::NONE;
}

Bounds snap_position_to_bounds(int mon, SnapPosition pos) {
    auto limits = get_snap_limits(mon, pos);
    
    return snap_position_to_bounds_limited(mon, pos, limits);
}

SnapPosition opposite_snap_position(SnapPosition pos) {
    if (pos == SnapPosition::NONE) {
        return SnapPosition::MAX;
    } else if (pos == SnapPosition::MAX) {
        return SnapPosition::NONE;
    } else if (pos == SnapPosition::LEFT) {
        return SnapPosition::RIGHT;
    } else if (pos == SnapPosition::RIGHT) {
        return SnapPosition::LEFT;
    } else if (pos == SnapPosition::TOP_LEFT) {
        return SnapPosition::BOTTOM_LEFT;
    } else if (pos == SnapPosition::TOP_RIGHT) {
        return SnapPosition::BOTTOM_RIGHT;
    } else if (pos == SnapPosition::BOTTOM_LEFT) {
        return SnapPosition::TOP_LEFT;
    } else if (pos == SnapPosition::BOTTOM_RIGHT) {
        return SnapPosition::TOP_RIGHT;
    }
    return pos;
}

void paint_snap_preview(Container *actual_root, Container *c) {
    snap_preview::draw(actual_root, c);
    
    auto root = get_rendering_root();
    if (!root)
        return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    auto cid = *datum<int>(c, "cid");

    if (active_id == cid && stage == (int)STAGE::RENDER_PRE_WINDOW) {
        renderfix 
        if (*datum<bool>(c, "snapped") && (*datum<int>(c, "snap_type") != (int)SnapPosition::MAX)) {
            auto b = c->real_bounds;
            b.shrink(1);
            auto a = *datum<float>(c, "titlebar_alpha");
            border(b, {.5, .5, .5, (float) (.8 * a)}, 1);
        }
        //rect({root->real_bounds.x, hypriso->pass_info(cid).cby, root->real_bounds.w, 1}, {1, 0, 0, 1});
    }
}

void fit_on_screen(int cid)  {
    int mon = get_monitor(cid);
    auto reserved = bounds_reserved_monitor(mon);
    auto bounds = bounds_client(cid);
    bool needs = false;
    float amount = .85;
    if (bounds.w > reserved.w * amount) {
        needs = true;
        bounds.w = reserved.w * amount;
    }
    if (bounds.h > reserved.h * amount) {
        needs = true;
        bounds.h = reserved.h * amount;
    }
    if (!needs)
        return;
    hypriso->move_resize(cid, 
        reserved.x + reserved.w * .5 - bounds.w * .5,
        reserved.y + reserved.h * .5 - bounds.h * .5,
        bounds.w, bounds.h);
}

void apply_restore_info(int id) {
    //auto tc = c_from_id(id);
    auto monitor = get_monitor(id);
    auto cname = hypriso->class_name(id);
    for (auto [class_n, info] : restore_infos) {
        if (cname == class_n) {
            //notify(fz("{} {} {}", cname, info.keep_above, info.fake_fullscreen));
            //hypriso->pin(id, info.keep_above);
            //hypriso->fake_fullscreen(id, info.fake_fullscreen);

            // Skip restore info if class name is same as parent class name (dialogs)
            int parent = hypriso->parent(id);
            if (parent != -1) {
                auto pname = hypriso->class_name(parent);
                if (pname == cname) {
                    continue;
                }
            }
            
            auto b = bounds_client(id);
            auto s = scale(monitor);
            auto b2 = bounds_reserved_monitor(monitor);
            b.w = b2.w * info.box.w;
            b.h = b2.h * info.box.h;
            if (b.w >= b2.w - 60 * s) {
                b.w = b2.w - 60 * s;
            }
            bool fix = false;
            if (b.h >= b2.h - 60 * s) {
                b.h = b2.h - 60 * s;
                fix = true;
            }
            b.x = b2.x + b2.w * .5 - b.w * .5;
            b.y = b2.y + b2.h * .5 - b.h * .5;
            if (fix)
                b.y += (titlebar_h * s) * .5;

            if (info.remember_size)
                hypriso->move_resize(id, b.x, b.y, b.w, b.h);

            if (info.remember_workspace)
                hypriso->move_to_workspace(id, info.remembered_workspace);
        }
    }
}

static void on_window_open(int id) {    
    // We make the client on the first monitor we fine, because we move the container later based on actual monitor location
    {
        auto m = actual_root; 
        auto c = m->child(FILL_SPACE, FILL_SPACE);
        c->custom_type = (int) TYPE::CLIENT;
        c->when_paint = paint_snap_preview;
        c->handles_pierced = [](Container* c, int x, int y) {
            auto cid = *datum<int>(c, "cid");
            bool inside = bounds_contains(c->real_bounds, x, y);
            bool on_workspace = (hypriso->get_workspace(cid) == hypriso->get_active_workspace(hypriso->monitor_from_cursor())); 
            return inside && on_workspace;
        };
        c->when_mouse_down = paint {
            if (META_PRESSED && c->state.mouse_button_pressed == BTN_RIGHT) {
                root->consumed_event = true;
            }
        };
        c->when_clicked = paint {
            if (c->state.mouse_button_pressed == BTN_RIGHT && META_PRESSED) {
                auto cid = *datum<int>(c, "cid");
                titlebar::titlebar_right_click(cid);
                root->consumed_event = true;
            }
        };
        
        *datum<int>(c, "cid") = id; 
        *datum<bool>(c, "snapped") = false; 
        *datum<bool>(c, "previous_hidden_state") = hypriso->is_hidden(id); 
        *datum<long>(c, "hidden_state_change_time") = 0; 

        auto client_info = new ClientInfo;
        c->user_data = client_info;
    }
    
    hypriso->set_corner_rendering_mask_for_window(id, 3);
    
    apply_restore_info(id);
    later_immediate([id](Timer *) {
        fit_on_screen(id);
    });

    titlebar::on_window_open(id);
    alt_tab::on_window_open(id);
    resizing::on_window_open(id);
    second::layout_containers();
    dock::add_window(id);

    if (hypriso->has_decorations(id)) {
        later(50, [id](Timer *) {
            hypriso->set_float_state(id, true);
            //apply_restore_info(id);
        });
    }
}

static void on_window_closed(int id) {
    hypriso->set_corner_rendering_mask_for_window(id, 0);
    clear_snap_groups(id);
    
    titlebar::on_window_closed(id);
    resizing::on_window_closed(id);
    alt_tab::on_window_closed(id);
    dock::remove_window(id);

    {
        auto m = actual_root; 

        for (int i = m->children.size() - 1; i >= 0; i--) {
            auto cid = *datum<int>(m->children[i], "cid");
            if (cid == id) {
                delete m->children[i];
                m->children.erase(m->children.begin() + i);
            }
        } 
    }
    second::layout_containers();
}

static void on_layer_open(int id) {    
    //return;
    auto m = actual_root; 
    auto c = m->child(FILL_SPACE, FILL_SPACE);
    c->custom_type = (int) TYPE::LAYER;
    c->when_paint = [](Container *actual_root, Container *c) {
        return;
        auto root = get_rendering_root();
        if (!root)
            return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);

        if (stage == (int)STAGE::RENDER_POST_WINDOW) {
            renderfix 
            rect(c->real_bounds, {1, 0, 1, 1});
        }
    };
    *datum<int>(c, "cid") = id;
    log(fz("open layer {}", id));
}

static void on_layer_closed(int id) {    
    auto m = actual_root; 

    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto cid = *datum<int>(m->children[i], "cid");
        if (cid == id) {
            delete m->children[i];
            m->children.erase(m->children.begin() + i);
        }
    } 

    second::layout_containers();
}

static void on_layer_change() {
    // move snapped windows
    later_immediate([](Timer *) {
        for (auto c : actual_root->children) {
            if (c->custom_type == (int) TYPE::CLIENT) {
                auto snapped = *datum<bool>(c, "snapped");
                auto snap_type = *datum<int>(c, "snap_type");
                if (snapped) {
                    int cid = *datum<int>(c, "cid");
                    auto p = snap_position_to_bounds(get_monitor(cid), (SnapPosition) snap_type);
                    float scalar = hypriso->has_decorations(cid); // if it has a titlebar
                    hypriso->move_resize(cid, p.x, p.y + titlebar_h * scalar, p.w, p.h - titlebar_h * scalar, false);
                    hypriso->should_round(cid, false);
                }
            }
        }        
    });

}

static void test_container(Container *m) {
    auto c = m->child(100, 100);
    c->custom_type = (int) TYPE::TEST;
    c->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
        c->scroll_v_real += scroll_y; 
    };
    c->when_paint = [](Container *root, Container *c) {
        auto b = c->real_bounds;
        c->real_bounds.y += c->scroll_v_real;
        if (c->state.mouse_pressing) {
            rect(c->real_bounds, {1, 1, 1, 1});
        } else if (c->state.mouse_hovering) {
            rect(c->real_bounds, {1, 0, 0, 1});
        } else {
            rect(c->real_bounds, {1, 0, 1, 1});
        }
        auto info = gen_text_texture(mylar_font, fz("{} {}", c->real_bounds.x, c->real_bounds.y), 20, {1, 1, 1, 1});
        draw_texture(info, c->real_bounds.x, c->real_bounds.y);
        free_text_texture(info.id);
        c->real_bounds = b;
    };
    c->pre_layout = [](Container *root, Container *c, const Bounds &bounds) {
        auto [rid, s, stage, active_id] = from_root(root);
        //nz(fz("{} {}", root->real_bounds.x * s, root->real_bounds.y));
        c->real_bounds = Bounds(20, 100, 100, 100);
        //hypriso->damage_entire(rid);
    };
    c->when_mouse_motion = request_damage;
    c->when_mouse_down = paint {
        consume_event(root, c);
        //request_damage(root, c);
    };
    c->when_mouse_up = paint {
        consume_event(root, c);
        //request_damage(root, c);
    };
    c->when_clicked = request_damage;
    c->when_mouse_leaves_container = request_damage;
    c->when_mouse_enters_container = request_damage;
}

static void on_monitor_open(int id) {
    auto c = new Container();
    //c->when_paint = paint_debug;
    actual_monitors.push_back(c);
    auto cid = datum<int>(c, "cid");
    *cid = id;
    second::layout_containers();
    dock::start(hypriso->monitor_name(id));
}

static void on_monitor_closed(int id) {
    for (int i = actual_monitors.size() - 1; i >= 0; i--) {
        auto cid = *datum<int>(actual_monitors[i], "cid");
        if (cid == id) {
            actual_monitors.erase(actual_monitors.begin() + i);
        }
    }
    second::layout_containers();
    dock::stop(hypriso->monitor_name(id));
}

static void on_activated(int id) {
    titlebar::on_activated(id);
    alt_tab::on_activated(id);
    dock::on_activated(id);
    snap_assist::close();
    overview::close();
}

static void on_draw_decos(std::string name, int monitor, int id, float a) {
    titlebar::on_draw_decos(name, monitor, id, a);
}

void draw_text(std::string text, int x, int y) {
    //return;
    float size = 20;
    TextureInfo first_info;
    {
        first_info = gen_text_texture("Monospace", text, size, {0, 0, 0, 1});
        rect(Bounds(x, y, (double) first_info.w, (double) first_info.h), {1, 0, 1, 1});
        draw_texture(first_info, x + 3, y + 4);
        free_text_texture(first_info.id);
    }
    {
        auto info = gen_text_texture("Monospace", text, size, {1, 1, 1, 1});
        draw_texture(info, x, y);
        free_text_texture(info.id);
    }
    
}

// HyprIso is asking us if the window is snapped
static bool is_snapped(int id) {
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::CLIENT && *datum<int>(c, "cid") == id && *datum<bool>(c, "snapped")) {
            return true;
        }
    }
    return false;
}

static void on_render(int id, int stage) {
    if (stage == (int) STAGE::RENDER_BEGIN) {
        second::layout_containers();
        for (auto c : actual_root->children) {
            if (c->custom_type == (int) TYPE::CLIENT) {
                auto cid = *datum<int>(c, "cid");
//                if (hypriso->is_mapped(cid) && !hypriso->is_hidden(cid))
//                    hypriso->screenshot_deco(cid);
            }
        }
 
    }

    int current_monitor = current_rendering_monitor();
    int current_window = current_rendering_window();
    int active_id = current_window == -1 ? current_monitor : current_window;

    for (auto r : actual_monitors) {
        auto cid = *datum<int>(r, "cid");
        //hypriso->damage_entire(cid);
        if (cid == current_monitor) {
            *datum<int>(actual_root, "stage") = stage;
            *datum<int>(actual_root, "active_id") = active_id;
            paint_outline(actual_root, actual_root);
        }
    }
    
    if (stage == (int) STAGE::RENDER_LAST_MOMENT) {
        for (auto c : actual_root->children) {
            if (c->custom_type == (int)TYPE::CLIENT) {
                // TODO: should be interleaved where window WAS, not LAST_MOMENT
                auto cid = *datum<int>(c, "cid");
                auto mon_id = get_monitor(cid);
                if (mon_id == current_monitor) {
                    auto time = datum<long>(c, "hidden_state_change_time");
                    auto previous = datum<bool>(c, "previous_hidden_state");
                    auto current = hypriso->is_hidden(cid);
                    auto current_time = get_current_time_in_ms();
                    if (*previous != current) {
                        *previous = current; 
                        *time = current_time;
                    }
                    // draw minimizing animation
                    long delta = current_time - *time;
                    if (delta < minimize_anim_time) { 
                        float scalar = ((float) delta) / ((float) minimize_anim_time);

                        auto bounds = dock::get_location(hypriso->monitor_name(mon_id), cid);
                        auto monitor_b = bounds_monitor(mon_id);
                        bounds.y = monitor_b.h;
                        bounds.scale(scale(mon_id));

                        hypriso->draw_raw_min_thumbnail(cid, bounds, scalar);
                        for (int i = actual_monitors.size() - 1; i >= 0; i--) {
                            auto cid = *datum<int>(actual_monitors[i], "cid");
                            hypriso->damage_entire(cid);
                        }
                    }
                }
            }
        }
    }
}

static void on_drag_start_requested(int id) {
    drag::begin(id);
}

static void on_resize_start_requested(int id, RESIZE_TYPE type) {
    auto cid = id;
    bool snapped = false;
    int snap_type = (int) SnapPosition::NONE;
    if (auto container = get_cid_container(cid)) {
        if (*datum<bool>(container, "snapped")) {
            snapped = true;
            snap_type = *datum<int>(container, "snap_type");
        }
    }

    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;
    int resize_type = (int) RESIZE_TYPE::NONE;
    if (type == RESIZE_TYPE::LEFT || type == RESIZE_TYPE::BOTTOM_LEFT || type == RESIZE_TYPE::TOP_LEFT)
        left = true;
    if (type == RESIZE_TYPE::RIGHT || type == RESIZE_TYPE::BOTTOM_RIGHT || type == RESIZE_TYPE::TOP_RIGHT)
        right = true;
    if (type == RESIZE_TYPE::TOP || type == RESIZE_TYPE::TOP_RIGHT || type == RESIZE_TYPE::TOP_LEFT)
        top = true;
    if (type == RESIZE_TYPE::BOTTOM || type == RESIZE_TYPE::BOTTOM_RIGHT || type == RESIZE_TYPE::BOTTOM_LEFT)
        bottom = true;

    if (snapped) {
        switch (snap_type) {
            case (int) SnapPosition::TOP_LEFT: {
                left = false;
                top = false;
                break;
            }
            case (int) SnapPosition::BOTTOM_LEFT: {
                left = false;
                bottom = false;
                break;
            }
            case (int) SnapPosition::TOP_RIGHT: {
                right = false;
                top = false;
                break;
            }
            case (int) SnapPosition::BOTTOM_RIGHT: {
                right = false;
                bottom = false;
                break;
            }
            case (int) SnapPosition::LEFT: {
                left = false;
                top = false;
                bottom = false;
                break;
            }
            case (int) SnapPosition::RIGHT: {
                right = false;
                top = false;
                bottom = false;
                break;
            }
        }
    }
    
    if (top && left) {
        resize_type = (int) RESIZE_TYPE::TOP_LEFT;
    } else if (top && right) {
        resize_type = (int) RESIZE_TYPE::TOP_RIGHT;
    } else if (bottom && left) {
        resize_type = (int) RESIZE_TYPE::BOTTOM_LEFT;
    } else if (bottom && right) {
        resize_type = (int) RESIZE_TYPE::BOTTOM_RIGHT;
    } else if (top) {
        resize_type = (int) RESIZE_TYPE::TOP;
    } else if (right) {
        resize_type = (int) RESIZE_TYPE::RIGHT;
    } else if (bottom) {
        resize_type = (int) RESIZE_TYPE::BOTTOM;
    } else if (left) {
        resize_type = (int) RESIZE_TYPE::LEFT;
    }

    if (resize_type != (int) RESIZE_TYPE::NONE) {
        resizing::begin(id, resize_type);
    }
}

static void on_drag_or_resize_cancel_requested() {
    if (drag::dragging()) {
        drag::end(drag::drag_window());
    }
    if (resizing::resizing()) {
        resizing::end(resizing::resizing_window());
    }
}


static void on_config_reload() {
    hypriso->set_zoom_factor(zoom_factor);
    hypriso->add_float_rule();
    hypriso->overwrite_defaults();
    dock::redraw();
}

Bounds fixed_box(float startx, float starty, float endx, float endy) {
    auto x = startx;
    auto y = starty;
    auto xn = endx;
    auto yn = endy;
    if (x > xn) {
        auto t = xn;
        xn = x;
        x = t;
    }
    if (y > yn) {
        auto t = yn;
        yn = y;
        y = t;
    }
    auto w = xn - x;
    auto h = yn - y;
    auto b = Bounds(x, y, w, h);
    return b; 
}

static void create_actual_root() {
    *datum<long>(actual_root, "drag_end_time") = 0;
    *datum<bool>(actual_root, "dragging") = false;

    actual_root->when_drag_end_is_click = false;
    actual_root->when_clicked = paint {
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            create_root_popup();
        }
    };
    actual_root->when_drag_start = paint {
        *datum<bool>(c, "dragging") = true;
        for (auto m : actual_monitors)
            hypriso->damage_entire(*datum<int>(m, "cid"));
    };
    actual_root->when_drag = [](Container *actual_root, Container *c) {
        actual_root->consumed_event = true;
        auto b = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);
        static Bounds previousB = b;
        b.grow(20);
        hypriso->damage_box(b);
        hypriso->damage_box(previousB);
        previousB = b;
    };
    actual_root->when_drag_end = paint {
        *datum<bool>(c, "dragging") = false;
        for (auto m : actual_monitors)
            hypriso->damage_entire(*datum<int>(m, "cid"));
    };
    actual_root->when_paint = [](Container* actual_root, Container* c) {
        auto root = get_rendering_root();
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        auto dragging = *datum<bool>(c, "dragging");
        if (stage == (int)STAGE::RENDER_POST_WALLPAPER && dragging && c->state.mouse_button_pressed == BTN_LEFT) {
            auto b = fixed_box(actual_root->mouse_initial_x, actual_root->mouse_initial_y, actual_root->mouse_current_x, actual_root->mouse_current_y);

            auto mb = bounds_monitor(rid);
            b.x -= mb.x;
            b.y -= mb.y;
            b.scale(s);
            b.round();
            auto col = color_sel_color();
            float rounding = 9.0f;
            render_drop_shadow(rid, 1.0, {0, 0, 0, .18}, std::round(rounding * s), 2.0, b);
            rect(b, RGBA(col.r, col.g, col.b, .50f), 0, std::round(rounding * s), 2.0f, true, 0.1);
            col = color_sel_border_color();
            border(b, RGBA(col.r, col.g, col.b, .8f), std::round(1.0f * s), 0, std::round(rounding * s), 2.0f, false, 1.0);
        }
    };
}

void load_restore_infos() {
    restore_infos.clear();

    // Resolve $HOME
    const char* home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }

    // Target path
    std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/restore.txt";

    std::ifstream in(filepath);
    if (!in) {
        // No file — silently return
        return;
    }

    std::string line;
    bool first_line = true;
    int file_version = 0; // original unversioned file
    while (std::getline(in, line)) {
        defer(first_line = false);
        std::istringstream iss(line);
        std::string class_name;
        Bounds info;
        bool keep_above = false;
        bool fake_fullscreen = false;
        bool remove_titlebar = false;
        bool remember_size = false;
        bool remember_workspace = false;
        int remembered_workspace = -1;

        if (first_line && !line.empty()) {
            if (line.starts_with("#version 1"))
                file_version = 1;
            if (line.starts_with("#version 2"))
                file_version = 2;
            if (line.starts_with("#version 3"))
                file_version = 3;
            if (file_version != 0)
                continue;
        }

        if (file_version == 0) {
            // Parse strictly: skip if the line is malformed
            if (!(iss >> class_name >> info.x >> info.y >> info.w >> info.h))
                continue; // bad line — skip
        } 
        if (file_version == 1 || file_version == 2) { 
            if (!(iss >> class_name >> info.x >> info.y >> info.w >> info.h >> keep_above >> fake_fullscreen >> remove_titlebar >> remember_size >> remember_workspace))
                continue; // bad line — skip
            if (file_version == 1) {
                keep_above = false;
                fake_fullscreen = false;
                remove_titlebar = false;
                remember_size = false;
                remember_workspace = false;
            }
        }
        if (file_version == 3) {
            if (!(iss >> class_name >> info.x >> info.y >> info.w >> info.h >> keep_above >> fake_fullscreen >> remove_titlebar >> remember_size >> remember_workspace >> remembered_workspace))
                continue; // bad line — skip
        }

        WindowRestoreLocation restore;
        restore.box = info;
        restore.keep_above = keep_above;
        restore.fake_fullscreen = fake_fullscreen;
        restore.remove_titlebar = remove_titlebar;
        restore.remember_size = remember_size;
        restore.remember_workspace = remember_workspace;
        restore.remembered_workspace = remembered_workspace;
        restore_infos[class_name] = restore;
    }
}

void save_restore_infos() {
    // Resolve $HOME
    const char* home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }

    // Target path
    std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/restore.txt";

    // Ensure parent directories exist
    std::filesystem::create_directories(filepath.parent_path());

    // Write file (overwrite mode)
    std::ofstream out(filepath, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to write file: " + filepath.string());
    }
    out << "#version 3" << "\n";
    for (auto [class_name, info] : restore_infos) {
        // class_name std::string
        // info.box.x info.box.y info.box.w info.box.h
        out << class_name << " " << info.box.x << " " << info.box.y << " " << info.box.w << " " << info.box.h << " " << info.keep_above << " " << info.fake_fullscreen << " " << info.remove_titlebar << " " << info.remember_size << " " << info.remember_workspace << " " << info.remembered_workspace << "\n";
    }
    //out << contents;
    if (!out.good()) {
        throw std::runtime_error("Error occurred while writing: " + filepath.string());
    }
}

void update_restore_info_for(int id) {
    WindowRestoreLocation info;
    int mid = get_monitor(id);
    if (mid != -1) {
        Bounds cb = bounds_client(id);
        Bounds cm = bounds_monitor(mid);
        auto s = scale(mid);
        info.box = {
            cb.x / cm.w,
            cb.y / cm.h,
            cb.w / cm.w,
            (cb.h + titlebar_h) / cm.h,
        };
        auto old = restore_infos[hypriso->class_name(id)];
        info.remember_workspace = old.remember_workspace;
        info.remember_size = old.remember_size;
        info.remove_titlebar = old.remove_titlebar;
        info.remembered_workspace = hypriso->get_workspace(id);
        restore_infos[hypriso->class_name(id)] = info;
        save_restore_infos(); // I believe it's okay to call this here because it only happens on resize end, and drag end
    }
}

void do_snap(SnapPosition pos) {
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::CLIENT) {
            auto cid = *datum<int>(c, "cid");
            if (hypriso->has_focus(cid)) {
                auto snapped = *datum<bool>(c, "snapped");
                auto snap_type = *datum<int>(c, "snap_type");
                drag::snap_window(get_monitor(cid), cid, (int) pos);
                return;
            }
        }
    }    
} 

void add_hyprctl_dispatchers() {
    hypriso->add_hyprctl_dispatcher("plugin:mylar:dock_start", [](std::string in) {
        dock::start();
        return true;
    });
    hypriso->add_hyprctl_dispatcher("plugin:mylar:dock_stop", [](std::string in) {
        dock::stop();
        return true;
    });
    hypriso->add_hyprctl_dispatcher("plugin:mylar:toggle_layout", [](std::string in) {
        toggle_layout();
        return true;
    });
    hypriso->add_hyprctl_dispatcher("plugin:mylar:snap_left", [](std::string in) {
        do_snap(SnapPosition::LEFT);
        return true;
    });
    hypriso->add_hyprctl_dispatcher("plugin:mylar:snap_right", [](std::string in) {
        do_snap(SnapPosition::RIGHT);
        return true;
    });
    hypriso->add_hyprctl_dispatcher("plugin:mylar:snap_up", [](std::string in) {
        do_snap(SnapPosition::MAX);
        return true;
    });
    hypriso->add_hyprctl_dispatcher("plugin:mylar:snap_down", [](std::string in) {
        do_snap(SnapPosition::NONE);
        return true;
    });
    hypriso->add_hyprctl_dispatcher("plugin:mylar:init", [](std::string in) {
        hypriso->login_animation();
        return true;
    });
    hypriso->add_hyprctl_dispatcher("plugin:mylar:toggle_dock_merge", [](std::string in) {
        dock::toggle_dock_merge();
        return true;
    });

    hypriso->add_hyprctl_dispatcher("plugin:mylar:right_click_active", [](std::string in) {
        for (auto m : actual_root->children) {
            if (m->custom_type == (int) TYPE::CLIENT) {
                auto cid = *datum<int>(m, "cid");
                if (hypriso->has_focus(cid)) {
                    titlebar::titlebar_right_click(cid, true);
                    return true;
                }
            }
        }
        return true;
    });
}

void on_requests_max_or_min(int cid, int wants) {
    if (wants == 1) {
        hypriso->set_hidden(cid, true);
        bool just_saw_cid = false;
        for (auto c : actual_root->children) {
            if (c->custom_type == (int) TYPE::CLIENT) {
                auto od = *datum<int>(c, "cid");
                if (just_saw_cid) {
                    hypriso->bring_to_front(od, true);
                    break;
                }
                if (od == cid) 
                    just_saw_cid = true;
            }
        }
        return;
    }
    if (auto c = get_cid_container(cid)) {
        auto snapped = *datum<bool>(c, "snapped");
        if (snapped) {
            drag::snap_window(get_monitor(cid) , cid, (int) SnapPosition::NONE);
        } else {
            drag::snap_window(get_monitor(cid) , cid, (int) SnapPosition::MAX);
        }
    }
}

void on_title_change(int cid) {
    dock::title_change(cid, hypriso->title_name(cid));
}

void on_workspace_change(int cid) {
    snap_assist::close();
    overview::close();
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::OUR_POPUP) {
            popup::close(c->uuid);
        }
    }
}

void second::begin() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    hypriso->create_config_variables();

    on_any_container_close = any_container_closed;
    create_actual_root();
    add_hyprctl_dispatchers();
            
    hypriso->on_mouse_press = on_mouse_press;
    hypriso->on_mouse_move = on_mouse_move;
    hypriso->on_key_press = on_key_press;
    hypriso->on_scrolled = on_scrolled;
    hypriso->on_draw_decos = on_draw_decos;
    hypriso->on_render = on_render;
    hypriso->is_snapped = is_snapped;
    hypriso->on_window_open = on_window_open;
    hypriso->on_window_closed = on_window_closed;
    hypriso->on_title_change = on_title_change;
    hypriso->on_layer_open = on_layer_open;
    hypriso->on_layer_closed = on_layer_closed;
    hypriso->on_layer_change = on_layer_change;
    hypriso->on_monitor_open = on_monitor_open;
    hypriso->on_monitor_closed = on_monitor_closed;
    hypriso->on_drag_start_requested = on_drag_start_requested;
    hypriso->on_resize_start_requested = on_resize_start_requested;
    hypriso->on_drag_or_resize_cancel_requested = on_drag_or_resize_cancel_requested;
    hypriso->on_config_reload = on_config_reload;
    hypriso->on_activated = on_activated;
    hypriso->on_requests_max_or_min = on_requests_max_or_min;
    hypriso->on_workspace_change = on_workspace_change;

    load_restore_infos();

	hypriso->create_callbacks();
	hypriso->create_hooks();
	
    hypriso->add_float_rule();

    if (icon_cache_needs_update()) {
        std::thread th([] {
            icon_cache_generate();
            icon_cache_load();
        });
        th.detach();
    } else {
        icon_cache_load();
    }

    //dock::start();
    /*std::thread t([] {
        start_dock();
    });
    t.detach();*/
}

void second::end() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    dock::stop();
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::CLIENT) {
            if (*datum<bool>(c, "snapped")) {
                auto cid = *datum<int>(c, "cid");
                //drag::snap_window(get_monitor(cid), cid, (int) SnapPosition::NONE);
                auto b = *datum<Bounds>(c, "pre_snap_bounds");
                hypriso->move_resize(cid, b);
            }
        }
    }
    save_restore_infos();
    hypriso->end();    
    settings::stop();
    //stop_dock();

//#ifdef TRACY_ENABLE
    //tracy::ShutdownProfiler();
//#endif
}

void second::layout_containers() {
    if (actual_monitors.empty())
        return;

    { // TODO: go through monitors and clac top left x top left y and br x y and set the actual_root real_bounds
        int tlx = -1;
        int tly = -1;
        int brx = -1;
        int bry = -1;
        for (int i = 0; i < actual_monitors.size(); i++) {
            auto mid = *datum<int>(actual_monitors[i], "cid");
            auto b = bounds_monitor(mid);
            if ((i == 0) || b.x < tlx)
                tlx = b.x;
            if ((i == 0) || b.y < tly)
                tly = b.y;
            if ((i == 0) || b.y + b.h > bry)
                bry = b.y + b.h;
            if ((i == 0) || b.x + b.w > brx)
                brx = b.x + b.w;
        }
        auto w = brx - tlx;
        auto h = bry - tly;
        actual_root->wanted_bounds = Bounds(tlx, tly, w, h);
        actual_root->real_bounds = actual_root->wanted_bounds;
    }
    
    std::vector<Container *> backup;
    {
        auto r = actual_root;
        for (int i = r->children.size() - 1; i >= 0; i--) {
            auto c = r->children[i];
            if (c->custom_type != (int) TYPE::CLIENT) {
                backup.push_back(c);
                r->children.erase(r->children.begin() + i);
            }
        }
    }

    // reorder based on stacking
    std::vector<int> order = get_window_stacking_order();
    {
        auto r = actual_root;
        // update the index based on the stacking order
        for (auto c : r->children) {
            auto sort_index = datum<int>(c, "sort_index");
            auto cid = *datum<int>(c, "cid");
            for (int i = 0; i < order.size(); i++)
               if (order[i] == cid)
                    *sort_index = i;
        }
        // sort the children based on index
        std::sort(r->children.begin(), r->children.end(), [](Container *a, Container *b) {
            auto adata_index = *datum<int>(a, "sort_index"); 
            auto bdata_index = *datum<int>(b, "sort_index"); 
            return adata_index > bdata_index; 
        });
    }
    
    for (auto r : actual_monitors) {
        auto rid = *datum<int>(r, "cid");
        r->real_bounds = bounds_monitor(rid); 
    }

    for (auto c : actual_root->children) {
        auto cid = *datum<int>(c, "cid");
        set_exists(c, hypriso->is_mapped(cid) && !hypriso->is_hidden(cid));
        
        if (c->exists) {
            auto b = bounds_client(cid);

            auto fo = hypriso->floating_offset(cid);
            auto so = hypriso->workspace_offset(cid);
            if (hypriso->has_decorations(cid)) {
                //auto boxxy = hypriso->getTexBox(cid);
                //boxxy.scale();
                //boxxy.round();
                //auto b = boxxy;
                //c->real_bounds = Bounds(b.x, b.y - titlebar_h, b.w, b.h + titlebar_h);

                c->real_bounds = Bounds(b.x + fo.x + so.x, b.y - titlebar_h + fo.y + so.y, b.w, b.h + titlebar_h);
            } else {
                //auto b = boxxy;
                c->real_bounds = Bounds(b.x + fo.x + so.x, b.y + fo.y + so.y, b.w, b.h);
                
            }
            ::layout(actual_root, c, c->real_bounds);
        }
    }

    for (auto c : backup) {
        *datum<bool>(c, "touched") = false;
    }

    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::ALT_TAB) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
                *datum<bool>(c, "touched") = true;
            }
        }
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
                *datum<bool>(c, "touched") = true;
            }
        }
        if (c->custom_type == (int) TYPE::CLIENT_RESIZE) {
            auto id = *datum<int>(c, "cid");
            set_exists(c, hypriso->is_mapped(id) && !hypriso->is_hidden(id) && hypriso->resizable(id));
            set_exists(c, (c->exists && (hypriso->get_workspace(id) == hypriso->get_active_workspace(hypriso->monitor_from_cursor())))); 
            
            for (int i = actual_root->children.size() - 1; i >= 0; i--) {
                auto child = actual_root->children[i];
                auto cid = *datum<int>(child, "cid");
                if (child->custom_type == (int) TYPE::CLIENT && cid == id) {
                    c->real_bounds = child->real_bounds;
                    //actual_root->children.insert(actual_root->children.begin() + i + 1, c);
                    actual_root->children.insert(actual_root->children.begin() + i, c);
                    *datum<bool>(c, "touched") = true;
                    if (c->pre_layout) {
                        c->pre_layout(actual_root, c, actual_root->real_bounds);
                    }
                }
            }
        }
        if (c->custom_type == (int) TYPE::TEST) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }
        
        if (c->custom_type == (int) TYPE::OUR_POPUP) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }

        if (c->custom_type == (int) TYPE::SNAP_HELPER) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }

        if (c->custom_type == (int) TYPE::LAYER) {
            // WOOPS background layer is a layer
            //log("TODO: layer needs to be positioned based on above or below, and level in that stack");
            c->parent->children.insert(c->parent->children.begin(), c);
            auto id = *datum<int>(c, "cid");
            auto b = bounds_layer(id);
            c->real_bounds = b;

            *datum<bool>(c, "touched") = true;
        }
    }

    snap_assist::fix_order();
    
    for (auto c : backup) {
        if (!(*datum<bool>(c, "touched"))) {
            notify("hey you forgot to layout one of the containers in layout_containers probably leading to it not getting drawn");
        }
    }
}

bool double_clicked(Container *c, std::string needle) {
    auto n = needle + "_double_click_check";
    long *data = get_data<long>(c->uuid, n);
    if (!data) {
        data = datum<long>(c, n);
        *data = 0;
    }
    long *activation = get_data<long>(c->uuid, n + "_activation");
    if (!activation) {
        activation = datum<long>(c, n + "_activation");
        *activation = 0;
    }
    
    long current = get_current_time_in_ms();
    long last_time = *data;
    long last_activation = *activation;
    if (current - last_time < 500 && current - last_activation > 600) {
        data = datum<long>(c, n);
        *activation = current;
        return true; 
    }
    *data = current;
    return false;
}

#include <fstream>
#include <string>
#include <mutex>

void log(const std::string& msg) {
    return;
    static bool firstCall = true;
    static std::ofstream ofs;
    static std::mutex writeMutex;
    static long num = 0; 

    std::lock_guard<std::mutex> lock(writeMutex);

    if (firstCall) {
        ofs.open("/tmp/log", std::ios::out | std::ios::trunc);
        firstCall = false;

        // Replace "program" with something that displays a live-updating file.
        // Example choices:
        //   - `xterm -e "tail -f /tmp/log"`
        //   - `gedit /tmp/log`
        //   - `glow /tmp/log`
        //std::thread t([]() {
            //system("alacritty -e tail -f /tmp/log");
        //});
        //t.detach();
    } else if (!ofs.is_open()) {
        // If log is called after close, recover
        ofs.open("/tmp/log", std::ios::out | std::ios::app);
    }

    std::string result = std::format("{:>10}", num++);
    ofs << result << ' ' << msg << '\n';
    ofs.flush(); // force write so GUI viewer always shows latest content
}

Container *get_rendering_root() {
    auto rendering_monitor = current_rendering_monitor();
    for (auto m : actual_monitors) {
        auto rid = *datum<int>(m, "cid");
        m->real_bounds = bounds_monitor(rid);
        if (rid == rendering_monitor)
            return m;
    }

    for (auto m : actual_monitors)
        return m;

    return nullptr;
}

void consume_event(Container *actual_root, Container *c) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    actual_root->consumed_event = true;
    request_damage(actual_root, c);
}

void consume_everything(Container *c) {
    c->when_mouse_down = consume_event;
    c->when_mouse_up = consume_event;
    c->when_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y) {
        consume_event(root, c); 
    };
    c->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
        consume_event(root, c); 
    };
    c->when_clicked = consume_event;
    c->when_mouse_enters_container = consume_event;
    c->when_mouse_leaves_container = consume_event;
    c->when_mouse_motion = consume_event;
    c->on_closed = [](Container *c) {
        consume_event(actual_root, c);
    };
}

void launch_command(std::string command) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (command.empty())
        return;
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "mylar: Could not fork\n");
        return;
    }
    
    if (pid == 0) {
        char *dir = getenv("HOME");
        if (dir) {
            int ret = chdir(dir);
            if (ret != 0) {
                fprintf(stderr, "mylar: failed to chdir to %s\n", dir);
            }
        }
        
        execlp("sh", "sh", "-c", command.c_str(), NULL);
        fprintf(stderr, "mylar: Failed to execute %s\n", command.c_str());
        
        _exit(1);
    } else {
        signal(SIGCHLD, SIG_IGN); // https://www.geeksforgeeks.org/zombie-processes-prevention/
    }
}

void clear_snap_groups(int id) {
    for (auto c : actual_root->children) {
       if (c->custom_type == (int) TYPE::CLIENT) {
           auto cdata = (ClientInfo *) c->user_data;
           auto cid = *datum<int>(c, "cid");
           // clear id client groups
           if (cid == id)
               cdata->grouped_with.clear();
           
           // clear id from other clients so they don't think that they are still grouped with id
           for (int i = cdata->grouped_with.size() - 1; i >= 0; i--) {
               if (cdata->grouped_with[i] == id) {
                   cdata->grouped_with.erase(cdata->grouped_with.begin() + i);
               } 
           }
       }
    }
}

bool groupable_types(SnapPosition a, SnapPosition b) {
    if (a == b)
        return false;
    if (a == SnapPosition::MAX || b == SnapPosition::MAX)
        return false;
    bool a_on_left = a == SnapPosition::LEFT || a == SnapPosition::TOP_LEFT || a == SnapPosition::BOTTOM_LEFT;
    bool b_on_left = b == SnapPosition::LEFT || b == SnapPosition::TOP_LEFT || b == SnapPosition::BOTTOM_LEFT;
    if (a_on_left != b_on_left) {
        return true;
    } else {
        bool a_on_top = a == SnapPosition::TOP_LEFT || a == SnapPosition::TOP_RIGHT;
        bool b_on_top = b == SnapPosition::TOP_LEFT || b == SnapPosition::TOP_RIGHT;
        if (a == SnapPosition::LEFT || b == SnapPosition::RIGHT)
            return false;
        if (a_on_top != b_on_top) {
            return true;
        }
    }

    return false;
}

bool groupable(SnapPosition position, const std::vector<int> ids) {
    std::vector<SnapPosition> positions;
    for (auto id : ids)
        if (auto client = get_cid_container(id))
            positions.push_back((SnapPosition) (*datum<int>(client, "snap_type"))); 

    // have to check in with group represented by b and see if all are mergable friendships
    for (auto p : positions)
        if (!groupable_types(position, p)) 
            return false;

    return true; 
}

void add_to_snap_group(int id, int other, const std::vector<int> &grouped) {
    // go through all current groups of other, and add id to those as well
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::CLIENT) {
            auto cdata = (ClientInfo *) c->user_data;
            bool part_of_group = false;
            auto cid = *datum<int>(c, "cid");
            for (auto g : grouped)
                if (g == cid)        
                    part_of_group = true;
            if (cid == other || part_of_group) {
                cdata->grouped_with.push_back(id);
            }
            if (cid == id) {
                cdata->grouped_with.push_back(other);
                for (auto g : grouped) {
                    cdata->grouped_with.push_back(g);
                }
            }
        }
    }
}

void damage_all() {
    for (auto m : actual_monitors)
        hypriso->damage_entire(*datum<int>(m, "cid")); 
}
