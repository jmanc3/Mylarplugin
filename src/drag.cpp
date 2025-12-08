#include "drag.h"

#include "second.h"
#include "defer.h"
#include "snap_preview.h"

#include <string>
#include <cmath>

struct DraggingData {
    int cid = -1;
    Bounds mouse_start;
    Bounds bounds_start;
};

DraggingData *data = nullptr;

void drag::begin(int cid) {
    if (!hypriso->is_floating(cid)) {
        auto b = bounds_client(cid);
        hypriso->move_resize(cid, b.x, b.y - titlebar_h * 2, b.w, b.h);
        later_immediate([cid](Timer *) {
            hypriso->do_default_drag(cid);
        });
        return;
    }
    //notify(fz("wants no decorations {}", hypriso->requested_client_side_decorations(cid)));
    data = new DraggingData;
    data->cid = cid;
    data->mouse_start = mouse();
    clear_snap_groups(cid); 
    snap_preview::on_drag_start(cid, data->mouse_start.x, data->mouse_start.y);
    auto c = get_cid_container(cid);
    if (*datum<bool>(c, "drag_from_titlebar")) {
        data->mouse_start = *datum<Bounds>(c, "initial_click_position");
    }
    auto client_snapped = datum<bool>(c, "snapped");
    if (*client_snapped) {
        data->bounds_start = bounds_client(cid);

        *client_snapped = false;
        hypriso->should_round(cid, true);

        auto client_pre_snap_bounds = *datum<Bounds>(c, "pre_snap_bounds");
        //data->bounds_start.w = client_pre_snap_bounds.w;
        //data->bounds_start.h = client_pre_snap_bounds.h;

        auto b = data->bounds_start;
        auto MOUSECOORDS = data->mouse_start;
        auto mb = bounds_monitor(get_monitor(cid));

        float perc = (MOUSECOORDS.x - b.x) / b.w;
        bool window_left_side = b.x < mb.x + b.w * .5;
        bool click_left_side = perc <= .5;
        float size_from_left = b.w * perc;
        float size_from_right = b.w - size_from_left;
        bool window_smaller_after = b.w > client_pre_snap_bounds.w;
        float x = MOUSECOORDS.x - (perc * (client_pre_snap_bounds.w)); // perc based relocation
        log(fz("{} {} {} {} {} {} {}", perc, window_left_side, click_left_side, size_from_left, size_from_right, window_smaller_after, x));
        // keep window fully on screen
        if (!window_smaller_after) {
            if (click_left_side) {
                if (window_left_side) {
                    x = MOUSECOORDS.x - size_from_left;
                } else {
                    x = MOUSECOORDS.x - client_pre_snap_bounds.w + size_from_right;
                }
            } else {
                if (window_left_side) {
                    x = b.x;
                } else {
                    x = MOUSECOORDS.x - client_pre_snap_bounds.w + size_from_right;
                }
            }
        } else {
            // if offset larger than resulting window use percentage
        }

        hypriso->move_resize(cid, 
            x, 
            b.y, 
            client_pre_snap_bounds.w, 
            client_pre_snap_bounds.h);
        
        data->bounds_start = bounds_client(cid);
    } else {
        data->bounds_start = bounds_client(cid);
    }
    drag::motion(cid);
}

void drag::motion(int cid) {
    if (!data)
        return;
    auto mouse_current = mouse();
    auto diff_x = mouse_current.x - data->mouse_start.x;
    auto diff_y = mouse_current.y - data->mouse_start.y;
    auto new_bounds = data->bounds_start;
    new_bounds.x += diff_x;
    new_bounds.y += diff_y;
    hypriso->move_resize(cid, new_bounds);
    { // damage 
        Bounds b = bounds_full_client(cid);
        static Bounds p = b;
        b.grow(20);
        hypriso->damage_box(b);
        hypriso->damage_box(p);
        p = b;
    }

    snap_preview::on_drag(cid, mouse_current.x, mouse_current.y);
}

// TODO: multi-monitor broken
// I don't think it's mon, it's workspace that we have to worry about
void drag::snap_window(int snap_mon, int cid, int pos) {
    if (snap_mon == -1)
        return;
    auto c = get_cid_container(cid);
    if (!c) return;

    auto snapped = datum<bool>(c, "snapped");
    
    if (!(*snapped) && pos == (int) SnapPosition::NONE) // no need to unsnap
        return;

    if (*snapped) {
        // perform unsnap
        *snapped = false; 
        auto p = *datum<Bounds>(c, "pre_snap_bounds");
        auto mon = snap_mon;
        auto bounds = bounds_reserved_monitor(mon);
        p.x = bounds.x + bounds.w * .5 - p.w * .5; 
        if (p.x < bounds.x)
            p.x = bounds.x;
        p.y = bounds.y + bounds.h * .5 - p.h * .5; 
        if (p.y < bounds.y)
            p.y = bounds.y;
        hypriso->move_resize(cid, p.x, p.y, p.w, p.h, false);
        hypriso->should_round(cid, true);
        clear_snap_groups(cid);
    } else {
        // perform snap
        *snapped = true; 
        *datum<Bounds>(c, "pre_snap_bounds") = bounds_client(cid);
        *datum<int>(c, "snap_type") = pos;

        auto p = snap_position_to_bounds(snap_mon, (SnapPosition) pos);
        if (pos != (int) SnapPosition::MAX) {
            //p.shrink(1 * scale(snap_mon));
        }
        if (hypriso->has_decorations(cid)) {
            hypriso->move_resize(cid, p.x, p.y + titlebar_h, p.w, p.h - titlebar_h, false);
        } else {
            hypriso->move_resize(cid, p.x, p.y, p.w, p.h, false);
        }
        hypriso->should_round(cid, false); 
    }
    hypriso->damage_entire(snap_mon);

    // This sends
    later(new int(0), 10, [](Timer *t) {
        t->keep_running = true;
        int *times = (int *) t->data; 
        *times = (*times) + 1;
        if (*times > 20) {
            t->keep_running = false;
            delete (int *) t->data;
        }
        if (hypriso->on_mouse_move) {
            auto m = mouse();
            hypriso->on_mouse_move(0, m.x, m.y);
        }
    });
}

void drag::end(int cid) {
    //notify("end");
    delete data;
    data = nullptr;

    int mon = hypriso->monitor_from_cursor();
    auto m = mouse();
    auto pos = mouse_to_snap_position(mon, m.x, m.y);
    snap_preview::on_drag_end(cid, m.x, m.y, (int) pos);
    snap_window(mon, cid, (int) pos);
    *datum<long>(actual_root, "drag_end_time") = get_current_time_in_ms();

    if (auto c = get_cid_container(cid)) {
        *datum<bool>(c, "drag_from_titlebar") = false;
        bool is_snapped = *datum<bool>(c, "snapped");
        if (!(is_snapped)) {
            update_restore_info_for(cid);
        } else {
            bool create_snap_helper = true;
            // attempt to merge
            // find first top to bottom snapped client that is not self
            // if not mergeble, don't do anything special
            // if can be merged, then merge by adding to itself and other to each other groups
            for (auto ch : actual_root->children) {
                if (ch->custom_type == (int)TYPE::CLIENT) {
                    auto other_cdata = (ClientInfo*)ch->user_data;
                    auto othercid = *datum<int>(ch, "cid");
                    if (othercid == cid)
                        continue; // skip self
                    auto other_client = get_cid_container(othercid);
                    if (!(*datum<bool>(other_client, "snapped")))
                        continue; // skip non snapped
                    std::vector<int> ids;
                    ids.push_back(othercid);
                    for (auto grouped_id : other_cdata->grouped_with)
                        ids.push_back(grouped_id);
                    bool mergable = groupable(((SnapPosition)*datum<int>(c, "snap_type")), ids);
                    if (mergable) {
                        add_to_snap_group(cid, othercid, other_cdata->grouped_with);
                        create_snap_helper = false;
                    } else {
                        // if first merge attempt fails, we don't seek deeper layers
                        break;
                    }
                }
            }

            auto client = c;
            {
                float left_perc = .5;
                float middle_perc = .5;
                float right_perc = .5;
                Bounds reserved = bounds_reserved_monitor(get_monitor(cid));

                //auto gs = thin_groups(c->id);
                auto info = (ClientInfo*)client->user_data;
                for (auto g : info->grouped_with) {
                    auto b = bounds_client(g);
                    auto g_snap_type = *datum<int>(get_cid_container(g), "snap_type");
                    if (g_snap_type == (int)SnapPosition::TOP_LEFT) {
                        left_perc = (b.h + titlebar_h) / reserved.h;
                        middle_perc = (b.w) / reserved.w;
                    } else if (g_snap_type == (int)SnapPosition::TOP_RIGHT) {
                        right_perc = (b.h + titlebar_h) / reserved.h;
                        middle_perc = 1.0 - ((b.w) / reserved.w);
                    } else if (g_snap_type == (int)SnapPosition::BOTTOM_LEFT) {
                        left_perc = (b.y - titlebar_h) / reserved.h;
                        middle_perc = (b.w) / reserved.w;
                    } else if (g_snap_type == (int)SnapPosition::BOTTOM_RIGHT) {
                        right_perc = (b.y - titlebar_h) / reserved.h;
                        middle_perc = 1.0 - ((b.w) / reserved.w);
                    } else if (g_snap_type == (int)SnapPosition::LEFT) {
                        middle_perc = (b.w) / reserved.w;
                    } else if (g_snap_type == (int)SnapPosition::RIGHT) {
                        middle_perc = 1.0 - ((b.w) / reserved.w);
                    }
                }
                if (left_perc == .5 && right_perc != .5) {
                    left_perc = right_perc;
                }
                if (right_perc == .5 && left_perc != .5) {
                    right_perc = left_perc;
                }
                auto r = reserved;
                Bounds lt = Bounds(r.x, r.y, r.w * middle_perc, r.h * left_perc);
                Bounds lb = Bounds(r.x, r.y + r.h * left_perc, r.w * middle_perc, r.h * (1 - left_perc));
                Bounds rt = Bounds(r.x + r.w * middle_perc, r.y, r.w * (1 - middle_perc), r.h * right_perc);
                Bounds rb = Bounds(r.x + r.w * middle_perc, r.y + r.h * right_perc, r.w * (1 - middle_perc), r.h * (1 - right_perc));

                auto c_snap_type = pos;

                auto theight = titlebar_h;
                if (!hypriso->has_decorations(cid))
                    theight = 0;

                if (c_snap_type == SnapPosition::LEFT) {
                    hypriso->move_resize(cid, reserved.x, reserved.y + theight, reserved.w * middle_perc, reserved.h - theight, false);
                } else if (c_snap_type == SnapPosition::RIGHT) {
                    hypriso->move_resize(cid, reserved.x + reserved.w * middle_perc, reserved.y + theight, reserved.w * (1 - middle_perc), reserved.h - theight, false);
                } else if (c_snap_type == SnapPosition::TOP_LEFT) {
                    hypriso->move_resize(cid, lt.x, lt.y + theight, lt.w, lt.h - theight, false);
                } else if (c_snap_type == SnapPosition::BOTTOM_LEFT) {
                    hypriso->move_resize(cid, lb.x, lb.y + theight, lb.w, lb.h - theight, false);
                } else if (c_snap_type == SnapPosition::TOP_RIGHT) {
                    hypriso->move_resize(cid, rt.x, rt.y + theight, rt.w, rt.h - theight, false);
                } else if (c_snap_type == SnapPosition::BOTTOM_RIGHT) {
                    hypriso->move_resize(cid, rb.x, rb.y + theight, rb.w, rb.h - theight, false);
                }
            }

            //snap_assist::create(cid);
        }
    }
    //if (hypriso->on_activated) {
        //later(500, [cid](Timer *) { hypriso->on_activated(cid); });
    //}
}

bool drag::dragging() {
    if (!data)
        return false;
    return data->cid != -1;
}

int drag::drag_window() {
    if (!data)
        return -1;
    return data->cid;
}
