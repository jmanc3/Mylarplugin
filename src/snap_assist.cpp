
#include "snap_assist.h"

#include "heart.h"
#include "events.h"
#include "titlebar.h"
#include "icons.h"
#include "layout_thumbnails.h"

#include <algorithm>

static bool skip_close = false;

struct HelperData : UserData {
    bool showing = false;
    int cid = 0;
    int lowest_group_cid = cid;
    int index = 0;
    int monitor = 0;
    SnapPosition pos;
    long time_mouse_in = 0;
    long time_mouse_out = 0;
    long creation_time = 0;

    float scroll_amount = 0;

    long slide_start;
    bool should_slide = false;
    SnapPosition came_from; // For slide
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
static RGBA titlebar_closed_button_bg_hovered_color() {
    static RGBA default_color("rgba(ff0000ff)");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_closed_button_bg_hovered_color", default_color);
}
static RGBA titlebar_closed_button_bg_pressed_color() {
    static RGBA default_color("rgba(0000ffff)");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_closed_button_bg_pressed_color", default_color);
}
static RGBA titlebar_closed_button_icon_color_hovered_pressed() {
    static RGBA default_color("rgba(999999ff)");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_closed_button_icon_color_hovered_pressed", default_color);
}

// {"anchors":[{"x":0,"y":1},{"x":0.47500000000000003,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.2911752162835537,"y":0.9622916751437718},{"x":0.6883506970527843,"y":0.08506946563720702}]}
static std::vector<float> slidetopos = { 0, 0.0030000000000000027, 0.006000000000000005, 0.01100000000000001, 0.016000000000000014, 0.02200000000000002, 0.030000000000000027, 0.038000000000000034, 0.04800000000000004, 0.05900000000000005, 0.07099999999999995, 0.08399999999999996, 0.09899999999999998, 0.11499999999999999, 0.132, 0.15100000000000002, 0.17200000000000004, 0.19399999999999995, 0.21799999999999997, 0.243, 0.271, 0.30000000000000004, 0.33199999999999996, 0.366, 0.402, 0.44099999999999995, 0.483, 0.527, 0.575, 0.612, 0.636, 0.6579999999999999, 0.6799999999999999, 0.7, 0.72, 0.738, 0.756, 0.773, 0.789, 0.8049999999999999, 0.8200000000000001, 0.834, 0.847, 0.86, 0.873, 0.884, 0.895, 0.906, 0.916, 0.925, 0.9339999999999999, 0.943, 0.951, 0.959, 0.966, 0.972, 0.979, 0.985, 0.99, 0.995, 1 };

// {"anchors":[{"x":0,"y":1},{"x":0.4,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.25099658672626207,"y":0.7409722222222223},{"x":0.6439499918619792,"y":0.007916683620876747}]}
static std::vector<float> slidetopos2 = { 0, 0.017000000000000015, 0.03500000000000003, 0.05400000000000005, 0.07199999999999995, 0.09199999999999997, 0.11099999999999999, 0.132, 0.15200000000000002, 0.17400000000000004, 0.19599999999999995, 0.21899999999999997, 0.242, 0.266, 0.29100000000000004, 0.31699999999999995, 0.344, 0.372, 0.4, 0.43000000000000005, 0.46099999999999997, 0.494, 0.527, 0.563, 0.6, 0.626, 0.651, 0.675, 0.6970000000000001, 0.719, 0.739, 0.758, 0.777, 0.794, 0.8109999999999999, 0.8260000000000001, 0.841, 0.855, 0.868, 0.881, 0.892, 0.903, 0.914, 0.923, 0.9319999999999999, 0.9410000000000001, 0.948, 0.955, 0.962, 0.968, 0.973, 0.978, 0.983, 0.986, 0.99, 0.993, 0.995, 0.997, 0.998, 0.999, 1 };

// {"anchors":[{"x":0,"y":1},{"x":1,"y":0}],"controls":[{"x":0.4484324841621595,"y":0.10111109415690105}]}
static std::vector<float> slidetopos2fade = { 0, 0.03300000000000003, 0.06499999999999995, 0.09699999999999998, 0.128, 0.15900000000000003, 0.18799999999999994, 0.21699999999999997, 0.246, 0.274, 0.30100000000000005, 0.32699999999999996, 0.353, 0.379, 0.404, 0.42800000000000005, 0.45099999999999996, 0.474, 0.497, 0.519, 0.54, 0.5609999999999999, 0.581, 0.601, 0.62, 0.639, 0.657, 0.675, 0.692, 0.708, 0.725, 0.74, 0.755, 0.77, 0.784, 0.798, 0.8109999999999999, 0.8240000000000001, 0.836, 0.848, 0.86, 0.871, 0.881, 0.891, 0.901, 0.91, 0.919, 0.928, 0.9359999999999999, 0.943, 0.95, 0.957, 0.963, 0.969, 0.975, 0.98, 0.985, 0.989, 0.993, 0.997 };

void do_snap(int snap_mon, int cid, int pos, Bounds start_pos) {
    if (snap_mon == -1)
        return;
    auto c = get_cid_container(cid);
    if (!c) return;

    auto snapped = datum<bool>(c, "snapped");
    if (!*snapped)
        *datum<Bounds>(c, "pre_snap_bounds") = bounds_client(cid);

    *snapped = true;
    *datum<int>(c, "snap_type") = pos;

    auto p = snap_position_to_bounds(snap_mon, (SnapPosition) pos);
    {
        auto s = start_pos;
        hypriso->move_resize(cid, s.x, s.y, s.w, s.h, true);
    }
    if (hypriso->get_active_workspace_id(snap_mon) != hypriso->get_active_workspace_id_client(cid)) {
        hypriso->move_to_workspace(cid, hypriso->get_active_workspace(snap_mon));
    }
    hypriso->render_whitelist.push_back(cid);
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

void remove_all_of_cid(int close_cid) {
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
       auto child = actual_root->children[i];
       if (child->custom_type == (int) TYPE::SNAP_HELPER) {
           for (int snap_i = child->children.size() - 1; snap_i >= 0; snap_i--) {
               auto snap = child->children[snap_i];
               auto snap_data = (SnapThumb *) snap->user_data;
               if (snap_data->cid == close_cid) {
                   delete snap;
                   child->children.erase(child->children.begin() + snap_i);
               }
           }
       }
    }    
}

void possibly_close_if_none_left() {
    bool any_seen = false;
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
       auto child = actual_root->children[i];
       if (child->custom_type == (int) TYPE::SNAP_HELPER) {
           if (!child->children.empty()) {
               any_seen = true;
           }
       }
    }
    if (!any_seen) {
        snap_assist::close();
    }
}
    
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

        //std::reverse(add.begin(), add.end());

        for (auto o : add) {
            auto thumb = c->child(::absolute, 40, 40);
            auto data = new SnapThumb;
            data->cid = o;
            data->creation_time = get_current_time_in_ms();
            thumb->user_data = data;
            thumb->when_clicked = paint {
                auto parent_data = (HelperData *) c->parent->user_data;
                auto data = (SnapThumb *) c->user_data;
                auto close_cid = data->cid;
                auto b = c->real_bounds;
                auto s = scale(parent_data->monitor);
                b.y += titlebar_h * s;
                b.h -= titlebar_h * s;
                skip_close = true;
                do_snap(parent_data->monitor, data->cid, (int) parent_data->pos, b);
                auto other_cdata = (ClientInfo *) get_cid_container(parent_data->cid)->user_data;
                add_to_snap_group(data->cid, parent_data->cid, other_cdata->grouped_with);

                later_immediate([parent_data, close_cid](Timer *) {
                    skip_close = false;
                    auto came_from = SnapPosition::NONE;
                    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
                       auto child = actual_root->children[i];
                       auto child_data = (HelperData *) child->user_data;
                       if (child->custom_type == (int) TYPE::SNAP_HELPER && parent_data == child_data && child_data->showing) {
                           came_from = child_data->pos;
                           delete child;
                           actual_root->children.erase(actual_root->children.begin() + i);
                       }
                    }

                    remove_all_of_cid(close_cid);

                    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
                       auto child = actual_root->children[i];
                       auto child_data = (HelperData *) child->user_data;
                       if (child->custom_type == (int) TYPE::SNAP_HELPER) {
                           child_data->showing = true;
                           child_data->came_from = came_from;
                           child->interactable = true;
                           child_data->slide_start = get_current_time_in_ms();
                           child_data->should_slide = true;
                           break;
                       }
                    }
                    possibly_close_if_none_left();
                    for (auto m : actual_monitors)
                        hypriso->damage_entire(*datum<int>(m, "cid"));
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
                auto ratioscalar = .53;
                if (parent_data->pos != SnapPosition::LEFT && parent_data->pos != SnapPosition::RIGHT) {
                    ratioscalar *= 1.15;
                }
                float alpha = ((float) (get_current_time_in_ms() - parent_data->creation_time)) / (450.0f * ratioscalar);
                if (alpha > 1.0)
                    alpha = 1.0;
                float fadea = alpha;
                if (hypriso->has_decorations(cid))
                    fadea = 1.0;
                if (parent_data->should_slide) {
                    fadea = ((float) (get_current_time_in_ms() - parent_data->slide_start)) / (650.0f * ratioscalar);
                    if (fadea > 1.0)
                        fadea = 1.0;
                    fadea = pull(slidetopos2fade, fadea);
                }

                auto size = hypriso->thumbnail_size(data->cid).scale(s);
                auto pos = bounds_client(data->cid);
                size.x = pos.x * s;
                size.y = pos.y * s;
                auto final_thumb_spot = c->real_bounds;
                final_thumb_spot.y += std::round(titlebar_h * s);
                final_thumb_spot.h -= std::round(titlebar_h * s);
                auto l = lerp(size, final_thumb_spot, pull(slidetopos, alpha));
                if (our_space != parent_space) {
                    l = final_thumb_spot;
                }
                if (parent_data->should_slide) {
                    l = final_thumb_spot;
                    float alpha2 = ((float) (get_current_time_in_ms() - parent_data->slide_start)) / (550.0 * ratioscalar);
                    if (alpha2 > 1.0)
                        alpha2 = 1.0;

                    bool from_right = false;
                    bool from_left = false;
                    bool from_top = false;
                    bool from_bottom = false;

                    switch (parent_data->came_from) {
                        case SnapPosition::BOTTOM_LEFT: {
                            if (parent_data->pos == SnapPosition::BOTTOM_LEFT) {
                            } else if (parent_data->pos == SnapPosition::BOTTOM_RIGHT) {
                                from_left = true;
                            } else if (parent_data->pos == SnapPosition::TOP_LEFT) {
                                from_bottom = true;
                            } else if (parent_data->pos == SnapPosition::TOP_RIGHT) {
                                from_bottom = true;
                                from_left = true;
                            }
                            break;
                        }
                        case SnapPosition::BOTTOM_RIGHT: {
                            if (parent_data->pos == SnapPosition::BOTTOM_LEFT) {
                                from_right = true;
                            } else if (parent_data->pos == SnapPosition::BOTTOM_RIGHT) {
                            } else if (parent_data->pos == SnapPosition::TOP_LEFT) {
                                from_right = true;
                                from_bottom = true;
                            } else if (parent_data->pos == SnapPosition::TOP_RIGHT) {
                                from_bottom = true;
                            }
                            break;
                        }
                        case SnapPosition::TOP_LEFT: {
                            if (parent_data->pos == SnapPosition::BOTTOM_LEFT) {
                                from_top = true;
                            } else if (parent_data->pos == SnapPosition::BOTTOM_RIGHT) {
                                from_top = true;
                                from_left = true;
                            } else if (parent_data->pos == SnapPosition::TOP_LEFT) {
                            } else if (parent_data->pos == SnapPosition::TOP_RIGHT) {
                                from_left = true;
                            }
                            break;
                        }
                        case SnapPosition::TOP_RIGHT: {
                            if (parent_data->pos == SnapPosition::BOTTOM_LEFT) {
                                from_top = true;
                                from_right = true;
                            } else if (parent_data->pos == SnapPosition::BOTTOM_RIGHT) {
                                from_top = true;
                            } else if (parent_data->pos == SnapPosition::TOP_LEFT) {
                                from_right = true;
                            } else if (parent_data->pos == SnapPosition::TOP_RIGHT) {
                            }
                            break;
                        }
                    }
                    
                    float slide_amount = 150 * s;
                    if (from_right)
                        l.x += (slide_amount) * (1.0 - pull(slidetopos2, alpha2));
                    if (from_left)
                        l.x -= (slide_amount) * (1.0 - pull(slidetopos2, alpha2));
                    if (from_bottom)
                        l.y += (slide_amount) * (1.0 - pull(slidetopos2, alpha2));
                    if (from_top)
                        l.y -= (slide_amount) * (1.0 - pull(slidetopos2, alpha2));
                }
                auto full = l;
                full.y -= titlebar_h * s;
                full.h += titlebar_h * s;
                {
                    render_drop_shadow(rid, 1.0, {0, 0, 0, .15f * fadea}, 10 * s, 2.0, full);
                }
                
                auto backup = c->real_bounds;
                defer(c->real_bounds = backup);
                c->real_bounds.h = std::round(titlebar_h * s);
                c->real_bounds = l;
                c->real_bounds.y -= std::round(titlebar_h * s);
                c->real_bounds.h = std::round(titlebar_h * s) + 1;

                {
                    int titlebar_mask = 12;
                    Bounds titlebar_bounds = c->real_bounds;
                    bool child_hovered = false;
                    if (!c->children.empty()) {
                        if (c->children[0]->state.mouse_hovering || c->children[0]->state.mouse_pressing) {
                            titlebar_bounds.w -= titlebar_h * s * .5;
                            child_hovered = true;
                        }
                    }
                    if (c->state.mouse_hovering || child_hovered) {
                        auto focused = color_titlebar_focused();
                        focused.a = fadea;
                        rect(titlebar_bounds, focused, titlebar_mask, 10 * s, 2.0f, false, pull(slidetopos, fadea));
                    } else {
                        auto unfocused = color_titlebar_unfocused();
                        unfocused.a = fadea;
                        rect(titlebar_bounds, unfocused, titlebar_mask, 10 * s, 2.0f, false, pull(slidetopos, fadea));
                    }
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
                    draw_texture(*info, c->real_bounds.x + 8 * s, center_y(c, info->h), pull(slidetopos, fadea));
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
                                c->real_bounds.x + overflow, center_y(c, texture_info->h), pull(slidetopos, fadea), clip_w);
                        }
                    }
                }

                if (alpha >= 1.0) {
                    hypriso->clip = true;
                    auto clipbox = c->parent->real_bounds;
                    clipbox.scale(s);
                    hypriso->clipbox = clipbox;
                }
                if (parent_data->should_slide) {
                    hypriso->draw_thumbnail(data->cid, l, 10 * s, 2.0, 3, fadea);
                } else if (our_space != parent_space) {
                    hypriso->draw_thumbnail(data->cid, l, 10 * s, 2.0, 3, pull(slidetopos, fadea));
                } else {
                    hypriso->draw_thumbnail(data->cid, l, 10 * s, 2.0, 3);
                }
                if (c->state.mouse_hovering) {
                    //rect(final_thumb_spot, {1, 1, 1, .3}, 3, 10 * s, 2.0, false, 1.0);
                }
                hypriso->clip = false;
            };

            auto close = thumb->child(FILL_SPACE, FILL_SPACE);
            close->when_paint = [](Container *actual_root, Container *c) {
                auto root = get_rendering_root();
                if (!root) return;
                auto [rid, s, stage, active_id] = roots_info(actual_root, root);
                renderfix

                c->real_bounds.h += 1;
                c->real_bounds.round();

                if (c->state.mouse_pressing) {
                    rect(c->real_bounds, titlebar_closed_button_bg_pressed_color(), 13, 10 * s, 2.0);
                } else if (c->state.mouse_hovering) {
                    rect(c->real_bounds, titlebar_closed_button_bg_hovered_color(), 13, 10 * s, 2.0);
                } else if (c->parent->state.mouse_hovering) {
                    rect(c->real_bounds, color_titlebar_focused(), 13, 10 * s, 2.0);
                }

                auto icon = "\ue8bb";
                auto closed = get_cached_texture(root, root, "close_close_invariant", "Segoe Fluent Icons", 
                    icon, titlebar_closed_button_icon_color_hovered_pressed(), titlebar_button_icon_h());

                if (c->state.mouse_pressing || c->state.mouse_hovering || c->parent->state.mouse_hovering) {
                    auto texture_info = closed;
                    if (texture_info->id != -1) {
                        clip(to_parent(root, c), s);
                        draw_texture(*texture_info, center_x(c, texture_info->w), center_y(c, texture_info->h), 1.0);
                    }
                }
            };
            close->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
                auto root = get_rendering_root();
                if (!root) return;
                auto [rid, s, stage, active_id] = roots_info(actual_root, root);
                
                auto w = titlebar_h * titlebar_button_ratio();
                c->wanted_bounds = {b.x + b.w - w, b.y, w, (float) titlebar_h};
                c->real_bounds = c->wanted_bounds;
            };
            auto cid_copy = data->cid;
            close->when_clicked = [cid_copy](Container *root, Container *c) {
                close_window(cid_copy);
                auto close_cid = cid_copy;
                later(10, [close_cid](Timer *) { 
                    remove_all_of_cid(close_cid);
                    possibly_close_if_none_left(); 
                });
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
    auto scroll_amount = helper_data->scroll_amount;
    if (scroll_amount < 0)
        scroll_amount = 0;
    if (scroll_amount > result.bounds.h) {
        scroll_amount = result.bounds.h;
    }

    for (int i = 0; i < c->children.size(); i++) {
        auto ch = c->children[i];
        ch->z_index = c->children.size() + 10 - i;
        ch->wanted_bounds = result.items[i];
        ch->wanted_bounds.x += bounds.x;
        ch->wanted_bounds.y += bounds.y + scroll_amount;
        ch->real_bounds = result.items[i];
        ch->real_bounds.x += bounds.x;
        ch->real_bounds.y += bounds.y + scroll_amount;
        ::layout(actual_root, ch, ch->real_bounds);
    }

    hypriso->damage_entire(monitor);
}

void actual_open(int monitor, int cid) {
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
            auto top_open = groupable(SnapPosition::TOP_RIGHT, ids);
            auto bottom_open = groupable(SnapPosition::BOTTOM_RIGHT, ids);
            if (groupable(SnapPosition::RIGHT, ids) && (top_open && bottom_open)) {
                open_slots.push_back(SnapPosition::RIGHT);
            } else if (top_open) {
                open_slots.push_back(SnapPosition::TOP_RIGHT);
            } else if (bottom_open) {
                open_slots.push_back(SnapPosition::BOTTOM_RIGHT);
            }
        } else {
            auto top_open = groupable(SnapPosition::TOP_LEFT, ids);
            auto bottom_open = groupable(SnapPosition::BOTTOM_LEFT, ids);
            if (groupable(SnapPosition::LEFT, ids) && (top_open && bottom_open)) {
                open_slots.push_back(SnapPosition::LEFT);
            } else if (top_open) {
                open_slots.push_back(SnapPosition::TOP_LEFT);
            } else if (bottom_open) {
                open_slots.push_back(SnapPosition::BOTTOM_LEFT);
            }            
        }
    } else {
        if (type == SnapPosition::TOP_LEFT) {
            for (auto pos : {SnapPosition::BOTTOM_LEFT, SnapPosition::TOP_RIGHT, SnapPosition::BOTTOM_RIGHT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::BOTTOM_LEFT) {
            for (auto pos : {SnapPosition::TOP_LEFT, SnapPosition::BOTTOM_RIGHT, SnapPosition::TOP_RIGHT, })
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::BOTTOM_RIGHT) {
            for (auto pos : {SnapPosition::TOP_RIGHT, SnapPosition::BOTTOM_LEFT, SnapPosition::TOP_LEFT,})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::TOP_RIGHT) {
            for (auto pos : {SnapPosition::BOTTOM_RIGHT, SnapPosition::TOP_LEFT, SnapPosition::BOTTOM_LEFT, })
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        }
    }

    // ==============================================
    // open containers
    // ==============================================

    for (int i = 0; i < open_slots.size(); i++) {
        auto pos = open_slots[i];
        auto snap_helper = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
        auto helper_data = new HelperData;
        helper_data->index = i;
        helper_data->cid = cid;
        helper_data->monitor = monitor;
        helper_data->pos = pos;
        helper_data->lowest_group_cid = helper_data->cid;
        auto cid_data = (ClientInfo *) get_cid_container(cid)->user_data;
        for (auto actual_cid : get_window_stacking_order()) {
            for (auto g : cid_data->grouped_with) {
                if (g == actual_cid) {
                    helper_data->lowest_group_cid = actual_cid;
                    goto done;
                }
            }
            if (actual_cid == cid) {
                helper_data->lowest_group_cid = cid;
                goto done;
            }
        }
        done:
        if (i == 0) {
            helper_data->showing = true;
            snap_helper->interactable = true;
        } else {
            snap_helper->interactable = false;
        }
        helper_data->creation_time = get_current_time_in_ms();
        snap_helper->user_data = helper_data; 
        snap_helper->custom_type = (int) TYPE::SNAP_HELPER;
        snap_helper->receive_events_even_if_obstructed = true;
        *datum<bool>(snap_helper, "setting_cursor") = false;
        snap_helper->on_closed = [](Container *c) {
            // unset the resize if it's set?
            if (*datum<bool>(c, "setting_cursor")) {
                unsetCursorImage(true);
            }
        };
        snap_helper->when_mouse_enters_container = paint {
            auto data = (HelperData *) c->user_data;
            data->time_mouse_in = get_current_time_in_ms();
            setCursorImageUntilUnset("default");
            *datum<bool>(c, "setting_cursor") = true;
        };
        snap_helper->when_mouse_leaves_container = paint {
            auto data = (HelperData *) c->user_data;
            data->time_mouse_out = get_current_time_in_ms();
            unsetCursorImage(true);
            *datum<bool>(c, "setting_cursor") = false;
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
        snap_helper->when_fine_scrolled = [](Container* root, Container* c, int scroll_x, int scroll_y, bool came_from_touchpad) {
            auto data = (HelperData *) c->user_data;
            data->scroll_amount += scroll_y;
        };

        snap_helper->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
            auto data = (HelperData *) c->user_data;
            snap_helper_pre_layout(actual_root, c, b, data->monitor, data->pos);
        };

        snap_helper->when_paint = [](Container *actual_root, Container *c) {
            auto data = (HelperData *) c->user_data;
            auto root = get_rendering_root();
            if (!root) return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            if (rid != data->monitor)
                return;
            if (stage != (int) STAGE::RENDER_PRE_WINDOW)
                return;
            if (active_id != data->lowest_group_cid)
                return;
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
                //rect(c->real_bounds, {1, 1, 1, .3f * alpha2}, 0, std::round(8 * s), 2.0, false, 1.0);
            } else {
                float alpha2 = ((float) (get_current_time_in_ms() - data->time_mouse_out)) / fade_in_time();
                if (alpha2 > 1.0)
                    alpha2 = 1.0;
                //rect(c->real_bounds, {1, 1, 1, .3f * (1.0f - alpha2)}, 0, std::round(8 * s), 2.0, false, 1.0); 
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

    for (auto i : ids)
        hypriso->render_whitelist.push_back(i);
}

void snap_assist::open(int monitor, int cid) {
    later_immediate([monitor, cid](Timer *) {
        hypriso->screenshot_all(); 
        actual_open(monitor, cid);
    });
}

void snap_assist::close() {
    if (skip_close)
        return;
    for (int i = actual_root->children.size() - 1; i >= 0; i--) {
       auto child = actual_root->children[i];
       if (child->custom_type == (int) TYPE::SNAP_HELPER) {
           for (auto ch : child->children) {
               auto data = (SnapThumb * ) ch->user_data;
               //hypriso->set_hidden(data->cid, false, false);
           }
           delete child;
           actual_root->children.erase(actual_root->children.begin() + i);
       }
    }
    hypriso->render_whitelist.clear();
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

void snap_assist::fix_order() {
    struct TempLocation {
        Container* c;
        int target_index;
        bool set = false;
    };

    TempLocation current;
    TempLocation last;

    // Collect helpers
    for (int i = 0; i < (int)actual_root->children.size(); i++) {
        Container* child = actual_root->children[i];
        if (child->custom_type == (int)TYPE::SNAP_HELPER) {
            auto* helper_data = (HelperData*)child->user_data;
            last.set = true;
            last.c = child;
            last.target_index = i;
            if (helper_data->showing) {
                current.set = true;
                current.c = child;
                current.target_index = i;
            }
        }
    }

    if (last.set && current.set) {
        if (last.target_index != current.target_index) {
            std::swap(last.c, current.c);
        }
    }
}

