
#include "resizing.h"

#include "heart.h"
#include <experimental/filesystem>

static bool is_resizing = false;
static int window_resizing = -1;
static int initial_x = 0;
static int initial_y = 0;
static int active_resize_type = 0;
static Bounds initial_win_box;

int get_current_resize_type(Container *c);

static float titlebar_button_ratio() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_button_ratio", 1.4375f);
}

float titlebar_icon_h() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_icon_h", 21);
}

float resize_edge_size() {
    return hypriso->get_varfloat("plugin:mylardesktop:resize_edge_size", 10);
}

bool resizing::resizing() {
    return is_resizing;
}

int resizing::resizing_window() {
    return window_resizing;
}

void update_cursor(int type) {
   if (type == (int) RESIZE_TYPE::NONE) {
        unsetCursorImage(true);
    } else if (type == (int) RESIZE_TYPE::BOTTOM) {
        setCursorImageUntilUnset("s-resize");
    } else if (type == (int) RESIZE_TYPE::BOTTOM_LEFT) {
        setCursorImageUntilUnset("sw-resize");
    } else if (type == (int) RESIZE_TYPE::BOTTOM_RIGHT) {
        setCursorImageUntilUnset("se-resize");
    } else if (type == (int) RESIZE_TYPE::TOP) {
        setCursorImageUntilUnset("n-resize");
    } else if (type == (int) RESIZE_TYPE::TOP_LEFT) {
        setCursorImageUntilUnset("nw-resize");
    } else if (type == (int) RESIZE_TYPE::TOP_RIGHT) {
        setCursorImageUntilUnset("ne-resize");
    } else if (type == (int) RESIZE_TYPE::LEFT) {
        setCursorImageUntilUnset("w-resize");
    } else if (type == (int) RESIZE_TYPE::RIGHT) {
        setCursorImageUntilUnset("e-resize");
    }
}

void resizing::begin(int cid, int type) {
    update_cursor(type);
    window_resizing = cid;
    is_resizing = true;
    initial_win_box = bounds_client(cid);
    auto m = mouse();
    initial_x = m.x;
    initial_y = m.y;
    active_resize_type = type;
    for (auto ch : actual_root->children) {
        if (ch->custom_type == (int) TYPE::CLIENT_RESIZE) {
            if (*datum<int>(ch, "cid") == cid) {
                auto vertical_bar_dot_amount_shown = datum<float>(ch, "vertical_bar_dot_amount_shown");
                animate(vertical_bar_dot_amount_shown, 1.0, 130, ch->lifetime);
                auto horizontal_bar_dot_amount_shown = datum<float>(ch, "horizontal_bar_dot_amount_shown");
                animate(horizontal_bar_dot_amount_shown, 1.0, 130, ch->lifetime);
            }
        }
    }

    //notify("resizing");
}

bool grouped_with(Container *client, int snap_type) {
    auto data = (ClientInfo *) client->user_data;
    for (auto cid : data->grouped_with) {
        if (auto container = get_cid_container(cid)) {
            auto type = *datum<int>(container, "snap_type");
            if (type == snap_type)
                return true;
        }
    }
    return false;
}

void update_resize_edge_preview_bar(int cid) {
    Container *c = get_cid_container(cid);
    if (!c) return;
    auto snapped = *datum<bool>(c, "snapped");
    auto snap_type = *datum<int>(c, "snap_type");
    Container *cr = nullptr;
    for (auto ch : actual_root->children) {
        if (ch->custom_type == (int) TYPE::CLIENT_RESIZE) {
            auto ch_cid = *datum<int>(ch, "cid");
            if (ch_cid == cid) {
                cr = ch;
                break;
            }
        }
    }

    auto current_vertical = datum<int>(cr, "vertical_bar_position");
    auto current_horizontal = *datum<int>(cr, "horizontal_bar_position");
    auto current_resize_type = get_current_resize_type(cr);

    RESIZE_TYPE new_vertical_type = RESIZE_TYPE::NONE;
    if (snap_type == (int) SnapPosition::LEFT) {
        if (current_resize_type == (int) RESIZE_TYPE::RIGHT) {
            if (grouped_with(c, (int) SnapPosition::RIGHT) ||
                grouped_with(c, (int) SnapPosition::TOP_RIGHT) ||
                grouped_with(c, (int) SnapPosition::BOTTOM_RIGHT)) {
                if (cr->state.concerned)
                    new_vertical_type = RESIZE_TYPE::RIGHT;
            }
        }
    } else if (snap_type == (int) SnapPosition::RIGHT) {
        if (current_resize_type == (int) RESIZE_TYPE::LEFT) {
            if (grouped_with(c, (int) SnapPosition::LEFT) ||
                grouped_with(c, (int) SnapPosition::TOP_LEFT) ||
                grouped_with(c, (int) SnapPosition::BOTTOM_LEFT)) {
                if (cr->state.concerned)
                    new_vertical_type = RESIZE_TYPE::LEFT;
            }
        }
    } else if (snap_type == (int) SnapPosition::TOP_RIGHT) {
        if (current_resize_type == (int) RESIZE_TYPE::LEFT) {
            if (grouped_with(c, (int) SnapPosition::LEFT) || (grouped_with(c, (int) SnapPosition::TOP_LEFT) && grouped_with(c, (int) SnapPosition::BOTTOM_LEFT))) {
               if (cr->state.concerned)
                    new_vertical_type = RESIZE_TYPE::LEFT;
            }
        }
    } else if (snap_type == (int) SnapPosition::TOP_LEFT) {
        if (current_resize_type == (int) RESIZE_TYPE::RIGHT) {
            if (grouped_with(c, (int) SnapPosition::RIGHT) || (grouped_with(c, (int) SnapPosition::TOP_RIGHT) && grouped_with(c, (int) SnapPosition::BOTTOM_RIGHT))) {
               if (cr->state.concerned)
                    new_vertical_type = RESIZE_TYPE::RIGHT;
            }
        }
    } else if (snap_type == (int) SnapPosition::BOTTOM_LEFT) {
        if (current_resize_type == (int) RESIZE_TYPE::RIGHT) {
            if (grouped_with(c, (int) SnapPosition::RIGHT) || (grouped_with(c, (int) SnapPosition::TOP_RIGHT) && grouped_with(c, (int) SnapPosition::BOTTOM_RIGHT))) {
               if (cr->state.concerned)
                    new_vertical_type = RESIZE_TYPE::RIGHT;
            }
        }
    } else if (snap_type == (int) SnapPosition::BOTTOM_RIGHT) {
        if (current_resize_type == (int) RESIZE_TYPE::LEFT) {
            if (grouped_with(c, (int) SnapPosition::LEFT) || (grouped_with(c, (int) SnapPosition::TOP_LEFT) && grouped_with(c, (int) SnapPosition::BOTTOM_LEFT))) {
               if (cr->state.concerned)
                    new_vertical_type = RESIZE_TYPE::LEFT;
            }
        }
    }

    auto vert_amount = datum<float>(cr, "vertical_bar_amount_shown");
    if (new_vertical_type == RESIZE_TYPE::NONE) {
        if (*vert_amount != 0.0) {
            if (!is_being_animating_to(vert_amount, 0.0))  {
                animate(vert_amount, 0.0, 100.0f, c->lifetime);
            }
        }
    } else {
        if (*vert_amount != 1.0) {
            *current_vertical = (int) new_vertical_type;
            if (!is_being_animating_to(vert_amount, 1.0))  {
                animate(vert_amount, 1.0, 100.0f, c->lifetime);
            }
        }
    }
}

void resize_client(int cid, int resize_type) {
    auto m = mouse();
    Bounds diff = {m.x - initial_x, m.y - initial_y, 0, 0};

    int change_x = 0;
    int change_y = 0;
    int change_w = 0;
    int change_h = 0;

    if (resize_type == (int) RESIZE_TYPE::NONE) {
    } else if (resize_type == (int) RESIZE_TYPE::BOTTOM) {
        change_h += diff.y;
    } else if (resize_type == (int) RESIZE_TYPE::BOTTOM_LEFT) {
        change_w -= diff.x;
        change_x += diff.x;
        change_h += diff.y;
    } else if (resize_type == (int) RESIZE_TYPE::BOTTOM_RIGHT) {
        change_w += diff.x;
        change_h += diff.y;
    } else if (resize_type == (int) RESIZE_TYPE::TOP) {
        change_y += diff.y;
        change_h -= diff.y;
    } else if (resize_type == (int) RESIZE_TYPE::TOP_LEFT) {
        change_y += diff.y;
        change_h -= diff.y;
        change_w -= diff.x;
        change_x += diff.x;
    } else if (resize_type == (int) RESIZE_TYPE::TOP_RIGHT) {
        change_y += diff.y;
        change_h -= diff.y;
        change_w += diff.x;
    } else if (resize_type == (int) RESIZE_TYPE::LEFT) {
        change_w -= diff.x;
        change_x += diff.x;
    } else if (resize_type == (int) RESIZE_TYPE::RIGHT) {
        change_w += diff.x;
    }
    auto size = initial_win_box;
    size.w += change_w;
    size.h += change_h;
    bool y_clipped = false;
    bool x_clipped = false;
    if (size.w < 100) {
        size.w    = 100;
        x_clipped = true;
    }
    if (size.h < 50) {
        size.h    = 50;
        y_clipped = true;
    }
    auto min = hypriso->min_size(cid);

    auto mini = 200;

    min.x = 10;
    min.y = 10;
    min.w = 10;
    min.h = 10;
    if (min.w < mini) {
        min.w = mini;
    }
    
    if (hypriso->is_x11(cid)) {
        //min.x /= s;
        //min.y /= s;
    }
    if (size.w < min.w) {
        size.w    = min.w;
        x_clipped = true;
    }
    if (size.h < min.h) {
        size.h    = min.h;
        y_clipped = true;
    }

    auto pos = initial_win_box;
    auto real = bounds_client(cid);
    if (x_clipped) {
        pos.x = real.x;
    } else {
        pos.x += change_x;
    }
    if (y_clipped) {
        pos.y = real.y;
    } else {
        pos.y += change_y;
    }
    auto fb = Bounds(pos.x, pos.y, size.w, size.h);
    hypriso->move_resize(cid, fb.x, fb.y, fb.w, fb.h);
    for (auto m : actual_monitors) {
        hypriso->damage_entire(*datum<int>(m, "cid"));   
    }
}

double percent_position_clamped(double start, double end, double point) {
    double p = (point - start) / end;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return p;
}

SnapLimits calculate_limits(int monitor, int cid, int snap_type) {
    Bounds r = bounds_reserved_monitor(monitor);
    Bounds b = bounds_client(cid);
    if (hypriso->has_decorations(cid)) {
        b.y -= titlebar_h;
        b.h += titlebar_h;
    }
    
    SnapLimits limits = {.5f, .5f, .5f};
    if (snap_type == (int)SnapPosition::LEFT) {
        limits.middle_middle = percent_position_clamped(r.x, r.w, b.x + b.w);
    } else if (snap_type == (int)SnapPosition::RIGHT) {
        limits.middle_middle = percent_position_clamped(r.x, r.w, b.x);
    } else if (snap_type == (int)SnapPosition::TOP_LEFT) {
        limits.middle_middle = percent_position_clamped(r.x, r.w, b.x + b.w);
        limits.left_middle = percent_position_clamped(r.y, r.h, b.y + b.h);
    } else if (snap_type == (int)SnapPosition::BOTTOM_LEFT) {
        limits.middle_middle = percent_position_clamped(r.x, r.w, b.x + b.w);
        limits.left_middle = percent_position_clamped(r.y, r.h, b.y);
    } else if (snap_type == (int)SnapPosition::TOP_RIGHT) {
        limits.middle_middle = percent_position_clamped(r.x, r.w, b.x);
        limits.right_middle = percent_position_clamped(r.y, r.h, b.y + b.h);
    } else if (snap_type == (int)SnapPosition::BOTTOM_RIGHT) {
        limits.middle_middle = percent_position_clamped(r.x, r.w, b.x);
        limits.right_middle = percent_position_clamped(r.y, r.h, b.y);
    }

    return limits;
}

void adjust_grouped_with(int cid) {
    if (auto c = get_cid_container(cid)) {
        auto snap_against_type = *datum<int>(c, "snap_type");
        auto monitor = get_monitor(cid);
        SnapLimits main_limits = calculate_limits(monitor, cid, snap_against_type);

        log(fz("{} l{} r{} m{}", hypriso->class_name(cid), main_limits.left_middle, main_limits.right_middle, main_limits.middle_middle));
        
        auto info = (ClientInfo *) c->user_data;
        for (auto gid : info->grouped_with) {            
            auto other_pos = (SnapPosition) *datum<int>(get_cid_container(gid), "snap_type");
            SnapLimits other_limits = calculate_limits(monitor, gid, (int) other_pos);
            
            //log(fz("{} l{} r{} m{}, {} {} {}", hypriso->class_name(gid), other_limits.left_middle, other_limits.right_middle, other_limits.middle_middle, main_limits.left_middle, main_limits.right_middle, main_limits.middle_middle));

            // merge main limits into other limits
            if (snap_against_type == (int) SnapPosition::LEFT)  {
                other_limits.middle_middle = main_limits.middle_middle;
            } else if (snap_against_type == (int) SnapPosition::RIGHT) {
                other_limits.middle_middle = main_limits.middle_middle;
            } else if (snap_against_type == (int) SnapPosition::TOP_LEFT) {
                other_limits.middle_middle = main_limits.middle_middle;
                other_limits.left_middle = main_limits.left_middle;
            } else if (snap_against_type == (int) SnapPosition::BOTTOM_LEFT) {
                other_limits.middle_middle = main_limits.middle_middle;
                other_limits.left_middle = main_limits.left_middle;
            } else if (snap_against_type == (int) SnapPosition::TOP_RIGHT) {
                other_limits.middle_middle = main_limits.middle_middle;
                other_limits.right_middle = main_limits.right_middle;
            } else if (snap_against_type == (int) SnapPosition::BOTTOM_RIGHT) {
                other_limits.middle_middle = main_limits.middle_middle;
                other_limits.right_middle = main_limits.right_middle;
            }

            auto b = snap_position_to_bounds_limited(monitor, other_pos, other_limits);
            hypriso->move_resize(gid, b.x, b.y, b.w, b.h);
            
            if (hypriso->has_decorations(gid)) {
                hypriso->move_resize(gid, b.x, b.y + titlebar_h, b.w, b.h - titlebar_h);
            } else {
                hypriso->move_resize(gid, b.x, b.y, b.w, b.h);
            }
        }
    }
}

void resizing::motion(int cid) {
    // debounce 100 ms
    static Timer *timer = nullptr;
    if (timer)
        return;
    timer = later(60, [cid](Timer *t) {
        for (auto c : actual_root->children) {
            if (c->custom_type == (int) TYPE::CLIENT_RESIZE) {
                resize_client(cid, active_resize_type);
                adjust_grouped_with(cid);
            }
        }
        timer = nullptr;
    });
}

void resizing::end(int cid) {
    is_resizing = false;
    window_resizing = -1;
    update_cursor((int) RESIZE_TYPE::NONE);
    update_restore_info_for(cid);
    for (auto ch : actual_root->children) {
        if (ch->custom_type == (int) TYPE::CLIENT_RESIZE) {
            if (*datum<int>(ch, "cid") == cid) {
                auto vertical_bar_dot_amount_shown = datum<float>(ch, "vertical_bar_dot_amount_shown");
                animate(vertical_bar_dot_amount_shown, 0.0, 130, ch->lifetime);
                auto horizontal_bar_dot_amount_shown = datum<float>(ch, "horizontal_bar_dot_amount_shown");
                animate(horizontal_bar_dot_amount_shown, 0.0, 130, ch->lifetime);
            }
        }
    }
    //notify("stop resizing");
}

int index_of_cid(int cid) {
    auto order = get_window_stacking_order();
    for (int i = 0; i < order.size(); i++) {
        if (order[i] == cid) {
            return i;
        }
    }
    return 10000;
}

int highest_in_group(int cid) {
    if (auto c = get_cid_container(cid)) {
        auto data = (ClientInfo *) c->user_data;
        struct Pair {
            int cid;
            int index;
        };
        std::vector<Pair> all;
        all.push_back({cid, index_of_cid(cid)});
        for (auto g : data->grouped_with) {
            all.push_back({g, index_of_cid(g)});
        }
        std::sort(all.begin(), all.end(), [](Pair a, Pair b) {
            return a.index > b.index;
        });
        return all[0].cid;
    }
    return cid;
}

void paint_resize_edge(Container *actual_root, Container *c) {
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    auto cid = *datum<int>(c, "cid");

    if (stage == (int) STAGE::RENDER_POST_WINDOW && active_id == highest_in_group(cid)) {
        renderfix
        
        auto b = c->real_bounds;
        b.shrink(resize_edge_size() * s);
        update_resize_edge_preview_bar(cid);
        
        auto vert_dot_amount = *datum<float>(c, "vertical_bar_dot_amount_shown");

        auto vert_amount = *datum<float>(c, "vertical_bar_amount_shown");
        auto rm = bounds_reserved_monitor(rid);
        if (vert_amount != 0.0) {
            auto pos = *datum<int>(c, "vertical_bar_position");
            auto bb = c->real_bounds;
            bb.shrink(resize_edge_size() * s);
            bb.w = 8 * s;
            bb.x -= bb.w * .5;
            bb.y = rm.y * s;
            bb.h = rm.h * s;
            if (pos == (int) RESIZE_TYPE::RIGHT) {
                bb.x += b.w;
            }
            rect(bb, {0, 0, 0, vert_amount});
            auto cc = bb;
            hypriso->damage_entire(rid);

            float h = 45 * s;
            float wa = .4;
            bb.x += bb.w * .5 - (bb.w * wa * .5);
            bb.w = bb.w * wa;
            bb.y += bb.h * .5;
            bb.y -= h * .5;
            bb.h = h;
            rect(bb, {.5, .5, .5, vert_amount * vert_dot_amount}, 0, 0.0, 2.0);
        }
    }
}

// mouse inside rounded rectangle (uniform radius)
bool mouse_inside_rounded(const Bounds& r, float mx, float my, float radius)
{
    // clamp mouse pos to the nearest point *inside* the rect's inner bounds
    // (the area excluding corner arcs)
    float innerLeft   = r.x + radius;
    float innerRight  = r.x + r.w - radius;
    float innerTop    = r.y + radius;
    float innerBottom = r.y + r.h - radius;

    // if mouse is inside the central box, no need to test circles
    if (mx >= innerLeft && mx <= innerRight)
        return true;
    if (my >= innerTop && my <= innerBottom)
        return true;

    // Otherwise: test distance to nearest corner circle center
    float cx = (mx < innerLeft)  ? innerLeft  : innerRight;
    float cy = (my < innerTop)   ? innerTop   : innerBottom;

    float dx = mx - cx;
    float dy = my - cy;
    return dx*dx + dy*dy <= radius * radius;
}

int get_current_resize_type(Container *c) {
    auto cid = *datum<int>(c, "cid");
    bool snapped = false;
    int snap_type = (int) SnapPosition::NONE;
    if (auto container = get_cid_container(cid)) {
        if (*datum<bool>(container, "snapped")) {
            snapped = true;
            snap_type = *datum<int>(container, "snap_type");
        }
    }

    auto box = bounds_client(cid);
    auto m = mouse();
    //box.shrink(resize_edge_size());
    int corner = 20;

    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;
    int resize_type = (int) RESIZE_TYPE::NONE;
    if (m.x < box.x + corner)
        left = true;
    if (m.x > box.x + box.w - corner)
        right = true;
    if (m.y < box.y + corner)
        top = true;
    if (m.y > box.y + box.h - corner)
        bottom = true;

    // get rid of those options that shouldn't happen if snapped
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

    return resize_type;
}

void create_resize_container_for_window(int id) {
    auto c = actual_root->child(FILL_SPACE, FILL_SPACE);
    c->custom_type = (int) TYPE::CLIENT_RESIZE;
    c->on_closed = [](Container *c) {
        // unset the resize if it's set?
        if (*datum<bool>(c, "setting_cursor")) {
            update_cursor((int) RESIZE_TYPE::NONE);
        }
    };
    *datum<int>(c, "cid") = id;
    *datum<int>(c, "resize_type") = (int) RESIZE_TYPE::NONE;
    *datum<bool>(c, "setting_cursor") = false;

    *datum<int>(c, "vertical_bar_position") = (int) RESIZE_TYPE::NONE;
    *datum<float>(c, "vertical_bar_amount_shown") = 0.0;
    *datum<float>(c, "vertical_bar_dot_amount_shown") = 0.0;
    *datum<int>(c, "horizontal_bar_position") = (int) RESIZE_TYPE::NONE;
    *datum<float>(c, "horizontal_bar_amount_shown") = 0.0;
    *datum<float>(c, "horizontal_bar_dot_amount_shown") = 0.0;

    c->when_paint = paint_resize_edge; 
    c->when_mouse_down = paint {
        hypriso->bring_to_front(*datum<int>(c, "cid"));  
        consume_event(root, c);
    };
    c->when_mouse_up = consume_event;
    c->when_mouse_enters_container = paint {
        setCursorImageUntilUnset("grabbing");

        auto resize_type = get_current_resize_type(c);

        *datum<int>(c, "resize_type") = resize_type;
        *datum<bool>(c, "setting_cursor") = true;

        update_cursor(resize_type);

        consume_event(root, c);
        damage_all();
    };
    c->when_mouse_motion = c->when_mouse_enters_container;
    c->when_mouse_leaves_container = paint {
        consume_event(root, c);
        update_cursor((int) RESIZE_TYPE::NONE);
        *datum<bool>(c, "setting_cursor") = false;
        damage_all();
    };
    c->when_clicked = paint {
        hypriso->bring_to_front(*datum<int>(c, "cid"));  
        consume_event(root, c);
    };
    c->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
       c->real_bounds.grow(resize_edge_size());
    };
    c->handles_pierced = [](Container* c, int x, int y) {
        auto b = c->real_bounds;
        b.shrink(resize_edge_size());
        auto cid = *datum<int>(c, "cid");
        // no resizing when snapped (for now)
        if (auto container = get_cid_container(cid)) {
            if (*datum<bool>(container, "snapped")) {
                //return false;
            }
        }
        float rounding = hypriso->get_rounding(cid);
       
        if (bounds_contains(c->real_bounds, x, y)) {
            if (bounds_contains(b, x, y)) {
                //unsetCursorImage();
                return false;
            }
            //notify("here");
            //setCursorImageUntilUnset("grabbing");
            return !hypriso->has_popup_at(cid, Bounds(x, y, x, y));
        }
        //unsetCursorImage();
        return false; 
    };
    c->when_drag_start = paint {
        int type = *datum<int>(c, "resize_type");
        int cid = *datum<int>(c, "cid");

        resizing::begin(cid, type);
    };
}

void remove_resize_container_for_window(int id) {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
        auto c = actual_root->children[i];
        auto cid = *datum<int>(c, "cid");
        if (cid == id && c->custom_type == (int) TYPE::CLIENT_RESIZE) {
            delete c;
            actual_root->children.erase(actual_root->children.begin() + i);
        }
    }
}

void resizing::on_window_open(int id) {
    create_resize_container_for_window(id);
}

void resizing::on_window_closed(int id) {
    remove_resize_container_for_window(id);
}

