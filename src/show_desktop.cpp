#include "show_desktop.h"

#include "container.h"
#include "hypriso.h"
#include "dock.h"
#include "heart.h"
#include "overview.h"
#include "snap_assist.h"
#include "spring.h"

static bool is_open = false;

bool show_desktop::is_opened() {
    return is_open;
}

void show_desktop::start() {
    if (is_open || overview::is_showing() || snap_assist::is_showing())
        return;
    bool any_client_visible = false;
    for (auto c : actual_root->children) {
        if (c->custom_type != (int) TYPE::CLIENT) {
            auto cid = *datum<int>(c, "cid");
            if (!hypriso->is_hidden(cid) && !is_slept(cid))
                any_client_visible = true;
        }
    }
    if (!any_client_visible)
        return;
    
    later_immediate([](Timer *) {
        for (auto c : actual_root->children) {
            if (c->custom_type != (int) TYPE::CLIENT)
                continue;
            auto cid = *datum<int>(c, "cid");
            hypriso->screenshot_min(cid);
            hypriso->set_animate_to_dock(cid, true);
        }
 
        is_open = true;
        hypriso->whitelist_on = true;
        damage_all();
        later(20, [](Timer *) { hypriso->simulateMouseMovement(); });
    });

    later((1000.0f / hypriso->fps(current_rendering_monitor())) * .5, [](Timer *t) {
        t->keep_running = is_open;
        for (auto c : actual_root->children) {
            if (c->custom_type != (int) TYPE::CLIENT)
                continue;
            auto cid = *datum<int>(c, "cid");
            hypriso->screenshot_min(cid);
        }
    });
}

void actual_stop() {
    is_open = false;
    hypriso->whitelist_on = false;
    for (auto c : actual_root->children) {
        if (c->custom_type != (int) TYPE::CLIENT)
            continue;
        auto cid = *datum<int>(c, "cid");
        hypriso->set_animate_to_dock(cid, false);
    }
    damage_all();
}

void show_desktop::stop() {
    if (!is_open)
        return;
    show_desktop::set_scalar(0);

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
    if (!show_desktop::is_opened())
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
        if (minimize_gesture_count != start_count) {
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
            return;
        }

        if (target == 1.0 && scalar >= 0.999f) {
            show_desktop::set_scalar(1.0);
            t->keep_running = false;
            return;
        }

        show_desktop::set_scalar(scalar);
        request_refresh();
    });
}

void show_desktop::minimize_animate_out(long start, long end, float y_offset, float scalar_at_start) {
    //constexpr static float slow = .42;
    constexpr static float slow = 1.0;
    
    const auto gestureDuration = std::max(end - start, 1L) / 1000.0;
    const auto gestureVelocity = std::abs(y_offset) / (250.0 * .42) / gestureDuration;
    constexpr auto flickVelocity = 0.6 * slow;
    const bool isFlick = gestureVelocity >= flickVelocity;

    // Flicks can complete with less travel: 0.15 to open, 0.85 to close.
    // Slow releases still require crossing the midpoint.
    double target;
    if (isFlick && y_offset > 0)
        target = scalar_at_start >= 0.01f ? 1.0 : 0.0;
    else if (isFlick && y_offset < 0)
        target = scalar_at_start <= 0.99f ? 0.0 : 1.0;
    else
        target = scalar_at_start < 0.5f ? 0.0 : 1.0;

    auto initialVelocity = target < scalar_at_start ? -gestureVelocity : gestureVelocity;
    initialVelocity *= .8;
    float start_count = minimize_gesture_count;

    actual_spring_anim(end, initialVelocity, scalar_at_start, target, start_count);
}

void show_desktop::start_animation() {
    show_desktop::start();
    later(10, [](Timer *) {
        minimize_gesture_count++;
        actual_spring_anim(get_current_time_in_ms(), 0.0, get_scalar(), 1.0, minimize_gesture_count);
    });
}

void show_desktop::stop_animation() {
    minimize_gesture_count++;
    actual_spring_anim(get_current_time_in_ms(), 0.0, get_scalar(), 0.0, minimize_gesture_count);
}
