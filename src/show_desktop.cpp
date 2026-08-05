#include "show_desktop.h"

#include "container.h"
#include "hypriso.h"
#include "dock.h"
#include "heart.h"
#include "overview.h"
#include "snap_assist.h"

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
