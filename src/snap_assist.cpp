
#include "snap_assist.h"

#include "heart.h"
#include "events.h"
#include "drag.h"
#include "titlebar.h"
#include "icons.h"
#include "layout_thumbnails.h"

#include <algorithm>
#include <ios>

static bool skip_close = false;

struct HelperData : UserData {
    bool showing = false;
    int cid = 0;
    int monitor = 0;
    SnapPosition pos;
    long time_mouse_in = 0;
    long time_mouse_out = 0;
    long creation_time = 0;
};

static float fade_in_time() {
    static float amount = 400;
    return hypriso->get_varfloat("plugin:mylardesktop:snap_helper_fade_in", amount);
}

static RGBA color_titlebar_focused() {
    static RGBA default_color("ffffffff");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_focused_color", default_color);
}
static RGBA color_titlebar_unfocused() {
    static RGBA default_color("f0f0f0ff");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_unfocused_color", default_color);
}
static RGBA color_titlebar_text_focused() {
    static RGBA default_color("000000ff");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_focused_text_color", default_color);
}
static RGBA color_titlebar_text_unfocused() {
    static RGBA default_color("303030ff");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_unfocused_text_color", default_color);
}
static float titlebar_button_ratio() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_button_ratio", 1.4375f);
}
static float titlebar_text_h() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_text_h", 15);
}
static float titlebar_icon_h() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_icon_h", 21);
}
static float titlebar_button_icon_h() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_button_icon_h", 13);
}


void do_snap(int snap_mon, int cid, int pos, Bounds start_pos) {
    if (snap_mon == -1)
        return;
    auto c = get_cid_container(cid);
    if (!c) return;

    auto snapped = datum<bool>(c, "snapped");

    // perform snap
    *snapped = true;
    *datum<Bounds>(c, "pre_snap_bounds") = bounds_client(cid);
    *datum<int>(c, "snap_type") = pos;

    auto p = snap_position_to_bounds(snap_mon, (SnapPosition) pos);
    {
        auto s = start_pos;
        hypriso->move_resize(cid, s.x, s.y, s.w, s.h, true);
    }
    hypriso->set_hidden(cid, false);
    if (hypriso->has_decorations(cid)) {
        hypriso->move_resize(cid, p.x, p.y + titlebar_h, p.w, p.h - titlebar_h, false);
    } else {
        if (pos == (int) SnapPosition::BOTTOM_LEFT || pos == (int) SnapPosition::BOTTOM_RIGHT ||
            pos == (int) SnapPosition::LEFT || pos == (int) SnapPosition::RIGHT || pos == (int) SnapPosition::MAX) {
            hypriso->move_resize(cid, p.x, p.y, p.w, p.h, false);
        } else {
            hypriso->move_resize(cid, p.x, p.y, p.w, p.h + titlebar_h, false);
        }
    }
    hypriso->should_round(cid, false);
    skip_close = true;
    hypriso->bring_to_front(cid, true);
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
    
void snap_helper_pre_layout(Container *actual_root_m, Container *c, const Bounds &b, int monitor, SnapPosition pos) {
    auto s = scale(monitor);
    auto bounds = snap_position_to_bounds(monitor, pos);
    bounds.grow(-8 * s);
    c->wanted_bounds = bounds;
    c->real_bounds = bounds;
    auto helper_data = (HelperData *) c->user_data;

    {
        auto order = get_window_stacking_order();
        std::reverse(order.begin(), order.end());
        auto our_workspace = hypriso->get_active_workspace(monitor);
        std::stable_sort(order.begin(), order.end(),
            [our_workspace](int a, int b) {
                const bool a_on = hypriso->get_active_workspace_id_client(a) == our_workspace;
                const bool b_on = hypriso->get_active_workspace_id_client(b) == our_workspace;
                return b_on && !a_on;
            }
        );

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
            thumb->when_clicked = paint {
                auto parent_data = (HelperData *) c->parent->user_data;
                auto data = (SnapThumb *) c->user_data;
                auto b = c->real_bounds;
                auto s = scale(parent_data->monitor);
                b.y += titlebar_h * s;
                b.h -= titlebar_h * s;
                do_snap(parent_data->monitor, data->cid, (int) parent_data->pos, b);
                
                later_immediate([parent_data](Timer *) {
                    skip_close = false;
                    snap_assist::close();
                    /*
                    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
                       auto child = actual_root->children[i];
                       auto child_data = (HelperData *) child->user_data;
                       if (child->custom_type == (int) TYPE::SNAP_HELPER && parent_data == child_data && child_data->showing) {
                           delete child;
                           actual_root->children.erase(actual_root->children.begin() + i);
                       }
                    }
                    for (auto m : actual_monitors)
                        hypriso->damage_entire(*datum<int>(m, "cid"));
                    */
                });
            };
            thumb->when_paint = [](Container *actual_root, Container *c) {
                auto root = get_rendering_root();
                if (!root) return;
                auto [rid, s, stage, active_id] = roots_info(actual_root, root);
                renderfix
                auto parent_data = (HelperData *) c->parent->user_data;
                auto data = (SnapThumb *) c->user_data;
                auto cid = data->cid;
                auto parent_space = hypriso->get_workspace(parent_data->cid);
                auto our_space = hypriso->get_workspace(data->cid);
                float alpha = ((float) (get_current_time_in_ms() - parent_data->creation_time)) / 150.0f;
                if (alpha > 1.0)
                    alpha = 1.0;
                float fadea = ((float) (get_current_time_in_ms() - parent_data->creation_time)) / 270.0f;
                if (fadea > 1.0)
                    fadea = 1.0;

                auto size = hypriso->thumbnail_size(data->cid).scale(s);
                auto pos = bounds_client(data->cid);
                size.x = pos.x * s;
                size.y = pos.y * s;
                auto final_thumb_spot = c->real_bounds;
                final_thumb_spot.y += std::round(titlebar_h * s);
                final_thumb_spot.h -= std::round(titlebar_h * s);
                auto l = lerp(size, final_thumb_spot, alpha * alpha * alpha * alpha);
                if (our_space != parent_space) {
                    l = final_thumb_spot;
                }
                
                auto backup = c->real_bounds;
                defer(c->real_bounds = backup);
                c->real_bounds.h = std::round(titlebar_h * s);

                if (c->state.mouse_hovering) {
                    auto focused = color_titlebar_focused();
                    focused.a = fadea;
                    rect(c->real_bounds, focused, 12, 10 * s, 2.0f, false, fadea);
                } else {
                    auto unfocused = color_titlebar_unfocused();
                    unfocused.a = fadea;
                    rect(c->real_bounds, unfocused, 12, 10 * s, 2.0f, false, fadea);
                }

                int icon_width = 0; 
                { // load icon
                    TextureInfo *info = datum<TextureInfo>(c, std::to_string(rid) + "_icon");
                    auto real_icon_h = std::round(titlebar_icon_h() * s);
                    if (info->id != -1) {
                        if (info->cached_h != real_icon_h) {
                            free_text_texture(info->id);
                            info->id = -1;
                            info->reattempts_count = 0;
                            info->last_reattempt_time = 0;
                        }                
                    }
                    if (info->id == -1 && info->reattempts_count < 30) {
                        if (icons_loaded && enough_time_since_last_check(1000, info->last_reattempt_time)) {
                            info->last_reattempt_time = get_current_time_in_ms();
                            auto name = hypriso->class_name(cid);
                            auto path = one_shot_icon(real_icon_h, {
                                name, c3ic_fix_wm_class(name), to_lower(name), to_lower(c3ic_fix_wm_class(name))
                            });
                            if (!path.empty()) {
                                log(fz("{} {} {} ",path, real_icon_h, info->cached_h));

                                *info = gen_texture(path, real_icon_h);
                                info->cached_h = real_icon_h;
                            }
                        }
                    }
                    if (info->id != -1)
                        icon_width = info->w;
                    float focus_alpha = 1.0;
                    if (true) {
                        focus_alpha = color_titlebar_text_focused().a;
                    } else {
                        focus_alpha = color_titlebar_text_unfocused().a;
                    }
                    clip(to_parent(root, c), s);
                    draw_texture(*info, c->real_bounds.x + 8 * s, center_y(c, info->h), 1.0 * focus_alpha * fadea);
                }
                
                std::string title_text = hypriso->title_name(cid);
                if (!title_text.empty()) {
                    TextureInfo *focused = nullptr;
                    TextureInfo *unfocused = nullptr;
                    auto color_titlebar_textfo = color_titlebar_text_focused();
                    auto titlebar_text = titlebar_text_h();
                    auto color_titlebar_textunfo = color_titlebar_text_unfocused();
                    focused = get_cached_texture(root, c, std::to_string(rid) + "_title_focused", mylar_font, 
                        title_text, color_titlebar_textfo, titlebar_text);
                    unfocused = get_cached_texture(root, c, std::to_string(rid) + "_title_unfocused", mylar_font, 
                        title_text, color_titlebar_textunfo, titlebar_text);
                    
                    auto texture_info = focused;

                    if (texture_info->id != -1) {
                        auto overflow = std::max((c->real_bounds.h - texture_info->h), 0.0);
                        auto overflow_amount = std::max((c->real_bounds.h - texture_info->h), 0.0);
                        if (icon_width != 0)
                            overflow = icon_width + 16 * s;

                        auto clip_w = c->real_bounds.w - overflow - overflow_amount;
                        if (clip_w > 0) {
                            draw_texture(*texture_info, 
                                c->real_bounds.x + overflow, center_y(c, texture_info->h), fadea, clip_w);
                        }
                    }
                }

                if (alpha >= 1.0) {
                    hypriso->clip = true;
                    auto clipbox = c->parent->real_bounds;
                    clipbox.scale(s);
                    hypriso->clipbox = clipbox;
                }
                if (our_space != parent_space) {
                    hypriso->draw_thumbnail(data->cid, l, 10 * s, 2.0, 3, fadea * fadea);
                } else {
                    hypriso->draw_thumbnail(data->cid, l, 10 * s, 2.0, 3);
                }
                if (c->state.mouse_hovering) {
                    rect(final_thumb_spot, {1, 1, 1, .3}, 0, 10 * s, 2.0, false, 1.0);
                }
                hypriso->clip = false;
            };
        }
    }

    std::vector<Item> items;
    for (auto ch : c->children) {
        auto data = (SnapThumb *) ch->user_data;
        auto size = hypriso->thumbnail_size(data->cid);
        Item item;
        item.aspectRatio = size.w / (size.h + titlebar_h);
        items.push_back(item);
    }

    LayoutParams params {
        .availableWidth = (int) c->real_bounds.w,
        .availableHeight = (int) c->real_bounds.h,
        .horizontalSpacing = (int) (10 * s),
        .verticalSpacing = (int) (10 * s),
        .margin = (int) (10 * s),
        .maxThumbWidth = (int) (350 * s * .85),
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
        ch->z_index = c->children.size() + 10 - i;
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

    bool first = true;
    for (auto pos : open_slots) {
        auto snap_helper = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
        auto helper_data = new HelperData;
        helper_data->cid = cid;
        helper_data->monitor = monitor;
        helper_data->pos = pos;
        if (first) {
            first = false;
            helper_data->showing = true;
        }
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
                //snap_assist::close();
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
            auto data = (HelperData *) c->user_data;
            if (data->showing)
                c->automatically_paint_children = true;
            renderfix

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
    if (skip_close)
        return;
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

