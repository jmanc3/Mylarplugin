
#include "snap_assist.h"

#include "heart.h"
#include "events.h"

struct HelperData : UserData {
    long time_mouse_in = 0;
    long time_mouse_out = 0;
    long creation_time = 0;
};
        
static float fade_in_time() {
    static float amount = 400;
    return hypriso->get_varfloat("plugin:mylardesktop:snap_helper_fade_in", amount);
}

void snap_helper_pre_layout(Container *actual_root, Container *c, const Bounds &b, int monitor, SnapPosition pos) {
    auto s = scale(monitor);
    auto bounds = snap_position_to_bounds(monitor, pos);
    bounds.grow(-8 * s);
    c->wanted_bounds = bounds;
    c->real_bounds = bounds;

    {
        struct SnapThumb : UserData {
            int cid = 0;
        };

        auto order = get_window_stacking_order();

        std::vector<int> add;
        std::vector<int> remove;
        
        for (auto ch : c->children) {
            bool found = false;
            auto data = (SnapThumb *) ch->user_data;
            for (auto o : order)
                if (o == data->cid)
                    found = true;

            if (!found)
                remove.push_back(data->cid);
        }

        for (auto o : order) {
            bool found = false;
            for (auto ch : c->children) {
                auto data = (SnapThumb *) ch->user_data;
                if (o == data->cid)
                    found = true;
            }
            if (!found) {
                if (hypriso->alt_tabbable(o))
                    add.push_back(o);
            }
        }
        
        for (int i = c->children.size() - 1; i >= 0; i--) {
            auto ch = c->children[i];
            auto data = (SnapThumb *) ch->user_data;
            for (auto o : remove) {
                if (o == data->cid) {
                    delete ch;
                    c->children.erase(c->children.begin() + i);
                    break;
                }
            }
        }

        for (auto o : add) {
            auto thumb = c->child(40, 40);
            auto data = new SnapThumb;
            data->cid = o;
            thumb->user_data = data;
            thumb->when_paint = [](Container *actual_root, Container *c) {
                auto root = get_rendering_root();
                if (!root) return;
                auto [rid, s, stage, active_id] = roots_info(actual_root, root);
                renderfix
                auto data = (HelperData *) c->parent->user_data;
                float alpha = ((float) (get_current_time_in_ms() - data->creation_time)) / fade_in_time();
                if (alpha > 1.0)
                    alpha = 1.0;

                if (c->state.mouse_hovering) {
                    //rect(c->real_bounds, {1, 1, 1, 1 * alpha});
                }
                //border(c->real_bounds, {1, 0, 0, 1 * alpha}, 4);
            };
        }
    }

    ::layout(actual_root, c, c->real_bounds);
    hypriso->damage_entire(monitor);
}

void snap_assist::open(int monitor, int cid) {
    auto c = get_cid_container(cid);
    if (!c)
        return;

    auto type = (SnapPosition) *datum<int>(c, "snap_type");
    if (type == SnapPosition::MAX || type == SnapPosition::NONE)
        return;

    // collect those that shall be represented
    auto cdata = (ClientInfo *) c->user_data;
    std::vector<int> others;
    for (auto o : get_window_stacking_order()) {
        if (hypriso->alt_tabbable(o) && o != cid) {
            bool skip = false;
            for (auto grouped : cdata->grouped_with) {
                if (grouped == o)
                    skip = true;
            }
            if (!skip) {
                others.push_back(o);
            }
        }
    }
    if (others.empty())
        return;

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

    for (auto pos : open_slots) {
        auto snap_helper = actual_root->child(::vbox, FILL_SPACE, FILL_SPACE);
        auto helper_data = new HelperData;
        helper_data->creation_time = get_current_time_in_ms();
        snap_helper->user_data = helper_data; 
        snap_helper->custom_type = (int) TYPE::SNAP_HELPER;
        snap_helper->receive_events_even_if_obstructed = true;
        //consume_everything(snap_helper);
        snap_helper->when_mouse_enters_container = paint {
            auto data = (HelperData *) c->user_data;
            data->time_mouse_in = get_current_time_in_ms();
            setCursorImageUntilUnset("default");
        };
        snap_helper->when_mouse_leaves_container = paint {
            auto data = (HelperData *) c->user_data;
            data->time_mouse_out = get_current_time_in_ms();
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
        snap_helper->when_clicked = paint {
            later_immediate([](Timer *) {
                snap_assist::close();
            });
        };

        snap_helper->pre_layout = [monitor, pos](Container *actual_root, Container *c, const Bounds &b) {
            snap_helper_pre_layout(actual_root, c, b, monitor, pos);
        };

        snap_helper->when_paint = [cid, monitor](Container *actual_root, Container *c) {
            auto root = get_rendering_root();
            if (!root) return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            if (rid != monitor)
                return;
            if (stage != (int) STAGE::RENDER_PRE_WINDOW)
                return;
            if (active_id != cid)
                return;
            c->automatically_paint_children = true;
            renderfix

            auto data = (HelperData *) c->user_data;
            float alpha = ((float) (get_current_time_in_ms() - data->creation_time)) / fade_in_time();
            if (alpha > 1.0)
                alpha = 1.0;

            auto sha = c->real_bounds;
            render_drop_shadow(rid, 1.0, {0, 0, 0, .14f * alpha}, std::round(8 * s), 2.0, sha);
            rect(c->real_bounds, {1, 1, 1, .3f * alpha}, 0, std::round(8 * s), 2.0, true, 1.0 * alpha);
            if (c->state.mouse_hovering || c->state.mouse_pressing) {
                float alpha2 = ((float) (get_current_time_in_ms() - data->time_mouse_in)) / fade_in_time();
                if (alpha2 > 1.0)
                    alpha2 = 1.0;
                rect(c->real_bounds, {1, 1, 1, .3f * alpha2}, 0, std::round(8 * s), 2.0, false, 1.0);
            } else {
                float alpha2 = ((float) (get_current_time_in_ms() - data->time_mouse_out)) / fade_in_time();
                if (alpha2 > 1.0)
                    alpha2 = 1.0;
                rect(c->real_bounds, {1, 1, 1, .3f * (1.0f - alpha2)}, 0, std::round(8 * s), 2.0, false, 1.0); 
            }
            auto b = c->real_bounds;
            b.shrink(1.0f);
            border(b, {0.6, 0.6, 0.6, 0.5f * alpha}, 1.0f, 0, 8 * s, 2.0f, false, 1.0);
        };
        snap_helper->after_paint = paint {
            c->automatically_paint_children = false;
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

