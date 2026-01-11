
#include "snap_assist.h"

#include "heart.h"
#include "events.h"
#include "layout_thumbnails.h"

struct HelperData : UserData {
    int cid = 0;
    long time_mouse_in = 0;
    long time_mouse_out = 0;
    long creation_time = 0;
};
        
static float fade_in_time() {
    static float amount = 400;
    return hypriso->get_varfloat("plugin:mylardesktop:snap_helper_fade_in", amount);
}

bool part_of_group(int o, int cid) {
    if (cid == o)
        return true;
    
    if (auto c = get_cid_container(cid)) {
        auto data = (ClientInfo *) c->user_data;
        for (auto other : data->grouped_with) {
            if (other == o) {
                return true;
            }
        }
    }

    return false;
}

struct SnapThumb : UserData {
    int cid = 0;
    long creation_time = 0;
};
    
void snap_helper_pre_layout(Container *actual_root, Container *c, const Bounds &b, int monitor, SnapPosition pos) {
    auto s = scale(monitor);
    auto bounds = snap_position_to_bounds(monitor, pos);
    bounds.grow(-8 * s);
    c->wanted_bounds = bounds;
    c->real_bounds = bounds;
    auto helper_data = (HelperData *) c->user_data;

    {
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
                if (hypriso->alt_tabbable(o) && !part_of_group(o, helper_data->cid)) {
                    later_immediate([o](Timer *) {
                        hypriso->set_hidden(o, true, false);
                    });
                    add.push_back(o);
                }
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
            data->creation_time = get_current_time_in_ms();
            thumb->user_data = data;
            later_immediate([](Timer *) {
                hypriso->screenshot_all();
            }); 
            thumb->when_paint = [](Container *actual_root, Container *c) {
                auto root = get_rendering_root();
                if (!root) return;
                auto [rid, s, stage, active_id] = roots_info(actual_root, root);
                renderfix
                auto parent_data = (HelperData *) c->parent->user_data;
                auto data = (SnapThumb *) c->user_data;
                auto parent_space = hypriso->get_workspace(parent_data->cid);
                auto our_space = hypriso->get_workspace(data->cid);
                float alpha = ((float) (get_current_time_in_ms() - parent_data->creation_time)) / 300.0f;
                if (alpha > 1.0)
                    alpha = 1.0;
                float fadea = ((float) (get_current_time_in_ms() - parent_data->creation_time)) / 550.0f;
                if (fadea > 1.0)
                    fadea = 1.0;

                auto size = hypriso->thumbnail_size(data->cid).scale(s);
                auto pos = bounds_client(data->cid);
                size.x = pos.x * s;
                size.y = pos.y * s;
                auto l = lerp(size, c->real_bounds, alpha * alpha * alpha * alpha);
                if (our_space != parent_space) {
                    l = c->real_bounds;
                }
                hypriso->clip = true;
                auto clipbox = c->parent->real_bounds;
                clipbox.scale(s);
                hypriso->clipbox = clipbox;
                if (our_space != parent_space) {
                    hypriso->draw_thumbnail(data->cid, l, 0, 2.0, 0, fadea * fadea);
                } else {
                    hypriso->draw_thumbnail(data->cid, l);
                }
                hypriso->clip = false;
                
                if (c->state.mouse_hovering) {
                    rect(c->real_bounds, {1, 1, 1, .1f}, 0, 0, 2.0, false);
                }
            };
        }
    }

    std::vector<Item> items;
    for (auto ch : c->children) {
        auto data = (SnapThumb *) ch->user_data;
        auto size = hypriso->thumbnail_size(data->cid);
        Item item;
        item.aspectRatio = size.w / size.h;
        items.push_back(item);
    }

    LayoutParams params {
        .availableWidth = (int) c->real_bounds.w,
        .availableHeight = (int) c->real_bounds.h,
        .horizontalSpacing = (int) (10 * s),
        .verticalSpacing = (int) (10 * s),
        .margin = (int) (10 * s),
        .densityPresets = {
            { 4, (int) (200 * s * .75) },
            { 9, (int) (166 * s * .75)},
            { 16, (int) (133 * s * .75) },
            { INT_MAX, (int) (100 * s * .75) }
        }
    };

    auto result = layoutAltTabThumbnails(params, items);
    for (int i = 0; i < c->children.size(); i++) {
        auto ch = c->children[i];
        ch->wanted_bounds = result.items[i];
        ch->wanted_bounds.x += bounds.x;
        ch->wanted_bounds.y += bounds.y;
        ch->real_bounds = result.items[i];
        ch->real_bounds.x += bounds.x;
        ch->real_bounds.y += bounds.y;
    }

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
        auto snap_helper = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
        auto helper_data = new HelperData;
        helper_data->cid = cid;
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
           for (auto ch : child->children) {
               auto data = (SnapThumb * ) ch->user_data;
               hypriso->set_hidden(data->cid, false, false);
           }
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

