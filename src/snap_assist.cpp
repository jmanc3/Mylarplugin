
#include "snap_assist.h"

#include "heart.h"
#include "events.h"

static float fade_in_time() {
    static float amount = 400;
    return hypriso->get_varfloat("plugin:mylardesktop:snap_helper_fade_in", amount);
}

void snap_assist::open(int monitor, int cid) {
    auto c = get_cid_container(cid);
    if (!c)
        return;

    auto type = (SnapPosition) *datum<int>(c, "snap_type");
    if (type == SnapPosition::MAX || type == SnapPosition::NONE)
        return;
    
    auto cdata = (ClientInfo *) c->user_data;

    std::vector<int> ids;
    ids.push_back(cid);
    for (auto grouped_id : cdata->grouped_with)
        ids.push_back(grouped_id);
    
    std::vector<SnapPosition> open_slots;
    
    if (type == SnapPosition::LEFT || type == SnapPosition::RIGHT) {
        if (type == SnapPosition::LEFT) {
            if (groupable(SnapPosition::RIGHT, ids)) {
                open_slots.push_back(SnapPosition::RIGHT);
            } else if (groupable(SnapPosition::TOP_RIGHT, ids)) {
                open_slots.push_back(SnapPosition::TOP_RIGHT);
            } else if (groupable(SnapPosition::BOTTOM_RIGHT, ids)) {
                open_slots.push_back(SnapPosition::BOTTOM_RIGHT);
            }
        } else {
            if (groupable(SnapPosition::LEFT, ids)) {
                open_slots.push_back(SnapPosition::LEFT);
            } else if (groupable(SnapPosition::TOP_LEFT, ids)) {
                open_slots.push_back(SnapPosition::TOP_LEFT);
            } else if (groupable(SnapPosition::BOTTOM_LEFT, ids)) {
                open_slots.push_back(SnapPosition::BOTTOM_LEFT);
            }            
        }
    } else {
        if (type == SnapPosition::TOP_LEFT) {
            for (auto pos : {SnapPosition::BOTTOM_LEFT, SnapPosition::BOTTOM_RIGHT, SnapPosition::TOP_RIGHT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::BOTTOM_LEFT) {
            for (auto pos : {SnapPosition::TOP_LEFT, SnapPosition::TOP_RIGHT, SnapPosition::BOTTOM_RIGHT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::BOTTOM_RIGHT) {
            for (auto pos : {SnapPosition::TOP_RIGHT, SnapPosition::TOP_LEFT, SnapPosition::BOTTOM_LEFT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::TOP_RIGHT) {
            for (auto pos : {SnapPosition::BOTTOM_RIGHT, SnapPosition::BOTTOM_LEFT, SnapPosition::TOP_LEFT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        }
    }

    // ==============================================
    // open containers
    // ==============================================

    long creation_time = get_current_time_in_ms();
    for (auto pos : open_slots) {
        auto snap_helper = actual_root->child(FILL_SPACE, FILL_SPACE);
        snap_helper->custom_type = (int) TYPE::SNAP_HELPER;
        //consume_everything(snap_helper);
        snap_helper->when_mouse_enters_container = paint {
            setCursorImageUntilUnset("default");
        };
        snap_helper->when_mouse_leaves_container = paint {
            unsetCursorImage(true);
        };
        snap_helper->when_mouse_down = paint {
            consume_event(root, c);
        };
        snap_helper->when_mouse_up = paint {
            consume_event(root, c);
        };
        snap_helper->when_mouse_motion = paint {
            consume_event(root, c);
        };

        snap_helper->pre_layout = [monitor, pos](Container *actual_root, Container *c, const Bounds &b) {
            auto s = scale(monitor);
            auto bounds = snap_position_to_bounds(monitor, pos);
            bounds.grow(-8 * s);
            c->wanted_bounds = bounds;
            c->real_bounds = bounds;
            ::layout(actual_root, c, c->real_bounds);
            hypriso->damage_entire(monitor);
        };

        snap_helper->when_paint = [cid, monitor, creation_time](Container *actual_root, Container *c) {
            auto root = get_rendering_root();
            if (!root) return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            if (rid != monitor)
                return;
            if (stage != (int) STAGE::RENDER_PRE_WINDOW)
                return;
            if (active_id != cid)
                return;
            renderfix

            float alpha = ((float) (get_current_time_in_ms() - creation_time)) / fade_in_time();
            if (alpha > 1.0)
                alpha = 1.0;

            render_drop_shadow(rid, 1.0, {0, 0, 0, .14f * alpha}, 8 * s, 2.0, c->real_bounds);
            rect(c->real_bounds, {1, 1, 1, .3f * alpha}, 0, 8 * s);
            auto b = c->real_bounds;
            b.shrink(std::round(1.0f * s));
            border(b, {1.0, 1.0, 1.0, 0.1f}, std::round(1.0f * s), 0, std::round(8 * s * alpha), 2.0f, false, 1.0);
        };
    }

    for (auto m : actual_monitors)
        hypriso->damage_entire(*datum<int>(m, "cid"));
}

void snap_assist::close() {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
       auto child = actual_root->children[i];
       if (child->custom_type == (int) TYPE::SNAP_HELPER) {
           delete child;
           actual_root->children.erase(actual_root->children.begin() + i);
       }
    }
    for (auto m : actual_monitors)
        hypriso->damage_entire(*datum<int>(m, "cid"));
}

void snap_assist::click(int id, int button, int state, float x, float y) {
    auto pierced = pierced_containers(actual_root, x, y);
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
       auto child = actual_root->children[i];
       if (child->custom_type == (int) TYPE::SNAP_HELPER) {
           for (auto p : pierced)
               if (p == child)
                   return;
       }
    }

    snap_assist::close();
}

