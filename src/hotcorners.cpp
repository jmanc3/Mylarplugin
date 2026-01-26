
#include "hotcorners.h"

#include "heart.h"
#include "alt_tab.h"
#include "drag.h"
#include "resizing.h"
#include "overview.h"
#include "drag_workspace_switcher.h"

#include <linux/input-event-codes.h>

void do_alt_tab() {
    static long last_time = 0;
    auto current = get_current_time_in_ms();
    if (current - last_time > 300) {
        later_immediate([](Timer *) { 
            auto order = get_window_stacking_order();
            std::reverse(order.begin(), order.end());
            bool first = true;
            for (auto o : order) {
                if (hypriso->alt_tabbable(o)) {
                    if (!first) {
                        hypriso->bring_to_front(o, true);
                        break;
                    }
                    first = false;
                }
            }
        });
        last_time = current;
    }
}

void do_overview(int monitor_id) {
    static long last_time = 0;
    auto current = get_current_time_in_ms();
    if (current - last_time > 300) {
        overview::open(monitor_id);
        last_time = current;
    }
}

void do_spotify_toggle() {
    static long last_time = 0;
    auto current = get_current_time_in_ms();
    auto drag_end_time = *datum<long>(actual_root, "drag_end_time");
    if (current - last_time > 200) {
        // focus spotify
        bool found = false;
        for (auto w : get_window_stacking_order()) {
            if (hypriso->class_name(w) == "Spotify") {
                found = true;
                hypriso->bring_to_front(w);
            }
        }
        if (!found) {
            system("nohup bash -c '/home/jmanc3/Scripts/./spotifytoggle.sh &'");
        } else {
            later(100, [](Timer* data) {
                hypriso->send_key(KEY_SPACE);
                later(100, [](Timer* data) {
                    do_alt_tab();
                    //alt_tab_menu->change_showing(true);
                    //tab_next_window();
                    //alt_tab_menu->change_showing(false);
                });
            });
        } 
        last_time = current;
    }
}

bool any_fullscreen(int monitor) {
    auto order = get_window_stacking_order();
    for (auto o : order) {
        if (get_monitor(o) == monitor) {
            if (hypriso->is_fullscreen(o)) {
                return true;
            }
        }
    }
    return false;
}

void monitor_hotspot(Container *m, int x, int y, int monitor_id) {
    if (drag::dragging() || resizing::resizing() || overview::is_showing() || any_fullscreen(monitor_id))
        return;
    auto current = get_current_time_in_ms();
    auto drag_end_time = *datum<long>(actual_root, "drag_end_time");
    if (current - drag_end_time < 400)
        return;        
    int x_off = x - m->real_bounds.x;
    float y_off = (y - m->real_bounds.y);
    if (x_off < 1) {
        if (y_off > 100) {
            if (y_off < 500) {
                do_spotify_toggle();
            } else {
                do_alt_tab();
            }
        } else if (y_off < 50) {
            do_overview(monitor_id);
        }
    }
    if (y_off < 1) {
        auto center_x = m->real_bounds.w * .5;
        if (x_off >= center_x - 200 && x_off <= center_x + 200) {
            //drag_workspace_switcher::open();
        }
    }
}

void hotcorners::motion(int id, int x, int y) {
    for (auto m : actual_monitors) {
        auto cid = *datum<int>(m, "cid");
        auto monitor_bounds = bounds_monitor(cid);
        auto xx = x - monitor_bounds.x;
        auto yy = y - monitor_bounds.y;
        m->real_bounds = monitor_bounds;
        if (bounds_contains(monitor_bounds, xx, yy)) {
            monitor_hotspot(m, xx, yy, cid);
            break;
        }
    }
}

