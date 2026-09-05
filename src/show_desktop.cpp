#include "show_desktop.h"

#include "container.h"
#include "hypriso.h"
#include "dock.h"
#include "heart.h"
#include "overview.h"
#include "snap_assist.h"
#include "spring.h"
#include "desktop_gesture.h"

static bool is_open = false;
static bool opening = false;
static unsigned int lifecycle = 0;

bool show_desktop::is_opened() {
    return is_open || opening;
}

void show_desktop::start() {
    if (is_opened() || overview::is_showing() || snap_assist::is_showing())
        return;
    bool any_client_visible = false;
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::CLIENT) {
            auto cid = *datum<int>(c, "cid");
            if (!hypriso->is_hidden(cid) && !is_slept(cid))
                any_client_visible = true;
        }
    }
    if (!any_client_visible)
        return;
    
    opening = true;
    const auto generation = ++lifecycle;
    later_immediate([generation](Timer *) {
        if (!opening || generation != lifecycle)
            return;
        for (auto c : actual_root->children) {
            if (c->custom_type != (int) TYPE::CLIENT)
                continue;
            auto cid = *datum<int>(c, "cid");
            hypriso->screenshot_min(cid);
            hypriso->set_animate_to_dock(cid, true);
        }
 
        is_open = true;
        opening = false;
        hypriso->whitelist_on = true;
        damage_all();
        later(20, [generation](Timer *) {
            if (is_open && generation == lifecycle)
                hypriso->simulateMouseMovement();
        });
    });

    later((1000.0f / hypriso->fps(current_rendering_monitor())) * .5, [generation](Timer *t) {
        t->keep_running = is_opened() && generation == lifecycle;
        if (!t->keep_running || !is_open)
            return;
        if (show_desktop::get_scalar() != 1.0) {
            for (auto c : actual_root->children) {
                if (c->custom_type != (int) TYPE::CLIENT)
                    continue;
                auto cid = *datum<int>(c, "cid");
                // hypriso->set_animate_to_dock(cid, true);
                hypriso->screenshot_min(cid);
            }
        }
    });
}

static void actual_stop() {
    is_open = false;
    hypriso->whitelist_on = false;
    hypriso->render_whitelist.clear();
    for (auto c : actual_root->children) {
        if (c->custom_type != (int) TYPE::CLIENT)
            continue;
        auto cid = *datum<int>(c, "cid");
        hypriso->set_animate_to_dock(cid, false);
    }
    damage_all();
}

void show_desktop::stop() {
    minimize_gesture_count++;
    lifecycle++;
    opening = false;
    show_desktop::set_scalar(0);
    if (!is_open)
        return;

    damage_all();
    actual_stop();
}

static float conf_scalar = 0.0;

void show_desktop::set_scalar(float scalar) {
    conf_scalar = scalar;
}

float show_desktop::get_scalar() {
    return conf_scalar;
}

void show_desktop::render() {
    if (!is_open || show_desktop::get_scalar() == 1.0)
        return;
    
    auto current_monitor = current_rendering_monitor();
    auto current_workspace_id = hypriso->get_active_workspace_id(current_monitor);

    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type != (int) TYPE::CLIENT)
            continue;
            
        auto cid = *datum<int>(c, "cid");
        if (hypriso->is_hidden(cid) || is_slept(cid))
            continue;

        auto mon_id = get_monitor(cid);
        if (mon_id != current_monitor)
            continue;
        
        auto cl_id = hypriso->get_active_workspace_id_client(cid);
        if (cl_id != current_workspace_id)
            continue;
 
        auto bounds = dock::get_location(hypriso->monitor_name(mon_id), cid);
        auto monitor_b = bounds_monitor(mon_id);
        bounds.y = monitor_b.h;
        bounds.scale(scale(mon_id));
        
        hypriso->draw_raw_min_thumbnail(cid, bounds, 1.0 - conf_scalar);
    }

    damage_all();
    request_refresh();
}


int minimize_gesture_count = 0;

static void actual_spring_anim(long end, float initialVelocity, float scalar_at_start, float target, int start_count) {
    later((1000.0f / hypriso->fps(current_rendering_monitor())) * .8, [end, initialVelocity, scalar_at_start, target, start_count](Timer *t) {
        t->keep_running = true;
        if (minimize_gesture_count != start_count || !show_desktop::is_opened()) {
            request_refresh();
            t->keep_running = false;
            return;
        }

        const auto elapsed = (get_current_time_in_ms() - end) / 1000.0;
        const auto state = springEvaluate(elapsed, scalar_at_start, target, initialVelocity, {0.3, 1.0});
        const auto scalar = static_cast<float>(state.value);

        if (target == 0.0 && scalar <= 0.001f) {
            show_desktop::set_scalar(0.0);
            show_desktop::stop();
            t->keep_running = false;
            request_refresh();
            return;
        }

        if (target == 1.0 && scalar >= 0.999f) {
            show_desktop::set_scalar(1.0);
            t->keep_running = false;
            request_refresh();
            return;
        }

        show_desktop::set_scalar(scalar);
        request_refresh();
    });
}

void show_desktop::minimize_animate_out(long start, long end, float y_offset, float scalar_at_start) {
    if (!is_opened())
        return;
    if (scalar_at_start < .01f) {
        stop();
        return;
    }
    minimize_gesture_count++;
    if (scalar_at_start > .99f) {
        set_scalar(1.0f);
        return;
    }
    const auto release = desktop_gesture::release(start, end, y_offset, scalar_at_start);
    actual_spring_anim(end, release.velocity, scalar_at_start, release.target, minimize_gesture_count);
}

void show_desktop::start_animation() {
    start();
    if (!is_opened())
        return;
    minimize_gesture_count++;
    actual_spring_anim(get_current_time_in_ms(), 0.0, get_scalar(), 1.0, minimize_gesture_count);
}

void show_desktop::stop_animation() {
    if (!is_opened())
        return;
    if (opening) {
        stop();
        return;
    }
    minimize_gesture_count++;
    actual_spring_anim(get_current_time_in_ms(), 0.0, get_scalar(), 0.0, minimize_gesture_count);
}
