/**
 * 
 * Event pumper and distributor
 * 
 */

#include "systray.h"
#include "heart.h"

#include "container.h"
#include "show_desktop.h"
#include "desktop_gesture.h"
#include "simple_dbus.h"
#include "hypriso.h"
#include "shortcuts.h"
#include "titlebar.h"
#include "events.h"
#include "icons.h"
#include "hotcorners.h"
#include "alt_tab.h"
#include "drag.h"
#include "resizing.h"
#include "dock/dock.h"
#include "snap_preview.h"
#include "popup.h"
#include "quick_shortcut_menu.h"
#include "settings.h"
#include "first.h"
#include "snap_assist.h"
#include "overview.h"
#include "wallpaper_layout.h"
#include "workspace_indicator.h"
#include "drag_workspace_switcher.h"
#include "audio.h"
#include "screenshot.h"
#include "desktop_icons.h"
#include "polling_thread.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <ctime>
#include <dbus/dbus-shared.h>
#include <gio/gio.h>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <pulse/proplist.h>
#include <sys/wait.h>
#include <wayland-server-protocol.h>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <linux/input-event-codes.h>
#include <thread>

static bool META_PRESSED = false;
static bool SHIFT_PRESSED = false;
static bool CTRL_PRESSED = false;
static float zoom_factor = 1.0;
static long zoom_nicely_ended_time = 0;
static bool zoom_needs_speed_update = true;
static bool force_meta_open = false;
static bool IS_META_PRESSED() {
    return META_PRESSED || force_meta_open;
}

static float initial_fade_in_scalar = 0.0;

static bool mouse_down = false;
//static bool and_on_desktop = false;

std::unordered_map<std::string, WindowRestoreLocation> restore_infos;

std::unordered_map<std::string, Datas> datas;

std::vector<Container *> actual_monitors; // actually just root of all
Container *actual_root = new Container;

void draw_text(std::string text, int x, int y);
void apply_restore_info(int id);

static void any_container_closed(Container *c) {
    remove_data(c->uuid); 
}

static void set_exists(Container *c, bool state) {
    if (c->exists || state)
        c->exists = state; 
}

static bool on_mouse_move(int id, float x, float y) {
    static int previous = -1;
    auto current = hypriso->monitor_from_cursor();
    if (previous != current) {
        previous = current;
        damage_all();
    }
    
    heart::layout_containers();
    auto mou = mouse();
    x = mou.x;
    y = mou.y;
    snap_preview::on_mouse_move(x, y); 
    drag_workspace_switcher::on_mouse_move(x, y); 

    if (drag::dragging()) {
        drag::motion(drag::drag_window());
        return true;
    }
    if (resizing::resizing()) {
        resizing::motion(resizing::resizing_window());
        return true;
    }
    
    //notify(fz("{} {}", x, y));
    {
        auto m = actual_root;
        Event event(x, y);
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

static bool on_mouse_press(int id, int button, int state, float x, float y) {
    mouse_down = state;
    heart::layout_containers();
    
    auto mou = mouse();
    x = mou.x;
    y = mou.y;
    
    const bool any_closed = popup::dismiss_outside(x, y);
    auto pierced = pierced_containers(actual_root, x, y);
    bool any_desktop_icons_pierced = false;
    if (!pierced.empty())
        any_desktop_icons_pierced = pierced[0]->custom_type == (int) TYPE::DESKTOP_ICON;
    if (!any_desktop_icons_pierced && state) {
    	//desktop_icons::deselect();
    }
    
    if (any_closed)
        damage_all();

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
        Event event(x, y, button, state);
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
    heart::layout_containers();
    {
        auto m = actual_root;
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
    const bool draw_zoom_allowed = !screenshot_tool::is_drawing() || SHIFT_PRESSED;
    if (IS_META_PRESSED() && time_since > 1000 && draw_zoom_allowed) {
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

static Bounds restored_window_bounds(int id, const WindowRestoreLocation& info) {
    const auto monitor = get_monitor(id);
    const auto s = scale(monitor);
    const auto reserved = bounds_reserved_monitor(monitor);
    auto b = bounds_client(id);
    b.w = std::min(reserved.w * info.box.w, std::max(1.0, reserved.w - 60 * s));
    b.h = std::min(reserved.h * info.box.h, std::max(1.0, reserved.h - 60 * s));
    b.x = reserved.x + (reserved.w - b.w) * .5;
    b.y = reserved.y + (reserved.h - b.h) * .5;
    if (reserved.h * info.box.h >= reserved.h - 60 * s)
        b.y += titlebar_h * s * .5;
    return b;
}

static void apply_window_tiling(int id, bool state, bool restore_position) {
    if (!hypriso->alt_tabbable(id) || hypriso->is_floating(id) == !state)
        return;

    auto c = get_cid_container(id);
    if (state) {
        if (c)
            *datum<Bounds>(c, "pre_mode_change_position") = bounds_client(id);
        hypriso->set_float_state(id, false);
        hypriso->should_round(id, true);
        return;
    }

    hypriso->set_float_state(id, true);
    if (c && *datum<bool>(c, "snapped"))
        hypriso->should_round(id, false);
    const auto previous = c ? *datum<Bounds>(c, "pre_mode_change_position") : Bounds{};
    if (restore_position) {
        if (previous.w <= 0 || previous.h <= 0)
            return;
        const auto now = bounds_client(id);
        hypriso->move_resize(id, now, true);
        hypriso->move_resize(id, previous, false);
        return;
    }

    const auto name = hypriso->class_name(id);
    const auto saved = restore_infos.find(name);
    const auto parent = hypriso->parent(id);
    if (saved == restore_infos.end() || (parent != -1 && hypriso->class_name(parent) == name))
        return;
    auto size = restored_window_bounds(id, saved->second);
    if (size.w <= 5 || size.h <= 5)
        return;

    // Restore size around the destination position, never the old workspace or
    // monitor coordinates. Clamp only when the destination cannot fit the size.
    const auto monitor = get_monitor(id);
    const auto reserved = bounds_reserved_monitor(monitor);
    if (monitor == -1 || reserved.w <= 0 || reserved.h <= 0)
        return;
    const auto extents = extents_client(id);
    const auto now = bounds_client_final(id);
    size.w = std::min(size.w, std::max(1.0, reserved.w - extents.left - extents.right));
    size.h = std::min(size.h, std::max(1.0, reserved.h - extents.top - extents.bottom));
    size.x = std::clamp(now.x + (now.w - size.w) * .5, reserved.x + extents.left,
                        std::max(reserved.x + extents.left, reserved.x + reserved.w - size.w - extents.right));
    size.y = std::clamp(now.y + (now.h - size.h) * .5, reserved.y + extents.top,
                        std::max(reserved.y + extents.top, reserved.y + reserved.h - size.h - extents.bottom));
    hypriso->move_resize(id, size, false);
}

static void apply_workspace_tiling(int s, bool state) {
    for (auto id : get_window_stacking_order()) {
        if (hypriso->get_active_workspace_id_client(id) != s)
            continue;
        apply_window_tiling(id, state, true);
    }
}

void save_restore_infos();

// Windows whose restore rules have run. Until then a workspace change is
// Hyprland placing the new window, not the user moving it.
static std::unordered_set<int> restore_rules_applied;

static void on_window_workspace_change(int id) {
    const int space = hypriso->get_client_workspace_id(id);
    if (space != -1)
        apply_window_tiling(id, hypriso->is_space_tiling_id(space), false);

    // "Remember workspace" used to save the workspace only on drag/resize end
    // or when a titlebar option was toggled, so moving a window with a
    // shortcut, the overview or the workspace switcher restored a stale one.
    if (!restore_rules_applied.contains(id))
        return;
    const auto name = hypriso->class_name(id);
    auto saved = restore_infos.find(name);
    if (saved == restore_infos.end() || !saved->second.remember_workspace)
        return;
    const int parent = hypriso->parent(id);
    if (parent != -1 && hypriso->class_name(parent) == name)
        return; // dialogs follow their parent
    // Special and named workspaces have non-positive ids and can't be restored.
    const int raw = hypriso->get_client_workspace(id);
    if (raw <= 0 || raw == saved->second.remembered_workspace)
        return;
    saved->second.remembered_workspace = raw;
    save_restore_infos();
}

void toggle_layout() {
    const auto space = hypriso->get_active_workspace_id(hypriso->monitor_from_cursor());
    hypriso->set_space_tiling_id(space, !hypriso->is_space_tiling_id(space));
}

static bool on_key_press(int id, int key, int state, bool update_mods) {
    // The Shortcuts settings page is recording a key combo.
    if (shortcuts::on_key_press(key, state))
        return true;

    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT)
        SHIFT_PRESSED = state;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL)
        CTRL_PRESSED = state;

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

    if (key == KEY_ESC && state == 0 && !screenshot_tool::is_open()) {
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

    // Only this key's handlers decide whether it is consumed. A flag left over from an
    // earlier key (e.g. the screenshot tool consuming the Esc that closed it) would
    // otherwise swallow every following key, SUPER_L included, until the mouse moved.
    auto root = actual_root;
    root->consumed_event = false;
    key_press(actual_root, key, state, key, key, false, std::to_string(key));

    if (root->consumed_event) {
        root->consumed_event = false;
        return true;
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
        return {x + w * limits.middle_middle, y, w - (w * limits.middle_middle) + 1, h};
    } else if (pos == SnapPosition::TOP_LEFT) {
        return {x, y, w * limits.middle_middle, h * limits.left_middle};
    } else if (pos == SnapPosition::TOP_RIGHT) {
        return {x + w * limits.middle_middle, y, w - w * limits.middle_middle + 1, h * limits.right_middle};
    } else if (pos == SnapPosition::BOTTOM_LEFT) {
        return {x, y + h * limits.left_middle, w * limits.middle_middle, h - h * limits.left_middle};
    } else if (pos == SnapPosition::BOTTOM_RIGHT) {
        return {x + w * limits.middle_middle, y + h * limits.right_middle, w - w * limits.middle_middle, h - h * limits.right_middle};
    }

    return out;
}

SnapPosition mouse_to_snap_position(int mon, int x, int y) {
    if (!set->resize_at_screen_edges)
        return SnapPosition::NONE;
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
        if (hypriso->is_floating(cid) && *datum<bool>(c, "snapped") && (*datum<int>(c, "snap_type") != (int)SnapPosition::MAX)) {
            auto b = c->real_bounds;
            b.shrink(1);
            auto a = *datum<float>(c, "titlebar_alpha");
            border(b, {.5, .5, .5, (float) (.8 * a)}, 1);
        }
        //rect({root->real_bounds.x, hypriso->pass_info(cid).cby, root->real_bounds.w, 1}, {1, 0, 0, 1});
    }
}

void fit_on_screen(int cid)  {
    if (!hypriso->is_floating(cid))
        return;
    if (auto c = get_cid_container(cid); c && *datum<bool>(c, "snapped"))
        return;
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
            
            if (info.remember_workspace)
                hypriso->move_to_workspace(id, info.remembered_workspace);

            if (!hypriso->is_floating(id))
                continue;
            auto c = get_cid_container(id);
            // Opening a decorated window can apply restore rules twice.
            if (c && *datum<bool>(c, "snapped"))
                continue;
            const bool restore_maximized = info.remember_maximized && info.remembered_snap_type == (int) SnapPosition::MAX && hypriso->can_maximize(id);
            const bool restore_size = info.remember_size && info.box.w > 0 && info.box.h > 0;
            const Bounds restored = restore_size ? restored_window_bounds(id, info) : Bounds();
            // A window that gets maximized only needs the remembered size for unmaximizing. Resizing it
            // first sends two configures right after mapping, and Firefox can drop the second one's repaint.
            if (restore_size && !restore_maximized)
                hypriso->move_resize(id, restored.x, restored.y, restored.w, restored.h);
            if (restore_maximized) {
                drag::snap_window(get_monitor(id), id, info.remembered_snap_type);
                if (restore_size && c && *datum<bool>(c, "snapped")) {
                    *datum<Bounds>(c, "pre_snap_bounds") = restored;
                    update_restore_info_for(id, true);
                }
            }
            return;
        }
    }
}

static void possibly_update_workspace_previews() {
    for (auto c : actual_root->children)
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            later_immediate([](Timer *) {
                auto mon = hypriso->monitor_from_cursor();
                auto spaces = hypriso->get_workspace_ids(mon); 
                for (auto s : spaces)
                    hypriso->screenshot_space(mon, s);
            });
            break;
        }
}

static void on_window_open(int id) {    
    possibly_update_workspace_previews();
    // We make the client on the first monitor we fine, because we move the container later based on actual monitor location
    {
        auto m = actual_root; 
        auto c = m->child(FILL_SPACE, FILL_SPACE);
        c->custom_type = (int) TYPE::CLIENT;
        c->when_paint = paint_snap_preview;
        c->handles_pierced = [](Container* c, int x, int y) {
            auto cid = *datum<int>(c, "cid");
            bool inside = bounds_contains(c->real_bounds, x, y);
            bool on_workspace = hypriso->is_window_workspace_visible(cid);
            return inside && on_workspace;
        };
        c->when_mouse_down = paint {
            if (IS_META_PRESSED() && c->state.mouse_button_pressed == BTN_RIGHT) {
                root->consumed_event = true;
            }
        };
        c->when_clicked = paint {
            if (c->state.mouse_button_pressed == BTN_RIGHT && IS_META_PRESSED()) {
                auto cid = *datum<int>(c, "cid");
                titlebar::titlebar_right_click(cid);
                root->consumed_event = true;
            }
        };
        
        *datum<int>(c, "cid") = id; 
        *datum<bool>(c, "snapped") = false; 
        *datum<bool>(c, "previous_hidden_state") = hypriso->is_hidden(id); 
        *datum<long>(c, "hidden_state_change_time") = 0; 
        *datum<bool>(c, "previous_slept_state") = is_slept(id); 
        *datum<long>(c, "slept_state_change_time") = 0; 

        auto client_info = new ClientInfo;
        c->user_data = client_info;
    }
    
    hypriso->set_corner_rendering_mask_for_window(id, 3);
    
    later_immediate([id](Timer *) {
        apply_restore_info(id);
        restore_rules_applied.insert(id);
        // Restore rules can move a newly opened window to another workspace.
        on_window_workspace_change(id);
        fit_on_screen(id);
    });
    later(100, [id](Timer *) {
        //hypriso->save_position_info(id);
    });

    titlebar::on_window_open(id);
    alt_tab::on_window_open(id);
    resizing::on_window_open(id);
    overview::on_window_open(id);
    heart::layout_containers();
    dock::add_window(id);

    if (hypriso->has_decorations(id)) {
        later(50, [id](Timer *) {
            apply_restore_info(id);
        });
    }
}

static void on_window_closed(int id) {
    restore_rules_applied.erase(id);
    possibly_update_workspace_previews();
    
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
    heart::layout_containers();
}

static void on_layer_open(int id) {    
    //return;
    auto m = actual_root; 
    auto c = m->child(FILL_SPACE, FILL_SPACE);
    c->custom_type = (int) TYPE::LAYER;
    *datum<bool>(c, "notification_layer") = is_notification_layer(id);
    c->when_paint = [](Container *actual_root, Container *c) {
        if (!*datum<bool>(c, "notification_layer") || c->real_bounds.w <= 0 || c->real_bounds.h <= 0)
            return;
        auto root = get_rendering_root();
        if (!root)
            return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS)
            return;

        renderfix
        const auto radius = std::min(std::round(dock::popup_corner_radius * s),
                                     std::floor(std::min(c->real_bounds.w, c->real_bounds.h) * .5));
        render_drop_shadow(rid, 1.0, {0, 0, 0, .0108f}, radius, 2.0, c->real_bounds, std::round(30.0f * s));
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

    heart::layout_containers();
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

struct RememberedWindowPosition {
    int cid = -1;
    Bounds old_reserved; // reserved area of the monitor the window was on
    Bounds outer; // window box including titlebar
    bool snapped = false;
    bool has_pre_snap = false;
    Bounds pre_snap_outer;
};

static std::vector<RememberedWindowPosition> remembered_window_positions;
static unsigned remembered_window_positions_generation = 0;

static Bounds client_to_outer(int cid, Bounds b) {
    if (hypriso->has_decorations(cid)) {
        b.y -= titlebar_h;
        b.h += titlebar_h;
    }
    return b;
}

static Bounds outer_to_client(int cid, Bounds b) {
    if (hypriso->has_decorations(cid)) {
        b.y += titlebar_h;
        b.h -= titlebar_h;
    }
    return b;
}

// Keeps the window's size (clamped to fit), and keeps the free space on each side in the same proportion
static Bounds remap_floating(Bounds b, Bounds from, Bounds to) {
    auto axis = [](double pos, double size, double from_pos, double from_size, double to_pos, double to_size, double &out_pos, double &out_size) {
        out_size = std::min(size, to_size);
        double free_from = from_size - size;
        double t = free_from > 0 ? (pos - from_pos) / free_from : .5;
        t = std::clamp(t, 0.0, 1.0);
        out_pos = std::round(to_pos + t * (to_size - out_size));
    };
    Bounds out;
    axis(b.x, b.w, from.x, from.w, to.x, to.w, out.x, out.w);
    axis(b.y, b.h, from.y, from.h, to.y, to.h, out.y, out.h);
    return out;
}

// Snapped windows keep the same fractions of the reserved area (so custom split ratios survive)
static Bounds remap_snapped(Bounds b, Bounds from, Bounds to) {
    double l = (b.x - from.x) / from.w;
    double t = (b.y - from.y) / from.h;
    double r = (b.x + b.w - from.x) / from.w;
    double bo = (b.y + b.h - from.y) / from.h;
    double nl = std::round(to.x + l * to.w);
    double nt = std::round(to.y + t * to.h);
    double nr = std::round(to.x + r * to.w);
    double nb = std::round(to.y + bo * to.h);
    return {nl, nt, nr - nl, nb - nt};
}

void heart::remember_window_positions() {
    remembered_window_positions.clear();
    for (auto c : actual_root->children) {
        if (c->custom_type != (int) TYPE::CLIENT)
            continue;
        int cid = *datum<int>(c, "cid");
        if (!hypriso->is_floating(cid) || hypriso->is_fullscreen(cid))
            continue;
        int mon = get_monitor(cid);
        if (mon == -1)
            continue;
        RememberedWindowPosition r;
        r.cid = cid;
        r.old_reserved = bounds_reserved_monitor(mon);
        if (r.old_reserved.w <= 0 || r.old_reserved.h <= 0)
            continue;
        r.outer = client_to_outer(cid, bounds_client_final(cid));
        r.snapped = *datum<bool>(c, "snapped");
        if (r.snapped) {
            auto pre = *datum<Bounds>(c, "pre_snap_bounds");
            if (pre.w > 0 && pre.h > 0) {
                r.has_pre_snap = true;
                r.pre_snap_outer = client_to_outer(cid, pre);
            }
        }
        remembered_window_positions.push_back(r);
    }

    // Monitor reloads happen asynchronously and can emit several layout changes; keep the snapshot
    // around for a bit, re-applying it is idempotent since it's always computed from the old state.
    auto generation = ++remembered_window_positions_generation;
    later(2000, [generation](Timer *) {
        if (generation == remembered_window_positions_generation)
            remembered_window_positions.clear();
    });
}

// Keeps Xft.dpi in the X resource database in line with the display scale, so
// Xwayland apps (which aren't scaled when force_zero_scaling is on) size their text right.
static int xrdb_applied_dpi = 0;
static bool xrdb_update_pending = false;

static void update_xrdb_dpi() {
    if (xrdb_update_pending)
        return;
    xrdb_update_pending = true;
    // Coalesce bursts of monitor events, and wait for Xwayland to set DISPLAY.
    later(300, [](Timer *t) {
        static int display_attempts = 0;
        const char *display = std::getenv("DISPLAY");
        if (!display || !*display) {
            // Without Xwayland DISPLAY never appears; give up after ~2 minutes.
            // The next monitor change starts a fresh round.
            t->keep_running = ++display_attempts < 120;
            t->delay = 1000;
            if (!t->keep_running) {
                display_attempts = 0;
                xrdb_update_pending = false;
            }
            return;
        }
        display_attempts = 0;
        t->keep_running = false;
        xrdb_update_pending = false;
        float max_scale = 0;
        for (auto m : actual_monitors)
            max_scale = std::max(max_scale, scale(*datum<int>(m, "cid")));
        if (max_scale <= 0)
            return;
        int dpi = (int) std::round(96.0f * max_scale);
        if (dpi == xrdb_applied_dpi)
            return;
        xrdb_applied_dpi = dpi;
        launch_command(fz("echo 'Xft.dpi: {}' | xrdb -merge", dpi));
    });
}

static void publish_wall_monitors();
struct WallSlot;
static WallSlot *wall_slot(Container *m, bool first);
static void wall_slot_load_initial(Container *c, int id);
static void wall_slot_clear(WallSlot *slot);

static void on_monitor_layout_change() {
    update_xrdb_dpi();
    publish_wall_monitors();
    if (remembered_window_positions.empty())
        return;
    later_immediate([](Timer *) {
        for (auto &r : remembered_window_positions) {
            auto c = get_cid_container(r.cid);
            if (!c)
                continue;
            if (!hypriso->is_floating(r.cid) || hypriso->is_fullscreen(r.cid))
                continue;
            int mon = get_monitor(r.cid);
            if (mon == -1)
                continue;
            Bounds now = bounds_reserved_monitor(mon);
            if (now.w <= 0 || now.h <= 0)
                continue;

            if (r.has_pre_snap)
                *datum<Bounds>(c, "pre_snap_bounds") = outer_to_client(r.cid, remap_floating(r.pre_snap_outer, r.old_reserved, now));

            Bounds target = r.snapped ? remap_snapped(r.outer, r.old_reserved, now) : remap_floating(r.outer, r.old_reserved, now);
            target = outer_to_client(r.cid, target);
            if (target.w < 1 || target.h < 1)
                continue;
            Bounds current = bounds_client_final(r.cid);
            if (current.x == target.x && current.y == target.y && current.w == target.w && current.h == target.h)
                continue;
            hypriso->move_resize(r.cid, target.x, target.y, target.w, target.h, false);
            if (r.snapped)
                hypriso->should_round(r.cid, false);
        }
        heart::layout_containers();
        damage_all();
    });
}

static void test_container(Container *m) {
    auto c = m->child(100, 100);
    c->custom_type = (int) TYPE::TEST;
    c->when_fine_scrolled = [](Container* root, Container* c, double scroll_x, double scroll_y, bool came_from_touchpad) {
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
        auto info = gen_text_texture(set->font, fz("{} {}", c->real_bounds.x, c->real_bounds.y), 20, {1, 1, 1, 1});
        draw_texture(info, c->real_bounds.x, c->real_bounds.y);
        free_text_texture(info.id);
        c->real_bounds = b;
    };
    c->pre_layout = [](Container *root, Container *c, const Bounds &bounds) {
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

static void create_magic_workspace_exit(int monitor) {
    auto bubble = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    bubble->custom_type = (int) TYPE::MAGIC_WORKSPACE_EXIT;
    *datum<int>(bubble, "monitor") = monitor;
    bubble->pre_layout = [](Container *, Container *c, const Bounds &) {
        const auto monitor = *datum<int>(c, "monitor");
        const auto bounds = bounds_monitor(monitor);
        constexpr double width = 258.0;
        constexpr double height = 44.0;
        constexpr double margin = 18.0;
        c->real_bounds = Bounds(bounds.right() - width - margin, bounds.y + margin, width, height);
        c->exists = hypriso->is_magic_workspace_active(monitor);
    };
    bubble->when_paint = [](Container *root, Container *c) {
        auto rendering_root = get_rendering_root();
        if (!rendering_root)
            return;
        const auto [monitor, s, stage, active_id] = roots_info(root, rendering_root);
        if (monitor != *datum<int>(c, "monitor") || stage != (int) STAGE::RENDER_POST_WINDOWS ||
            !hypriso->is_magic_workspace_active(monitor))
            return;

        renderfix
        rect(c->real_bounds, RGBA(.12, .12, .14, .78), 0, 22 * s, 2.0, true);
        auto label = get_cached_texture(rendering_root, c, "magic_workspace_exit_label", set->font,
                                        "Exit magic workspace", RGBA(.96, .97, 1.0, .98), 14);
        if (label->id != -1)
            draw_texture(*label, c->real_bounds.x + 16 * s, center_y(c, label->h));
    };
    bubble->interactable = true;
    bubble->when_mouse_down = consume_event;
    bubble->when_mouse_up = consume_event;
    bubble->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        consume_event(root, c);
        hypriso->exit_magic_workspace(*datum<int>(c, "monitor"));
        damage_all();
    };

    auto close = bubble->child(::absolute, 38, 38);
    close->pre_layout = [](Container *, Container *c, const Bounds &) {
        const auto bounds = c->parent->real_bounds;
        c->real_bounds = Bounds(bounds.right() - 41.0, bounds.y + 3.0, 38.0, 38.0);
    };
    close->when_paint = [](Container *root, Container *c) {
        auto rendering_root = get_rendering_root();
        if (!rendering_root)
            return;
        const auto [monitor, s, stage, active_id] = roots_info(root, rendering_root);
        if (monitor != *datum<int>(c->parent, "monitor") || stage != (int) STAGE::RENDER_POST_WINDOWS ||
            !hypriso->is_magic_workspace_active(monitor))
            return;

        renderfix
        const bool hovered = c->state.mouse_hovering || c->state.mouse_pressing;
        rect(c->real_bounds, hovered ? RGBA(.34, .36, .40, .95) : RGBA(.22, .23, .26, .80), 0, 19 * s, 2.0, true);
        auto icon = get_cached_texture(rendering_root, c, "magic_workspace_exit_icon", icon_font,
                                       "\uE8BB", RGBA(1.0, 1.0, 1.0, .98), 13);
        if (icon->id != -1)
            draw_texture(*icon, center_x(c, icon->w), center_y(c, icon->h));
    };
    close->when_mouse_down = consume_event;
    close->when_mouse_up = consume_event;
    close->when_mouse_motion = request_damage;
    close->when_mouse_enters_container = request_damage;
    close->when_mouse_leaves_container = request_damage;
    close->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        consume_event(root, c);
        hypriso->exit_magic_workspace(*datum<int>(c->parent, "monitor"));
        damage_all();
    };
}

static void on_monitor_open(int id) {
    auto c = new Container();
    //c->when_paint = paint_debug;
    actual_monitors.push_back(c);
    auto cid = datum<int>(c, "cid");
    *cid = id;
    create_magic_workspace_exit(id);
    heart::layout_containers();
    if (set->show_docks)
        dock::start(hypriso->monitor_name(id));
    update_xrdb_dpi();
    *datum<long>(c, "bg_wall_change_time_ms") = 0;
    *datum<bool>(c, "bg_wall1_active") = true;
    publish_wall_monitors();
    wall_slot_load_initial(c, id);
    overview::on_monitor_open(id);

    later(10, [](Timer *) {
        damage_all();
    });
}

static void on_monitor_closed(int id) {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type == (int) TYPE::MAGIC_WORKSPACE_EXIT && *datum<int>(c, "monitor") == id) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
    for (int i = actual_monitors.size() - 1; i >= 0; i--) {
        auto cid = *datum<int>(actual_monitors[i], "cid");
        if (cid == id) {
            wall_slot_clear(wall_slot(actual_monitors[i], true));
            wall_slot_clear(wall_slot(actual_monitors[i], false));
            
            actual_monitors.erase(actual_monitors.begin() + i);
        }
    }
    publish_wall_monitors();
    heart::layout_containers();
    dock::stop(hypriso->monitor_name(id));
}

static void on_activated(int id) {
    Container *c = get_cid_container(id);
    if (!c) {
        if (id == -1)
            dock::on_activated(id);
        return;
    }
    static bool leave = false;
    if (leave)
        return;
    
    auto info = ((ClientInfo *)c->user_data);
    auto grouped_with = info->grouped_with;
    if (!hypriso->is_floating(id)) {
        grouped_with.clear();
        const auto workspace = hypriso->get_client_workspace_id(id);
        for (auto g : get_window_stacking_order()) {
            if (!hypriso->is_floating(g) && hypriso->get_client_workspace_id(g) == workspace &&
                hypriso->is_mapped(g) && !hypriso->is_hidden(g))
                grouped_with.push_back(g);
        }
    }
    if (!grouped_with.empty()) {
        bool floating = hypriso->is_floating(id);
        later_immediate([grouped_with, id, floating](Timer *) {
            auto targets = grouped_with;
            if (floating) {
                // The snap group may have been broken before this ran (e.g. a client side
                // decoration drag started right after the click that focused id), so only
                // raise windows that are still grouped with it.
                auto c = get_cid_container(id);
                if (!c)
                    return;
                targets = ((ClientInfo *)c->user_data)->grouped_with;
            }
            leave = true;
            for (auto g : targets) {
                hypriso->bring_to_front(g, false);
            }
            // Keep the activated window above its group. Snapped windows don't
            // overlap on the desktop, but they do while overview animates them home.
            if (get_cid_container(id))
                hypriso->bring_to_front(id, false);
            leave = false;
        });
    }
    update_mute_states_for_pids();
    heart::layout_containers();

    if (show_desktop::is_opened()) {
        hypriso->render_whitelist.emplace_back(*datum<int>(c, "cid"));
        show_desktop::stop_animation();
    }
    titlebar::on_activated(id);
    alt_tab::on_activated(id);
    dock::on_activated(id);
    snap_assist::close();
    //overview::close(false);
}

static void on_draw_decos(std::string name, int monitor, int id, float a, int deco_offset_x, int deco_offset_y) {
    titlebar::on_draw_decos(name, monitor, id, a, deco_offset_x, deco_offset_y);
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

// ---- Wallpaper ----------------------------------------------------------------
//
// Each monitor holds two slots so a new wallpaper can fade in over the old one.
// A slot draws its cached final image (monitor sized, already scaled and
// cropped) when one matches the current settings. Otherwise it draws the source
// image directly, scaled on the GPU, and a new cached image is made on a worker
// thread once the settings have held still for a moment. Cached images are also
// written to ~/.cache/mylar/wallpaper so the next start can skip decoding the
// full sized source.

struct WallSlot {
    TextureInfo source;      // the image file as loaded
    bool source_loading = false;
    bool source_failed = false;
    TextureInfo cache;       // final image for cache_key
    std::string cache_key;
    std::string path;        // image this slot holds
    std::string stamp;       // wallpaper_file_stamp(path) when loaded
    std::string pending_key; // a cached image for this key is scheduled or being made
};

struct WallLoadState {
    uint64_t gen = 0;
    std::string path;
    std::string stamp;
};

static WallSlot *wall_slot(Container *m, bool first) {
    return datum<WallSlot>(m, first ? "wall_slot1" : "wall_slot2");
}

static Container *wall_monitor_container(int id) {
    for (auto m : actual_monitors)
        if (*datum<int>(m, "cid") == id)
            return m;
    return nullptr;
}

static WallMonitor wall_monitor(int id) {
    auto b = bounds_monitor(id);
    WallMonitor m;
    m.name = hypriso->monitor_name(id);
    m.x = b.x, m.y = b.y, m.w = b.w, m.h = b.h;
    m.scale = scale(id);
    return m;
}

static std::vector<WallMonitor> wall_monitors() {
    std::vector<WallMonitor> monitors;
    for (auto m : actual_monitors)
        monitors.push_back(wall_monitor(*datum<int>(m, "cid")));
    return monitors;
}

static void publish_wall_monitors() {
    auto monitors = wall_monitors();
    std::lock_guard<std::mutex> lock(wallpaper_monitors_mutex);
    wallpaper_monitors_snapshot = monitors;
}

static const std::vector<WallMonitorOverride> &wall_overrides() {
    static std::string parsed_text;
    static std::vector<WallMonitorOverride> parsed;
    if (parsed_text != set->wallpaper_monitors) {
        parsed_text = set->wallpaper_monitors;
        parsed = wallpaper_parse_overrides(parsed_text);
    }
    return parsed;
}

static WallResolved wall_resolve(int id, const WallMonitor &m) {
    return wallpaper_resolve(wallpaper_shared_params(*set), set->wallpaper_span, wall_overrides(), wall_monitors(), m);
}

static std::filesystem::path wall_disk_cache_dir() {
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/tmp") / ".cache/mylar/wallpaper";
}

static std::filesystem::path wall_disk_cache_file(const std::string &monitor, const std::string &key) {
    char hash[32];
    snprintf(hash, sizeof(hash), "%016zx", std::hash<std::string>{}(key));
    return wall_disk_cache_dir() / (wallpaper_safe_name(monitor) + "-" + hash + ".png");
}

static void wall_slot_clear(WallSlot *slot) {
    if (slot->source.id != -1)
        free_text_texture(slot->source.id);
    if (slot->cache.id != -1)
        free_text_texture(slot->cache.id);
    *slot = WallSlot();
}

static cairo_surface_t *wall_render_cache_surface(cairo_surface_t *src, const WallResolved &res, const WallMonitor &m) {
    if (!src)
        return nullptr;
    const int ow = std::max(1, (int) std::round(m.w * m.scale));
    const int oh = std::max(1, (int) std::round(m.h * m.scale));
    const double iw = cairo_image_surface_get_width(src);
    const double ih = cairo_image_surface_get_height(src);
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ow, oh);
    if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(out);
        return nullptr;
    }
    auto cr = cairo_create(out);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    for (auto &r : wallpaper_monitor_rects(res, iw, ih, m)) {
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_clip(cr);
        cairo_translate(cr, r.x, r.y);
        cairo_scale(cr, r.w / iw, r.h / ih);
        cairo_set_source_surface(cr, src, 0, 0);
        auto pattern = cairo_get_source(cr);
        cairo_pattern_set_filter(pattern, CAIRO_FILTER_GOOD);
        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(out);
    return out;
}

static cairo_surface_t *wall_copy_surface(cairo_surface_t *src) {
    if (!src || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS)
        return nullptr;
    const int w = cairo_image_surface_get_width(src);
    const int h = cairo_image_surface_get_height(src);
    const auto fmt = cairo_image_surface_get_format(src);
    cairo_surface_t *copy = cairo_image_surface_create(fmt, w, h);
    if (cairo_surface_status(copy) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(copy);
        return nullptr;
    }
    const int src_stride = cairo_image_surface_get_stride(src);
    const int dst_stride = cairo_image_surface_get_stride(copy);
    const unsigned char *s = cairo_image_surface_get_data(src);
    unsigned char *d = cairo_image_surface_get_data(copy);
    const int row_bytes = std::min(src_stride, dst_stride);
    for (int y = 0; y < h; ++y)
        std::memcpy(d + y * dst_stride, s + y * src_stride, row_bytes);
    cairo_surface_mark_dirty(copy);
    return copy;
}

static void wall_write_disk_cache(const std::string &monitor_name, const std::string &key, cairo_surface_t *out) {
    if (!out)
        return;
    // Keep one cached image per monitor on disk.
    std::error_code ec;
    auto dir = wall_disk_cache_dir();
    std::filesystem::create_directories(dir, ec);
    auto file = wall_disk_cache_file(monitor_name, key);
    auto prefix = wallpaper_safe_name(monitor_name) + "-";
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        auto name = entry.path().filename().string();
        if (name.starts_with(prefix) && entry.path() != file)
            std::filesystem::remove(entry.path(), ec);
    }
    auto tmp = file;
    tmp += ".tmp" + std::to_string(std::hash<std::string>{}(key + std::to_string(get_current_time_in_ms())));
    if (cairo_surface_write_to_png(out, tmp.c_str()) == CAIRO_STATUS_SUCCESS)
        std::filesystem::rename(tmp, file, ec);
    else
        std::filesystem::remove(tmp, ec);
}

// Decodes and renders a monitor's wallpaper on a worker thread, then uploads
// the monitor-sized surface on the main thread so setting a wallpaper never
// stalls the compositor.
static void wall_start_load(Container *c, int id, WallMonitor m, WallResolved res, std::string stamp, bool fade) {
    auto load = datum<WallLoadState>(c, "wall_load_state");
    const uint64_t gen = ++load->gen;
    if (stamp.empty()) {
        load->path.clear();
        load->stamp.clear();
        auto wall1_active = datum<bool>(c, "bg_wall1_active");
        if (fade) {
            wall_slot_clear(wall_slot(c, !*wall1_active));
            *wall1_active = !*wall1_active;
            *datum<long>(c, "bg_wall_change_time_ms") = get_current_time_in_ms();
        } else {
            wall_slot_clear(wall_slot(c, *wall1_active));
        }
        damage_all();
        return;
    }

    load->path = res.path;
    load->stamp = stamp;
    auto key = wallpaper_cache_key(res, m, stamp);
    std::thread([id, gen, fade, key, res, m, stamp]() {
        cairo_surface_t *out = nullptr;
        cairo_surface_t *disk_copy = nullptr;
        std::error_code ec;
        auto cached = wall_disk_cache_file(m.name, key);
        if (std::filesystem::exists(cached, ec))
            load_icon_full_path(&out, cached.string());
        if (!out) {
            cairo_surface_t *src = nullptr;
            load_icon_full_path(&src, res.path);
            if (src) {
                out = wall_render_cache_surface(src, res, m);
                cairo_surface_destroy(src);
                if (out)
                    disk_copy = wall_copy_surface(out);
            }
        }
        main_thread([id, gen, fade, key, path = res.path, stamp, out]() {
            auto c = wall_monitor_container(id);
            if (!c) {
                if (out)
                    cairo_surface_destroy(out);
                return;
            }
            auto load = datum<WallLoadState>(c, "wall_load_state");
            if (load->gen != gen) {
                if (out)
                    cairo_surface_destroy(out);
                return;
            }
            load->path.clear();
            load->stamp.clear();
            auto wall1_active = datum<bool>(c, "bg_wall1_active");
            const bool target_first = fade ? !*wall1_active : *wall1_active;
            auto slot = wall_slot(c, target_first);
            wall_slot_clear(slot);
            slot->path = path;
            slot->stamp = stamp;
            if (out) {
                auto info = gen_texture(out);
                cairo_surface_destroy(out);
                if (info.id != -1) {
                    slot->cache = info;
                    slot->cache_key = key;
                } else {
                    slot->source_failed = true;
                }
            } else {
                slot->source_failed = true;
            }
            if (fade) {
                *wall1_active = target_first;
                *datum<long>(c, "bg_wall_change_time_ms") = get_current_time_in_ms();
            }
            damage_all();
        });
        if (disk_copy) {
            wall_write_disk_cache(m.name, key, disk_copy);
            cairo_surface_destroy(disk_copy);
        }
    }).detach();
}

static void wall_slot_load_initial(Container *c, int id) {
    auto m = wall_monitor(id);
    auto res = wall_resolve(id, m);
    wall_start_load(c, id, m, res, wallpaper_file_stamp(res.path), false);
}

// Decodes a slot's full source image off the main thread for live GPU scaling
// (e.g. while dragging the crop preview).
static void wall_load_source(int id, bool first_slot, WallSlot *slot) {
    if (slot->source_loading)
        return;
    slot->source_loading = true;
    std::string path = slot->path;
    std::string stamp = slot->stamp;
    std::thread([id, first_slot, path, stamp]() {
        cairo_surface_t *src = nullptr;
        load_icon_full_path(&src, path);
        main_thread([id, first_slot, path, stamp, src]() {
            auto c = wall_monitor_container(id);
            auto slot = c ? wall_slot(c, first_slot) : nullptr;
            if (!slot || slot->path != path || slot->stamp != stamp) {
                if (src)
                    cairo_surface_destroy(src);
                return;
            }
            slot->source_loading = false;
            if (!src) {
                slot->source_failed = true;
                return;
            }
            auto info = gen_texture(src);
            cairo_surface_destroy(src);
            if (info.id == -1) {
                slot->source_failed = true;
                return;
            }
            if (slot->source.id != -1)
                free_text_texture(slot->source.id);
            slot->source = info;
            damage_all();
        });
    }).detach();
}

// Renders the final image on a worker thread, then swaps it in.
static void wall_make_cache(int id, bool first_slot, std::string key, WallResolved res, WallMonitor m, std::string stamp) {
    std::thread([id, first_slot, key, res, m, stamp]() {
        cairo_surface_t *src = nullptr;
        load_icon_full_path(&src, res.path);
        cairo_surface_t *out = nullptr;
        cairo_surface_t *disk_copy = nullptr;
        if (src) {
            out = wall_render_cache_surface(src, res, m);
            cairo_surface_destroy(src);
            if (out)
                disk_copy = wall_copy_surface(out);
        }
        main_thread([id, first_slot, key, stamp, out]() {
            auto c = wall_monitor_container(id);
            auto slot = c ? wall_slot(c, first_slot) : nullptr;
            if (!slot || slot->stamp != stamp) {
                if (out)
                    cairo_surface_destroy(out);
                return;
            }
            if (slot->pending_key == key)
                slot->pending_key.clear();
            if (!out)
                return;
            auto info = gen_texture(out);
            cairo_surface_destroy(out);
            if (info.id == -1)
                return;
            if (slot->cache.id != -1)
                free_text_texture(slot->cache.id);
            slot->cache = info;
            slot->cache_key = key;
            damage_all();
        });
        if (disk_copy) {
            wall_write_disk_cache(m.name, key, disk_copy);
            cairo_surface_destroy(disk_copy);
        }
    }).detach();
}

// Makes a cached image for key once it has stayed the wanted one for a moment
// (while the crop is being dragged it changes every frame).
static void wall_request_cache(int id, bool first_slot, WallSlot *slot, const std::string &key) {
    if (slot->pending_key == key)
        return;
    slot->pending_key = key;
    later(350, [id, first_slot, key](Timer *) {
        auto c = wall_monitor_container(id);
        auto slot = c ? wall_slot(c, first_slot) : nullptr;
        if (!slot || slot->pending_key != key)
            return;
        auto m = wall_monitor(id);
        auto res = wall_resolve(id, m);
        if (res.path != slot->path || wallpaper_cache_key(res, m, slot->stamp) != key) {
            slot->pending_key.clear(); // the next frame asks again
            damage_all();
            return;
        }
        wall_make_cache(id, first_slot, key, res, m, slot->stamp);
    });
}

static void draw_wall_slot(int id, Container *c, bool first_slot, bool active, double zoom, float alpha) {
    auto slot = wall_slot(c, first_slot);
    auto m = wall_monitor(id);
    auto res = wall_resolve(id, m);
    const double mw = m.w * m.scale, mh = m.h * m.scale;
    auto zoomed = [&](const WallRect &r) {
        const double cx = mw * .5, cy = mh * .5;
        return Bounds(cx + (r.x - cx) * zoom, cy + (r.y - cy) * zoom, r.w * zoom, r.h * zoom);
    };

    // A slot only follows setting changes for its own image; the outgoing one
    // of a fade keeps what it had.
    const bool same_image = res.path == slot->path;
    const auto key = wallpaper_cache_key(res, m, slot->stamp);
    const bool cache_ok = slot->cache.id != -1 && slot->cache_key == key;
    if (active && same_image && !cache_ok && !slot->stamp.empty()) {
        if (slot->source.id == -1 && !slot->source_failed && !slot->source_loading &&
            wallpaper_file_stamp(slot->path) == slot->stamp) {
            wall_load_source(id, first_slot, slot);
        }
        wall_request_cache(id, first_slot, slot, key);
    }

    if (cache_ok || slot->source.id == -1 || !same_image) {
        if (slot->cache.id != -1)
            draw_texture(slot->cache, zoomed({0, 0, mw, mh}), alpha);
        else if (slot->source.id != -1)
            draw_texture(slot->source, zoomed({0, 0, mw, mh}), alpha);
        return;
    }

    if (wallpaper_needs_backdrop(res.params))
        rect(zoomed({0, 0, mw, mh}), RGBA(0, 0, 0, alpha));
    for (auto &r : wallpaper_monitor_rects(res, slot->source.w, slot->source.h, m))
        draw_texture(slot->source, zoomed(r), alpha);
}

// Fades to a monitor's wallpaper when its image changed.
static void wall_refresh_monitor(Container *c) {
    const int id = *datum<int>(c, "cid");
    auto wall1_active = datum<bool>(c, "bg_wall1_active");
    auto active = wall_slot(c, *wall1_active);
    auto m = wall_monitor(id);
    auto res = wall_resolve(id, m);
    auto stamp = wallpaper_file_stamp(res.path);
    auto load = datum<WallLoadState>(c, "wall_load_state");
    if (res.path == active->path && stamp == active->stamp) {
        if (!load->path.empty()) {
            load->gen++;
            load->path.clear();
            load->stamp.clear();
        }
        return;
    }
    if (!load->path.empty() && load->path == res.path && load->stamp == stamp)
        return;
    wall_start_load(c, id, m, res, stamp, true);
}

void wallpaper_settings_changed() {
    publish_wall_monitors();
    for (auto c : actual_monitors)
        wall_refresh_monitor(c);
    damage_all();
    settings::preload_wallpapers();
}

static void paint_wallpaper(int current_monitor, Container *m) {
    auto wall1_active = *datum<bool>(m, "bg_wall1_active");
    auto start = *datum<long>(m, "bg_wall_change_time_ms");
    auto delta = (float) (get_current_time_in_ms() - start);
    // {"anchors":[{"x":0,"y":1},{"x":1,"y":0}],"controls":[{"x":0.08437150831111269,"y":0.011981451924641935}]}
    static std::vector<float> curve = { 0.000, 0.139, 0.228, 0.298, 0.355, 0.404, 0.448, 0.486, 0.521, 0.553, 0.582, 0.609, 0.634, 0.657, 0.678, 0.698, 0.717, 0.735, 0.752, 0.767, 0.782, 0.796, 0.810, 0.822, 0.834, 0.845, 0.856, 0.866, 0.875, 0.884, 0.893, 0.901, 0.909, 0.916, 0.923, 0.929, 0.935, 0.941, 0.946, 0.951, 0.956, 0.961, 0.965, 0.969, 0.972, 0.976, 0.979, 0.982, 0.984, 0.987, 0.989, 0.991, 0.993, 0.994, 0.996, 0.997, 0.998, 0.999, 0.999, 1.000, 1.000 };
    auto scalar = pull(curve, initial_fade_in_scalar);
    const double zoom = 1 + .4 * (1.0 - scalar);
    float trans = 700.0f;

    if (delta < trans) {
        draw_wall_slot(current_monitor, m, !wall1_active, false, zoom, 1.0); // old base
        draw_wall_slot(current_monitor, m, wall1_active, true, zoom, delta / trans); // new fades in
        request_refresh();
    } else {
        draw_wall_slot(current_monitor, m, wall1_active, true, zoom, 1.0);
    }
}

static bool initial_fade_in_done = false;
static bool initial_fade_in_forced = false;
static bool initial_fade_in_cursor_centered = false;
static long initial_fade_in_start_time = -1;

static void reset_initial_fade_in() {
    initial_fade_in_done = false;
    initial_fade_in_forced = true;
    initial_fade_in_cursor_centered = false;
    initial_fade_in_start_time = -1;
    initial_fade_in_scalar = 0.0;
    set_cursor_hidden_for_desktop_fade(true);
    damage_all();
}

static void paint_initial_fade_in() {
    auto& done = initial_fade_in_done;
    if (done)
        return;

    if (!initial_fade_in_forced && hyprland_instance_name() == last_hyprland_instance_name())
        done = true;
    if (done) {
        set_cursor_hidden_for_desktop_fade(false);
        return;
    }

    const int monitor = current_rendering_monitor();
    double dt = 0.0;
    double time = 550.0;

    request_refresh();

    if (!hypriso->session_active()) {
        rect(bounds_monitor(monitor).scale(scale(monitor)), RGBA(0, 0, 0, 1.0));
        return;
    }
    if (hypriso->zoom_progress(current_rendering_monitor()) < 1.F) {
        rect(bounds_monitor(monitor).scale(scale(monitor)), RGBA(0, 0, 0, 1.0));
        return;
    }

    if (initial_fade_in_start_time == -1)
        initial_fade_in_start_time = get_current_time_in_ms();
    dt = ((double) (get_current_time_in_ms() - initial_fade_in_start_time));
    double delay = 2000.0;
    if (dt >= delay && !initial_fade_in_cursor_centered) {
        center_cursor_on_monitor(hypriso->monitor_from_cursor());
        initial_fade_in_cursor_centered = true;
    }
    set_cursor_hidden_for_desktop_fade(dt < delay);
    if (dt < delay) {
        dt = 0.0;
    } else {
        dt -= delay;
    }
    
    rect(bounds_monitor(monitor).scale(scale(monitor)), RGBA(0, 0, 0, 1.0 - (dt / time)));

    done = dt > time;
    initial_fade_in_scalar = dt / time;
    if (done)
        initial_fade_in_scalar = 1.0;
}

static void on_render(int id, int stage) {
    if (stage == (int) STAGE::RENDER_BEGIN) {
        heart::layout_containers();
        drag_workspace_switcher::update_drag();
        for (auto c : actual_root->children) {
            if (c->custom_type == (int) TYPE::CLIENT) {
//                if (hypriso->is_mapped(cid) && !hypriso->is_hidden(cid))
//                    hypriso->screenshot_deco(cid);
            }
        }
 
    }

    int current_monitor = current_rendering_monitor();
    int current_window = current_rendering_window();
    int active_id = current_window == -1 ? current_monitor : current_window;

    if (stage == (int) STAGE::RENDER_POST_WALLPAPER && set->draw_wallpaper) {
        for (auto m : actual_monitors)
            if (*datum<int>(m, "cid") == current_monitor)
                paint_wallpaper(current_monitor, m);
    }

    for (auto r : actual_monitors) {
        auto cid = *datum<int>(r, "cid");
        //hypriso->damage_entire(cid);
        if (cid == current_monitor) {
            *datum<int>(actual_root, "stage") = stage;
            *datum<int>(actual_root, "active_id") = active_id;
            paint_outline(actual_root, actual_root);
        }
    }
    if (stage == (int) STAGE::RENDER_PRE_WINDOWS) {
        show_desktop::render();
    }
    if (stage == (int) STAGE::RENDER_PRE_CURSOR) {
        drag_workspace_switcher::paint_drag(current_monitor, true);
    }
    
    if (stage == (int) STAGE::RENDER_POST_WINDOWS) {
        if (!overview::is_showing()) {
            auto current_time = get_current_time_in_ms();
            for (auto zed : slept_windows) {
                auto time = zed.time_slept;
                // draw minimizing animation
                long delta = current_time - time;

                auto mon_id = zed.monitor_id;

                // A window slept from another workspace shouldn't fly into the
                // dock over the workspace currently being shown.
                if (((double) delta) < minimize_anim_time && hypriso->is_window_workspace_visible(zed.cid)) {
                    float scalar = ((float) delta) / ((float) minimize_anim_time);

                    auto bounds = dock::get_item_location(hypriso->monitor_name(mon_id), "sleep_button");
                    auto monitor_b = bounds_monitor(mon_id);
                    bounds.y = monitor_b.h;
                    bounds.scale(scale(mon_id));

                    hypriso->draw_raw_min_thumbnail(zed.cid, bounds, scalar);
                    damage_all();
                }
            }
        }
        
        for (auto c : actual_root->children) {
            if (c->custom_type == (int)TYPE::CLIENT && !overview::is_showing()) {
                // TODO: should be interleaved where window WAS, not LAST_MOMENT
                auto cid = *datum<int>(c, "cid");
                if (is_slept(cid))
                    continue;
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
                        bounds.w = bounds.h;
                        auto monitor_b = bounds_monitor(mon_id);
                        bounds.y = monitor_b.h;
                        bounds.scale(scale(mon_id));

                        hypriso->draw_raw_min_thumbnail(cid, bounds, scalar);
                        damage_all();
                    }
                }
            }
        }
    }

    if (stage == (int) STAGE::RENDER_LAST_MOMENT) {
        //log(fz("{}", current_monitor));
        //log(fz("{} {}", hypriso->whitelist_on, hypriso->render_whitelist.size()));
        //auto monbg = bounds_monitor(current_monitor);
        //rect(Bounds(monbg.x, monbg.y, 300, 300).scale(scale(current_monitor)), {1, 0, 1, 1}, 0, 15.0f, 2.0, true);
        
        auto snap_edge_animation = *datum<float>(actual_root, "snap_edge_animation");
        if (snap_edge_animation != 0.0 && snap_edge_animation != 1.0) {
           int mon = *datum<int>(actual_root, "snap_edge_animation_mon");
           if (mon == current_monitor) {
               auto seax = *datum<float>(actual_root, "snap_edge_animation_x");
               auto seay = *datum<float>(actual_root, "snap_edge_animation_y");
               auto s = scale(mon);
               hypriso->damage_box(Bounds(seax - 80 * s, seay - 80 * s, 160 * s, 160 * s));

               auto scalar = snap_edge_animation;
               float a = 1.0 - scalar;
               RGBA col = {1.0f, 1.0f, 1.0f, 0.1f};
               draw_colored_circ(seax * s, seay * s, 70 * s  * scalar, {(float) col.r * a, (float) col.g * a, (float) col.b * a, (float) col.a * a}, .4 + .6 * scalar, 1.0);
           }
        }

        paint_initial_fade_in();
    }
}

static void on_drag_start_requested(int id) {
    drag::begin(id);
    // drag::begin breaks the snap group, so the dragged window has to be raised above
    // its former group members (the titlebar drag path does the same).
    if (hypriso->is_floating(id))
        hypriso->bring_to_front(id);
}

static void on_resize_start_requested(int id, RESIZE_TYPE type) {
    if (!hypriso->is_floating(id)) {
        resizing::begin(id, (int) type);
        return;
    }
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
            case (int) SnapPosition::MAX: {
                right = false;
                top = false;
                bottom = false;
                left = false;
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

struct CombinedDesktopGesture {
    std::function<void(bool)> begin;
    std::function<void(Bounds)> update;
    std::function<void()> end;
};

static CombinedDesktopGesture minimize_overview_combined_gesture() {
    static desktop_gesture::State gesture;
    static desktop_gesture::VerticalStroke vertical_stroke;
    static long start = 0;
    static int monitor = -1;
    static bool workspace_gesture_started = false;
    static double horizontal_offset = 0;
    static double horizontal_applied = 0;
    static bool horizontal_active = false;
    static bool gesture_blocked = false;

    const auto activate = []() {
        if (gesture.owner == desktop_gesture::Owner::Overview) {
            gesture.initial_progress = std::clamp(overview::get_openess(), 0.0f, 1.0f);
            overview::begin_gesture(monitor);
        } else if (gesture.owner == desktop_gesture::Owner::ShowDesktop) {
            // Release overview ownership before starting the minimize gesture.
            overview::instant_close();
            gesture.initial_progress = std::clamp(show_desktop::get_scalar(), 0.0f, 1.0f);
            minimize_gesture_count++;
            show_desktop::start();
        }
    };

    // Both starting directions share ownership, progress, and release handling.
    return {[activate](bool horizontal) {
        gesture_blocked = screenshot_tool::is_open() && !screenshot_tool::is_picking_window();
        if (gesture_blocked)
            return;
        // A closing view is returning to normal; let the new swipe choose its view.
        const bool overview_open = overview::is_showing() && !overview::is_closing();
        const bool desktop_open = show_desktop::is_opened() && !show_desktop::is_closing();
        gesture.begin(overview_open, desktop_open, overview::get_openess(), show_desktop::get_scalar());
        start = get_current_time_in_ms();
        vertical_stroke = {};
        monitor = hypriso->monitor_from_cursor();
        workspace_gesture_started = false;
        horizontal_offset = 0;
        horizontal_applied = 0;
        horizontal_active = horizontal && overview_open;
        activate();
        // Hyprland delivers the begin event's movement again to update.
        request_refresh();
    }, [activate](Bounds s) {
        if (gesture_blocked)
            return;
        horizontal_offset += s.x;
        vertical_stroke.update(s.y, get_current_time_in_ms());
        const auto previous_owner = gesture.owner;
        gesture.update(s.y);
        if (gesture.owner != previous_owner)
            activate();

        if (gesture.owner == desktop_gesture::Owner::Overview && overview::is_showing()) {
            overview::overwrite_openess(gesture.progress());
            // Keep sideways noise in the accumulated input, but reveal it gradually
            // only after a deliberate horizontal movement. Once engaged, track freely.
            constexpr double horizontal_dead_zone = 30.0;
            constexpr double horizontal_full_motion = 75.0;
            const double distance = std::abs(horizontal_offset);
            if (distance >= horizontal_full_motion)
                horizontal_active = true;
            const double blend = horizontal_active ? 1.0 : std::clamp(
                (distance - horizontal_dead_zone) / (horizontal_full_motion - horizontal_dead_zone), 0.0, 1.0);
            const double visible_offset = horizontal_offset * blend * blend * (3.0 - 2.0 * blend);
            // Overview's scene is initialized after the opening gesture starts.
            if (!workspace_gesture_started && visible_offset != 0)
                workspace_gesture_started = overview::begin_workspace_gesture(monitor);
            if (workspace_gesture_started) {
                overview::update_workspace_gesture(monitor, visible_offset - horizontal_applied);
                horizontal_applied = visible_offset;
            }
        } else if (gesture.owner == desktop_gesture::Owner::ShowDesktop && show_desktop::is_opened())
            show_desktop::set_scalar(gesture.progress());
        request_refresh();
    }, []() {
        if (gesture_blocked) {
            gesture_blocked = false;
            return;
        }
        const auto end = get_current_time_in_ms();
        if (gesture.owner == desktop_gesture::Owner::Overview) {
            if (workspace_gesture_started)
                overview::end_workspace_gesture(monitor);
            overview::end_gesture(vertical_stroke.start, end, vertical_stroke.displacement());
        } else if (gesture.owner == desktop_gesture::Owner::ShowDesktop && show_desktop::is_opened())
            show_desktop::minimize_animate_out(start, end, gesture.y_offset, show_desktop::get_scalar());
        gesture = {};
        workspace_gesture_started = false;
        request_refresh();
    }};
}

static void on_config_reload() {
    hypriso->set_zoom_factor(zoom_factor);
    hypriso->add_float_rule();
    hypriso->overwrite_defaults();
    dock::redraw();

    static float offset_x = 0;
    static float offset_y = 0;
    static float offset_click = 75;
    static bool overview_gesture = false;
    // TODO: offset_click should scale down so that 1200 offset can rotate all windows list
    // only scale if factor > 1

    // alt tab gesture
    gestures_reset();

    const auto combined = minimize_overview_combined_gesture();
    make_gesture(3, 6, 0, 1.0, false, [combined](Bounds) {
        combined.begin(false);
    }, combined.update, combined.end);

    make_gesture(3, 7, 0, 1.0, false, [combined](Bounds s) {
        offset_x = 0;
        offset_y = 0;
        overview_gesture = overview::is_showing() && !overview::is_closing();
        if (overview_gesture) {
            combined.begin(true);
            return;
        }
        //alt_tab::visual_offset(0);
        //alt_tab::show_reticle(true);
        alt_tab::show();
        //alt_tab::move(1);
        //coverflow::open();
    }, [combined](Bounds s) {
        if (overview_gesture) {
            combined.update(s);
            return;
        }
        //coverflow::scroll(s.x, s.y);
        
        offset_x += s.x;
        offset_y += s.y;

        if (alt_tab::at_start_row())
            if (offset_y < 0)
                offset_y = 0;
        if (alt_tab::at_end_row())
            if (offset_y > 0)
                offset_y = 0;
        if (alt_tab::at_start_col())
            if (offset_x < 0)
                offset_x = 0;
        if (alt_tab::at_end_col())
            if (offset_x > 0)
                offset_x = 0;

        if (offset_x > offset_click) {
            alt_tab::move_x(1);
            offset_x = 0;
        } else if (offset_x < -offset_click) {
            offset_x = 0;
            alt_tab::move_x(-1);
        }
        if (offset_y > offset_click) {
            alt_tab::move_y(1, (offset_x / offset_click) * 250);
            offset_x = 0;
            offset_y = 0;
        } else if (offset_y < -offset_click) {
            offset_y = 0;
            offset_x = 0;
            alt_tab::move_y(-1, (offset_x / offset_click) * 250);
        }
        
        //alt_tab::visual_offset(offset / offset_click);
        
        damage_all();
    }, [combined]() {
        if (overview_gesture) {
            combined.end();
            overview_gesture = false;
            return;
        }
        //coverflow::close();
        
        alt_tab::close(true);
        //alt_tab::show_reticle(false);
        //alt_tab::visual_offset(0);
    });
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
    actual_root->when_drag_end_is_click = false;
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
    if (!std::filesystem::exists(filepath))
        return;

    std::ifstream in(filepath);
    if (!in)
        return;

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
        bool remember_maximized = true;
        int remembered_snap_type = (int) SnapPosition::NONE;
        bool remember_workspace = false;
        int remembered_workspace = -1;

        if (first_line && !line.empty()) {
            if (line.starts_with("#version 1"))
                file_version = 1;
            if (line.starts_with("#version 2"))
                file_version = 2;
            if (line.starts_with("#version 3"))
                file_version = 3;
            if (line.starts_with("#version 4"))
                file_version = 4;
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
        if (file_version == 3 || file_version == 4) {
            if (!(iss >> class_name >> info.x >> info.y >> info.w >> info.h >> keep_above >> fake_fullscreen >> remove_titlebar >> remember_size >> remember_workspace >> remembered_workspace))
                continue; // bad line — skip
        }
        if (file_version == 4) {
            if (!(iss >> remember_maximized >> remembered_snap_type))
                continue;
            if (remembered_snap_type < (int) SnapPosition::NONE || remembered_snap_type > (int) SnapPosition::BOTTOM_LEFT)
                remembered_snap_type = (int) SnapPosition::NONE;
        }

        WindowRestoreLocation restore;
        restore.box = info;
        restore.keep_above = keep_above;
        restore.fake_fullscreen = fake_fullscreen;
        restore.remove_titlebar = remove_titlebar;
        restore.remember_size = remember_size;
        restore.remember_maximized = remember_maximized;
        restore.remembered_snap_type = remembered_snap_type;
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
    std::filesystem::path tmp_filepath = std::filesystem::path(home) / ".config/mylar/restore.txt.tmp";

    // Ensure parent directories exist
    std::filesystem::create_directories(filepath.parent_path());

    // Write to temp file first, then copy over restore.txt when done
    std::ofstream out(tmp_filepath, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to write file: " + tmp_filepath.string());
    }
    out << "#version 4" << "\n";
    for (auto [class_name, info] : restore_infos) {
        // class_name std::string
        // info.box.x info.box.y info.box.w info.box.h
        out << class_name << " " << info.box.x << " " << info.box.y << " " << info.box.w << " " << info.box.h << " " << info.keep_above << " " << info.fake_fullscreen << " " << info.remove_titlebar << " " << info.remember_size << " " << info.remember_workspace << " " << info.remembered_workspace << " " << info.remember_maximized << " " << info.remembered_snap_type << "\n";
    }
    //out << contents;
    if (!out.good()) {
        throw std::runtime_error("Error occurred while writing: " + tmp_filepath.string());
    }
    out.close();

    std::error_code ec;
    std::filesystem::copy_file(tmp_filepath, filepath, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("Failed to copy temp file to: " + filepath.string());
    }
}

void update_restore_info_for(int id, bool use_final_bounds) {
    int mid = get_monitor(id);
    if (mid != -1) {
        auto info = restore_infos[hypriso->class_name(id)];
        // Native tiling resizes must preserve the previous floating box and snap state.
        if (hypriso->is_floating(id)) {
            Bounds cb = use_final_bounds ? bounds_client_final(id) : bounds_client(id);
            const Bounds cm = bounds_monitor(mid);
            auto c = get_cid_container(id);
            const bool snapped = c && *datum<bool>(c, "snapped");
            if (snapped)
                cb = *datum<Bounds>(c, "pre_snap_bounds");
            if (cm.w > 0 && cm.h > 0 && cb.w > 0 && cb.h > 0) {
                info.box = {
                    cb.x / cm.w,
                    cb.y / cm.h,
                    cb.w / cm.w,
                    (cb.h + titlebar_h) / cm.h,
                };
            }
            info.remembered_snap_type = snapped ? *datum<int>(c, "snap_type") : (int) SnapPosition::NONE;
        }
        info.remembered_workspace = hypriso->get_client_workspace(id);
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
                auto pre_snap_bounds = *datum<Bounds>(c, "pre_snap_bounds");
                
                if (pos == SnapPosition::NONE) {
                    if (!snapped)
                        return;
                    pos = SnapPosition::MAX;
                } 
                
                if (snapped) {
                    if (snap_type == (int) SnapPosition::MAX && pos == SnapPosition::MAX) {
                        drag::snap_window(get_monitor(cid), cid, (int) SnapPosition::NONE);
                        *datum<Bounds>(c, "pre_snap_bounds") = pre_snap_bounds;
                    } else {
                        clear_snap_groups(cid);
                        auto p = snap_position_to_bounds(get_monitor(cid), (SnapPosition) pos);
                        *datum<int>(c, "snap_type") = (int) pos;
                        if (hypriso->has_decorations(cid)) {
                            hypriso->move_resize(cid, p.x, p.y + titlebar_h, p.w, p.h - titlebar_h, false);
                        } else {
                            hypriso->move_resize(cid, p.x, p.y, p.w, p.h, false);
                        }
                        
                        drag::merge_client_into_existant_groups(cid, false);
                        update_restore_info_for(cid, true);
                    }
                } else {
                    drag::snap_window(get_monitor(cid), cid, (int) pos);
                }
                return;
            }
        }
    }    
} 

void add_hyprctl_dispatchers() {
    hypriso->add_hyprctl_dispatcher("overview_open_or_show_desktop", [](lua_State *) {
        if (screenshot_tool::is_open() && !screenshot_tool::is_picking_window())
            return 0;
        if (!overview::is_showing())
            overview::open(hypriso->monitor_from_cursor()); 
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("desktop_icons_start", [](lua_State *) {
        desktop_icons::start();
        damage_all();
        return 0; 
    }); 
    hypriso->add_hyprctl_dispatcher("desktop_icons_stop", [](lua_State *) {
        desktop_icons::stop();
        return 0; 
    }); 
    hypriso->add_hyprctl_dispatcher("overview_close_or_hide_desktop", [](lua_State *) {
        if (screenshot_tool::is_open() && !screenshot_tool::is_picking_window())
            return 0;
        if (overview::is_showing()) {
            overview::close(); 
            return 0;
        }
        
        if (!show_desktop::is_opened()) {
            show_desktop::start_animation();
            return 0;
        }

        return 0;
    });

    hypriso->add_hyprctl_dispatcher("dock_start", [](lua_State *) {
        for (auto m : actual_monitors) {
            auto mid = *datum<int>(m, "cid");
            dock::start(hypriso->monitor_name(mid));
        }
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("dock_stop", [](lua_State *) {
        dock::stop();
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("toggle_layout", [](lua_State *) {
        // Toggling reloads the Lua config, which must not happen inside this Lua call.
        later(1, [](Timer *) {
            toggle_layout();
        });
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("screenshot_tool", [](lua_State *) {
        screenshot_tool::open();
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("screenshot_tool_draw", [](lua_State *) {
        screenshot_tool::open(true);
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("snap_left", [](lua_State *) {
        do_snap(SnapPosition::LEFT);
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("snap_right", [](lua_State *) {
        do_snap(SnapPosition::RIGHT);
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("snap_up", [](lua_State *) {
        do_snap(SnapPosition::MAX);
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("snap_down", [](lua_State *) {
        do_snap(SnapPosition::NONE);
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("init", [](lua_State *) {
        reset_initial_fade_in();
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("toggle_dock_merge", [](lua_State *) {
        dock::toggle_dock_merge();
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("toggle_desktop_show", [](lua_State *) {
        if (show_desktop::is_opened()) {
            show_desktop::stop_animation();
        } else {
            show_desktop::start_animation();
        }
        damage_all();
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("desktop_hide", [](lua_State *) {
        show_desktop::stop_animation();
        damage_all();
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("desktop_show", [](lua_State *) {
        show_desktop::start_animation();
        damage_all();
        return 0;
    });
    hypriso->add_hyprctl_dispatcher("applications", [](lua_State *) {
        dock::open_applications();
        return 0;
    });

    hypriso->add_hyprctl_dispatcher("projection", [](lua_State *) {
        dock::open_projection();
        return 0;
    });

    hypriso->add_hyprctl_dispatcher("right_click_activate", [](lua_State *) {
        for (auto m : actual_root->children) {
            if (m->custom_type == (int) TYPE::CLIENT) {
                auto cid = *datum<int>(m, "cid");
                if (hypriso->has_focus(cid)) {
                    titlebar::titlebar_right_click(cid, true);
                    return 0;
                }
            }
        }
        return 0;
    });

    // Toggles the focused window between floating and fullscreen.
    hypriso->add_hyprctl_dispatcher("cycle_fullscreen", [](lua_State *) {
        for (auto m : actual_root->children) {
            if (m->custom_type != (int) TYPE::CLIENT)
                continue;
            auto cid = *datum<int>(m, "cid");
            if (!hypriso->has_focus(cid))
                continue;
            hypriso->set_fullscreen(cid, !hypriso->is_fullscreen(cid));
            // Fullscreen geometry belongs to the layout; only pin the floating box otherwise.
            // Fullscreen bounds would also overwrite the remembered floating size.
            if (!hypriso->is_fullscreen(cid)) {
                auto b = bounds_client(cid);
                hypriso->move_resize(cid, b.x, b.y, b.w, b.h);
                update_restore_info_for(cid);
            }
            // Some clients don't pick up the new size on their own, so force a configure.
            hypriso->force_send_window_size(cid);
            return 0;
        }
        return 0;
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
    damage_all();
}

void on_workspace_change(int cid) {
    dock::update_workspaces();
    snap_assist::close();
    //overview::close();
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::OUR_POPUP) {
            popup::close(c->uuid);
        }
    }
    workspace_indicator::on_change(cid);
}

static std::string rounding_shader = R"(
#ifndef ROUNDING_GLSL
#define ROUNDING_GLSL
// smoothing constant for the edge: more = blurrier, but smoother
#define M_PI               3.1415926535897932384626433832795
#define SMOOTHING_CONSTANT (M_PI / 5.34665792551)

uniform int cornerDisableMask; // new

float distanceWithRounding(vec2 coords, float roundingPower) {
    return pow(pow(coords.x, roundingPower) + pow(coords.y, roundingPower), 1.0 / roundingPower);
}

vec4 rounding(vec4 color, float radius, float roundingPower, vec2 topLeft, vec2 fullSize) {
    vec2 pixCoord = vec2(gl_FragCoord);
    
    vec2 preMirror = pixCoord - (topLeft + fullSize * 0.5);
    
    pixCoord -= topLeft + fullSize * 0.5;
    pixCoord *= vec2(lessThan(pixCoord, vec2(0.0))) * -2.0 + 1.0;
    pixCoord -= fullSize * 0.5 - radius;
    pixCoord += vec2(1.0, 1.0) / fullSize; // center the pix don't make it top-left

// new: skip rounding if bit for this corner is set
    int cornerBit = (preMirror.y < 0.0 ? (preMirror.x < 0.0 ? 1 : 2) : (preMirror.x < 0.0 ? 4 : 8)); 
    if ((cornerDisableMask & cornerBit) != 0) 
        return color;

    if (pixCoord.x + pixCoord.y > radius) {
        float dist = distanceWithRounding(pixCoord, roundingPower);

        if (dist > radius + SMOOTHING_CONSTANT)
            discard;

        float normalized = 1.0 - smoothstep(0.0, 1.0, (dist - radius + SMOOTHING_CONSTANT) / (SMOOTHING_CONSTANT * 2.0));

        color *= normalized;
    }

    return color;
}
#endif
)";

void create_rounding_shader() {
    const char* home = std::getenv("HOME");
    if (!home) {
        notify("$HOME environment variable not set");
        return;
    }

    std::filesystem::path filepath = std::filesystem::path(home) / ".config/hypr/shaders/corner_rounding.glsl";
    if (!std::filesystem::exists(filepath)) {
        std::filesystem::create_directories(filepath.parent_path());
        
        std::ofstream out(filepath, std::ios::trunc);
        out << rounding_shader << std::endl;
        later_immediate([](Timer *) {
            hypriso->reload();
        });
    }
}

void create_default_config() {
    later_immediate([](Timer *) {
        hypriso->reload();
    });
}

void on_config_generated() {
    settings::load_save_settings(true, set);
}

void on_audio_change() {
    dock::change_in_audio();
}

static constexpr float dock_popup_shadow_size = 16.0f;

static void damage_popup_shadow(const Bounds &bounds) {
    if (bounds.w <= 0 || bounds.h <= 0)
        return;
    auto damage = bounds;
    damage.grow(dock_popup_shadow_size + 2);
    hypriso->damage_box(damage);
}

static void remove_popup_containers(int id) {
    auto m = actual_root; 
    if (!m)
        return;

    for (int i = m->children.size() - 1; i >= 0; i--) {
        if (m->children[i]->custom_type == (int) TYPE::POPUP) {
            auto cid = *datum<int>(m->children[i], "cid");
            if (cid == id) {
                if (*datum<bool>(m->children[i], "dock_popup"))
                    damage_popup_shadow(m->children[i]->real_bounds);
                delete m->children[i];
                m->children.erase(m->children.begin() + i);
            }
        }
    }
}

void on_popup_closed(int id) {
    // Hyprland keeps drawing a dock popup as a fading snapshot after it's gone;
    // keep its shadow until that fade finishes so the two disappear together.
    if (!actual_root)
        return;
    bool fading_dock_popup = false;
    for (auto ch : actual_root->children)
        if (ch->custom_type == (int) TYPE::POPUP && *datum<int>(ch, "cid") == id && *datum<bool>(ch, "dock_popup"))
            fading_dock_popup = true;
    if (fading_dock_popup && alpha_popup(id) > 0.0f) {
        later(16, [id](Timer *t) {
            t->keep_running = alpha_popup(id) > 0.0f;
            if (!t->keep_running)
                remove_popup_containers(id);
        });
        return;
    }
    remove_popup_containers(id);
}

void on_popup_open(int id, int parent_id, bool owner_is_window) {
    {
        auto m = actual_root; 
        auto c = m->child(FILL_SPACE, FILL_SPACE);
        c->custom_type = (int) TYPE::POPUP;
        *datum<bool>(c, "dock_popup") = !owner_is_window && is_dock_layer(parent_id);
        c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto bounds = bounds_popup(*datum<int>(c, "cid"));
            auto previous = c->real_bounds;
            // Unmapped but still fading out: the snapshot stays where the popup was.
            if (*datum<bool>(c, "dock_popup") && (bounds.w <= 0 || bounds.h <= 0) && alpha_popup(*datum<int>(c, "cid")) > 0.0f)
                bounds = previous;
            if (*datum<bool>(c, "dock_popup") &&
                (bounds.x != previous.x || bounds.y != previous.y || bounds.w != previous.w || bounds.h != previous.h)) {
                damage_popup_shadow(previous);
                damage_popup_shadow(bounds);
            }
            c->real_bounds = bounds;
            if (*datum<bool>(c, "dock_popup")) {
                // The shadow follows Hyprland's popup fade; keep its area repainting until it settles.
                auto alpha = alpha_popup(*datum<int>(c, "cid"));
                auto last = datum<float>(c, "shadow_alpha");
                if (alpha != *last)
                    damage_popup_shadow(bounds);
                *last = alpha;
            }
        };
        c->when_paint = [](Container *actual_root, Container *c) {
            if (!*datum<bool>(c, "dock_popup") || c->real_bounds.w <= 0 || c->real_bounds.h <= 0)
                return;
            auto root = get_rendering_root();
            if (!root)
                return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            if (stage != (int) STAGE::RENDER_POST_WINDOWS)
                return;

            // Layer popups are drawn after this stage. Match their outer Cairo silhouette.
            renderfix
            const auto radius = std::min(std::round(dock::popup_corner_radius * s),
                                         std::floor(std::min(c->real_bounds.w, c->real_bounds.h) * .5));
            const auto alpha = alpha_popup(*datum<int>(c, "cid"));
            if (alpha <= 0.0f)
                return;
            render_drop_shadow(rid, 1.0, {0, 0, 0, .0108f * alpha}, radius, 2.0, c->real_bounds, std::round(dock_popup_shadow_size * s));
        };
        *datum<int>(c, "cid") = id; 
        *datum<float>(c, "shadow_alpha") = 0.0f;
        *datum<int>(c, "parent_cid") = parent_id; 
        *datum<bool>(c, "owner_is_window") = owner_is_window; 
    }
}

static int latest_file_watch = -1;

void watch_wallpaper_change() {
    const char* home = std::getenv("HOME");
    std::filesystem::path filepath =
        std::filesystem::path(home) / ".config/mylar/wall.png";
    static Timer *t = nullptr;
    
    latest_file_watch = watch_file(filepath.string(), [](FileWatchUpdate update, int fd) {
        if (update == FileWatchUpdate::REMOVED) {
            later(500, [](Timer *) {
                watch_wallpaper_change();
            update_xrdb_dpi();
            });
        }
        if (!t) {
            t = later(200, [](Timer *) {
                wallpaper_settings_changed();
                t = nullptr;
            });
        }
    });
}

void heart::begin() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    set_cursor_hidden_for_desktop_fade(!initial_fade_in_done);
    load_restore_infos();
            
    later(100, [](Timer *) {
        poll_thread->start();
        
        hypriso->create_config_variables();
        audio_state_change_callback(on_audio_change);

        later(1000, [](Timer*) {
            on_config_reload();
        });
        later(200, [](Timer *t) {
            static unsigned int attempts = 0;
            ++attempts;
            const bool ready = dbus_startup_ready();
            t->keep_running = !ready && attempts < 20;
            if (!ready && t->keep_running)
                return;

            later(200, [](Timer *) {
                dbus_start(DBUS_BUS_SYSTEM);
                dbus_start(DBUS_BUS_SESSION);
                systray::start();
            });
        });
        
        later(200, [](Timer *) {
            audio_start();
        });
 
        later_immediate([](Timer*) {
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
            hypriso->on_x11_hints_changed = titlebar::on_x11_hints_changed;
            
            hypriso->on_window_closed = on_window_closed;
            hypriso->on_popup_open = on_popup_open;
            hypriso->on_popup_closed = on_popup_closed;
            hypriso->on_title_change = on_title_change;
            hypriso->on_layer_open = on_layer_open;
            hypriso->on_layer_closed = on_layer_closed;
            hypriso->on_layer_change = on_layer_change;
            hypriso->on_monitor_layout_change = on_monitor_layout_change;
            hypriso->on_monitor_open = on_monitor_open;
            hypriso->on_monitor_closed = on_monitor_closed;
            hypriso->on_drag_start_requested = on_drag_start_requested;
            hypriso->on_resize_start_requested = on_resize_start_requested;
            hypriso->on_resize_ended = [](int id) { update_restore_info_for(id, true); };
            hypriso->on_tiled_drag_started = [](int id) {
                drag_workspace_switcher::open();
                drag_workspace_switcher::begin_drag(id);
            };
            hypriso->on_tiled_drag_motion = []() {
                heart::layout_containers();
                const auto m = mouse();
                drag_workspace_switcher::on_mouse_move(m.x, m.y);
            };
            hypriso->on_tiled_drag_ended = [](int id, bool dropped) {
                if (dropped)
                    drag_workspace_switcher::drop_window(id);
                drag_workspace_switcher::end_drag(id);
                drag_workspace_switcher::close_visually();
            };
            hypriso->on_drag_or_resize_cancel_requested = on_drag_or_resize_cancel_requested;
            hypriso->on_config_reload = on_config_reload;
            hypriso->on_activated = on_activated;
            hypriso->on_config_generated = on_config_generated;
            hypriso->on_requests_max_or_min = on_requests_max_or_min;
            hypriso->on_workspace_change = on_workspace_change;
            hypriso->on_workspace_tiling_change = apply_workspace_tiling;
            hypriso->on_window_workspace_change = on_window_workspace_change;
            hypriso->on_workspace_windows_change = []() {
                // A window's monitor is updated after the workspace move event.
                main_thread(dock::update_workspaces);
            };

        	hypriso->create_callbacks();
        	hypriso->create_hooks();

            // create_hooks read the instance signature the last run saw. A reload
            // of the plugin in the same Hyprland session only runs "Plugin start".
            {
                const bool new_session = hyprland_instance_name() != last_hyprland_instance_name();
                for (const auto &entry : set->autostart)
                    if (entry.when == "Plugin start" || (new_session && entry.when == "Session start"))
                        launch_command(entry.command);
            }

            hypriso->add_float_rule();

            later(100, [](Timer *t) {
                // Retry for up to ~30s; a slower first-run cache build is
                // handled by the worker thread below when it finishes.
                static int attempts = 0;
                t->keep_running = !icons_loaded && ++attempts < 300;
                desktop_icons::start();
                damage_all();
            });

            if (icon_cache_needs_update()) {
                std::thread th([] {
                    icon_cache_generate();
                    icon_cache_load();
                    main_thread([]() {
                        desktop_icons::start();
                        damage_all();
                    });
                });
                th.detach();
            } else {
                icon_cache_load();
            }

            create_rounding_shader();
            create_default_config();
            
            watch_wallpaper_change();
            overview::start_preloader();
            // Decode wallpapers into memory once startup settles, for the Display page.
            later(3000, [](Timer *) { settings::preload_wallpapers(); });

            //if (hyprland_instance_name() != last_hyprland_instance_name())
                //overview::open(hypriso->monitor_from_cursor());
        });
    });
}

void heart::end() {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    set_cursor_hidden_for_desktop_fade(false);
    hypriso->on_config_reload = nullptr;
    hypriso->on_monitor_layout_change = nullptr;
    hypriso->on_resize_ended = nullptr;
    hypriso->on_tiled_drag_started = nullptr;
    hypriso->on_tiled_drag_motion = nullptr;
    hypriso->on_tiled_drag_ended = nullptr;
    hypriso->on_workspace_tiling_change = nullptr;
    hypriso->on_window_workspace_change = nullptr;
    
    save_restore_infos();
    
    settings::stop();
    dock::stop();
    
    settings::load_save_settings(true, set);
    poll_thread->stop();
    
    audio_stop();
    audio_join();
    audio_stop_blip();
    if (latest_file_watch != -1)
        remove_watch(latest_file_watch);
    desktop_icons::stop();
    systray::stop();
    dbus_end();
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
    
    hypriso->end();    
}

void heart::layout_containers() {
    if (actual_monitors.empty())
        return;

    { // TODO: go through monitors and clac top left x top left y and br x y and set the actual_root real_bounds
        int tlx = -1;
        int tly = -1;
        int brx = -1;
        int bry = -1;
        for (int i = 0; i < (int) actual_monitors.size(); i++) {
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
            for (int i = 0; i < (int) order.size(); i++)
               if (order[i] == cid) {
                   const bool is_pinned = hypriso->is_pinned(cid);
                   const bool is_visible_special = hypriso->get_client_workspace_id(cid) < 0 && hypriso->is_window_workspace_visible(cid);
                   const int stacking_span = static_cast<int>(order.size()) + 1;
                   *sort_index = i + (is_visible_special ? stacking_span : 0) + (is_pinned ? 2 * stacking_span : 0);
               }
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
        // Not gated on resizable(): non-resizable X11 windows (most fullscreen games) would
        // otherwise stop blocking input, letting clicks fall through to the desktop icons.
        bool exists = hypriso->is_mapped(cid) && !hypriso->is_hidden(cid) && hypriso->is_window_workspace_visible(cid) && !is_slept(cid);
        if (hypriso->whitelist_on) {
            bool found = false;
            for (auto wid : hypriso->render_whitelist) {
                if (wid == cid)
                    found = true;
            }
            if (!found) {
                exists = false;
            }
        }
        set_exists(c, exists);
        
        if (c->exists || hypriso->whitelist_on) {
            auto b = bounds_client(cid);

            auto fo = hypriso->floating_offset(cid);
            auto so = hypriso->workspace_offset(cid);
            if (hypriso->has_decorations(cid)) {
                //auto boxxy = hypriso->getTexBox(cid);
                //boxxy.scale();
                //boxxy.round();
                //auto b = boxxy;
                //c->real_bounds = Bounds(b.x, b.y - titlebar_h, b.w, b.h + titlebar_h);

                c->real_bounds = Bounds(b.x + fo.x + so.x, std::round(b.y - titlebar_h + fo.y + so.y), b.w, std::round(b.h + titlebar_h));
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
        if (c->custom_type == (int) TYPE::DESKTOP_ICONS) {
            actual_root->children.push_back(c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }
    }

    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
                *datum<bool>(c, "touched") = true;
            }
        }
        if (c->custom_type == (int) TYPE::CLIENT_RESIZE) {
            auto id = *datum<int>(c, "cid");
            bool exists = hypriso->is_mapped(id) && !hypriso->is_hidden(id) && hypriso->resizable(id) && hypriso->is_window_workspace_visible(id) && !is_slept(id);
            if (hypriso->whitelist_on) {
                bool found = false;
                for (auto wid : hypriso->render_whitelist) {
                    if (wid == id)
                        found = true;
                }
                if (!found) {
                    exists = false;
                }
            }
            set_exists(c, exists);
            
            for (int i = actual_root->children.size() - 1; i >= 0; i--) {
                auto child = actual_root->children[i];
                if (child->custom_type == (int) TYPE::CLIENT) {
                    auto cid = *datum<int>(child, "cid");
                    if (cid == id) {
                        c->real_bounds = child->real_bounds;
                        actual_root->children.insert(actual_root->children.begin() + i, c);
                        *datum<bool>(c, "touched") = true;
                        if (c->pre_layout) {
                            c->pre_layout(actual_root, c, actual_root->real_bounds);
                        }
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

    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::WORKSPACE_CHANGE_INDICATOR) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
                *datum<bool>(c, "touched") = true;
            }
        }
    }
    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::ALT_TAB) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
                *datum<bool>(c, "touched") = true;
            }
        }
    }
    
    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
                *datum<bool>(c, "touched") = true;
            }
        }
    }
    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::OUR_POPUP) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }
    }
    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::POPUP) {
            bool inserted = false;

            if (true) {
                auto parent_cid = *datum<int>(c, "parent_cid");
                for (int i = c->parent->children.size() - 1; i >= 0; i--) {
                    auto p = c->parent->children[i];
                    if (p->custom_type == (int) TYPE::LAYER || p->custom_type == (int) TYPE::CLIENT) {
                        auto pcid = *datum<int>(p, "cid");
                        if (pcid == parent_cid) {
                            inserted = true;
                            c->parent->children.insert(c->parent->children.begin() + i, c);
                            break;
                        }
                    }
                }
            }

            if (!inserted)
                c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }
    }
    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::COVERFLOW) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }
    }
    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::DOCK_THUMBNAILS) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }
    }
    // Menus opened from the dock thumbnails must win hit-testing over the
    // preview they overlap, so move them ahead of it.
    for (auto c : backup) {
        if (c->custom_type != (int) TYPE::OUR_POPUP)
            continue;
        auto *data = (PopupUserData *) c->user_data;
        if (!data || !data->above_overlays)
            continue;
        auto &children = c->parent->children;
        children.erase(std::remove(children.begin(), children.end(), c), children.end());
        children.insert(children.begin(), c);
    }
    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::SCREENSHOT_TOOL) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout) {
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            }
            *datum<bool>(c, "touched") = true;
        }
    }
    for (auto c : backup) {
        if (c->custom_type == (int) TYPE::MAGIC_WORKSPACE_EXIT) {
            c->parent->children.insert(c->parent->children.begin(), c);
            if (c->pre_layout)
                c->pre_layout(actual_root, c, c->parent->real_bounds);
            ::layout(actual_root, c, c->real_bounds);
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
    c->when_fine_scrolled = [](Container* root, Container* c, double scroll_x, double scroll_y, bool came_from_touchpad) {
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
        // The compositor blocks SIGTERM and ignores SIGPIPE/SIGQUIT; both
        // survive exec, so without this launched programs can't be killed
        // with a plain kill/killall and misbehave on broken pipes.
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int sig : {SIGPIPE, SIGQUIT, SIGINT, SIGCHLD, SIGTERM, SIGHUP})
            signal(sig, SIG_DFL);
        setsid();

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
        return false; // never self
    if (a == SnapPosition::MAX || b == SnapPosition::MAX)
        return false; // never on max
    bool a_on_left = a == SnapPosition::LEFT || a == SnapPosition::TOP_LEFT || a == SnapPosition::BOTTOM_LEFT;
    bool b_on_left = b == SnapPosition::LEFT || b == SnapPosition::TOP_LEFT || b == SnapPosition::BOTTOM_LEFT;
    if (a_on_left != b_on_left) {
        // opposite sides yes
        return true;
    } else {
        // same sides
        bool a_on_top = a == SnapPosition::TOP_LEFT || a == SnapPosition::TOP_RIGHT;
        bool b_on_top = b == SnapPosition::TOP_LEFT || b == SnapPosition::TOP_RIGHT;
        // if on same side and take up entire place return false
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

void heart::set_force_meta_open(bool state) {
    force_meta_open = state;
}

void heart::set_zoom(float zoom) {
    zoom_factor = zoom;
    hypriso->set_zoom_factor(zoom);
}

bool heart::shift_pressed() {
    return SHIFT_PRESSED;
}

bool heart::ctrl_pressed() {
    return CTRL_PRESSED;
}


void update_mute_state_for_pid(int pid, bool mute_when_unfocused, bool focused) {
    audio([pid, mute_when_unfocused, focused]() {
        for (auto c : audio_clients) {
            if (pid == c->pid) {
                c->set_mute(mute_when_unfocused && !focused, false);
            }
        }
    });
}

void update_mute_states_for_pids(int cid) {
    if (cid != 0) {
        auto info = &restore_infos[hypriso->class_name(cid)];
        update_mute_state_for_pid(hypriso->get_pid(cid), info->mute_when_unfocused, hypriso->has_focus(cid));
        return;
    }

    auto windows = get_window_stacking_order();

    struct Target {
        int pid;
        bool mute_when_unfocused;
        bool focused;
    };
    std::vector<Target> targets;
    for (auto cid : windows) {
        auto info = &restore_infos[hypriso->class_name(cid)];
        targets.push_back({hypriso->get_pid(cid), info->mute_when_unfocused, hypriso->has_focus(cid)});
    }
    audio([targets]() {
        for (auto c : audio_clients) {
            bool matched = false;
            bool should_mute = false;
            for (auto &target : targets) {
                if (target.pid == c->pid) {
                    matched = true;
                    should_mute = should_mute || (target.mute_when_unfocused && !target.focused);
                }
            }
            if (matched)
                c->set_mute(should_mute, false);
        }
    });
}
