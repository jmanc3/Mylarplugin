#include "show_desktop.h"

#include "container.h"
#include "hypriso.h"
#include "dock.h"
#include "heart.h"
#include "overview.h"
#include "snap_assist.h"

static bool is_open = false;
static float conf_anim_time = 100.0;
static long start_time = 0;
static long end_time = 0;

bool show_desktop::is_opened() {
    return is_open;
}

void show_desktop::start() {
    if (is_open)
        return;
    if (overview::is_showing())
        return;
    if (snap_assist::is_showing())
        return;
    bool any_client_visible = false;
    for (auto c : actual_root->children) {
        if (c->custom_type != (int) TYPE::CLIENT) {
            auto cid = *datum<int>(c, "cid");
            if (!hypriso->is_hidden(cid) && !is_slept(cid) ) {
                any_client_visible = true;
            }
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
        start_time = get_current_time_in_ms();
        damage_all();
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
    start_time = 0;
    end_time = 0;
    damage_all();
}

void show_desktop::stop(bool animate) {
    if (!is_open)
        return;

    if (animate) {
        end_time = get_current_time_in_ms();
    } else {
        end_time = get_current_time_in_ms() - conf_anim_time * 2;
    }
    damage_all();
    if (animate) {
        later(conf_anim_time, [](Timer *) {
            actual_stop();
        });
    } else {
        actual_stop();
    }

}

void show_desktop::render() {
    auto current_time = get_current_time_in_ms();
    long delta = current_time - start_time;
    if (end_time != 0) {
        delta = current_time - end_time;
    }
    auto scalar = ((float) delta) / conf_anim_time;
    if (scalar > 1.0) {
        if (scalar < 1.5) {
            damage_all();
            request_refresh();
            return;
        } else {
            return;
        }
    }

    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        if (c->custom_type != (int) TYPE::CLIENT)
            continue;
            
        auto cid = *datum<int>(c, "cid");
        if (hypriso->is_hidden(cid) || is_slept(cid))
            continue;

        auto mon_id = get_monitor(cid);
        //auto bounds = dock::get_location(hypriso->monitor_name(mon_id), cid);
        auto bounds = bounds_monitor(mon_id);

        //auto monitor_b = bounds_monitor(mon_id);
        auto s = scale(mon_id);
        bounds = bounds.scale(s);
        
        bounds.y += bounds.h * .5;
        bounds.h = 100 * s;
        bounds.y -= 100 * s;
        
        bounds.x += bounds.w * .5;
        bounds.w = 100 * s;
        bounds.x -= bounds.w * .5;
        //bounds.scale(scale(mon_id));
        if (end_time != 0) {
            hypriso->draw_raw_min_thumbnail(cid, bounds, scalar);
        } else {
            hypriso->draw_raw_min_thumbnail(cid, bounds, 1.0 - scalar);
        }
    }

    damage_all();
    request_refresh();
}
