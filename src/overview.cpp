#include "overview.h"

#include "heart.h"
#include "hypriso.h"
#include "icons.h"
#include "titlebar.h"
#include "layout_thumbnails.h"

#include <climits>
#include <math.h>
#include <system_error>

struct OverviewData : UserData {
    std::vector<int> order;
    float scalar = 0.0;
    int clicked_cid = -1;
};

struct ThumbData : UserData {
    int cid;
};

static void animate(float *value, float target, float time_ms, std::shared_ptr<bool> lifetime, std::function<void(bool)> on_completion = nullptr);

static bool running = false;
static float overview_anim_time = 215.0f;

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

// {"anchors":[{"x":0,"y":1},{"x":0.4,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.25099658672626207,"y":0.7409722222222223},{"x":0.6439499918619792,"y":0.007916683620876747}]}
static std::vector<float> slidetopos2 = { 0, 0.017000000000000015, 0.03500000000000003, 0.05400000000000005, 0.07199999999999995, 0.09199999999999997, 0.11099999999999999, 0.132, 0.15200000000000002, 0.17400000000000004, 0.19599999999999995, 0.21899999999999997, 0.242, 0.266, 0.29100000000000004, 0.31699999999999995, 0.344, 0.372, 0.4, 0.43000000000000005, 0.46099999999999997, 0.494, 0.527, 0.563, 0.6, 0.626, 0.651, 0.675, 0.6970000000000001, 0.719, 0.739, 0.758, 0.777, 0.794, 0.8109999999999999, 0.8260000000000001, 0.841, 0.855, 0.868, 0.881, 0.892, 0.903, 0.914, 0.923, 0.9319999999999999, 0.9410000000000001, 0.948, 0.955, 0.962, 0.968, 0.973, 0.978, 0.983, 0.986, 0.99, 0.993, 0.995, 0.997, 0.998, 0.999, 1 };

void screenshot_loop() {
    running = true;
    later(200.0f, [](Timer *t) { 
        t->keep_running = running;
        t->delay = 1000.0f / 60.0f;
        auto order = get_window_stacking_order();
        std::reverse(order.begin(), order.end());
        int amount = 0;
        for (auto o : order) {
            if (hypriso->alt_tabbable(o)) {
                hypriso->screenshot(o); 
                if (amount++ > 2) {
                    break;
                }
            }
        }
    });
}

static bool screenshotting_wallpaper = false;

void fadeout_docks(Container *actual_root, Container *c, int monitor, long creation_time) {
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_POST_WALLPAPER || monitor != rid)
        return;
    renderfix
    auto overview_data = (OverviewData *) c->user_data;
    auto scalar = overview_data->scalar;
    scalar = pull(slidetopos2, scalar);
    
    auto m = bounds_monitor(monitor);
    m.scale(s);
    auto rawmon = m;
    //rect(rawmon, {.14, .14, .14, 1 * scalar}, 0, 0, 2.0, false);
    hypriso->draw_wallpaper(monitor, m);
    rect(m, {0, 0, 0, .4f}, 0, 0, 2.0, true);
    //rect(m, {0, 0, 0, .5}, 0, 0, 2.0, true);
}

void paint_over_wallpaper(Container *actual_root, Container *c, int monitor, long creation_time) {
    if (screenshotting_wallpaper)
        return;
    fadeout_docks(actual_root, c, monitor, creation_time);
    
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_POST_WINDOWS || monitor != rid)
        return;
    renderfix
    auto m = bounds_monitor(monitor);
    m.scale(s);
    auto rawmon = m;

    auto overview_data = (OverviewData *) c->user_data;
    auto scalar = overview_data->scalar;
    scalar = pull(slidetopos2, scalar);
   
    float padamount = .17;
    float padx = m.w * padamount;
    float pady = m.h * padamount;
    m.x += padx * .5;
    m.w -= padx;
    m.y += pady * .5;
    m.h -= pady;
    m = lerp(rawmon, m, scalar);
    hypriso->draw_wallpaper(monitor, m, 14 * s * scalar);
    auto b = m;
    render_drop_shadow(rid, 1, {.1, .1, .1, .33f * scalar}, 14 * s * scalar, 2.0, b, 50 * s);
    b.shrink(2);
    border(b, {1, 1, 1, .05}, 1, 0, 14 * s); 
}

static void paint_option(Container *actual_root, Container *c, int monitor, long creation_time) {
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    if (stage != (int) STAGE::RENDER_POST_WINDOWS || monitor != rid)
        return;
    renderfix

    auto overview_data = (OverviewData *) c->parent->user_data;
    auto scalar = overview_data->scalar;
    scalar = pull(slidetopos2, scalar);
    
    auto cid = *datum<int>(c, "cid");
    auto backup = c->real_bounds;

    {
        bool reposition_by_mouse = false;
        float initial_x;
        float initial_y;
        float current_x;
        float current_y;
        auto snap_back_scalar = *datum<float>(c, "snap_back_scalar");
        initial_x = *datum<float>(c, "snap_back_initial_x");
        initial_y = *datum<float>(c, "snap_back_initial_y");
        current_x = *datum<float>(c, "snap_back_current_x");
        current_y = *datum<float>(c, "snap_back_current_y");
        if (snap_back_scalar != 1.0 && snap_back_scalar != 0.0) {
            reposition_by_mouse = true;
            //snap_back_scalar = pull(slidetopos2, snap_back_scalar);
        }
        if ((c->state.mouse_dragging && !c->children[0]->state.mouse_hovering)) {
            reposition_by_mouse = true;
            initial_x = actual_root->mouse_initial_x;
            initial_y = actual_root->mouse_initial_y;
            current_x = actual_root->mouse_current_x;
            current_y = actual_root->mouse_current_y;
            snap_back_scalar = 1.0;
        }
        if (reposition_by_mouse) {
            c->z_index = 100;
            c->real_bounds.x += ((current_x - initial_x) * s) * (scalar * snap_back_scalar);
            c->real_bounds.y += ((current_y - initial_y) * s) * (scalar * snap_back_scalar);
        } else if (c->z_index != 1000) {
            c->z_index = 0;
        }
    }
    
    static float roundingAmt = 8;
    render_drop_shadow(monitor, 1.0, {0, 0, 0, .1}, roundingAmt * s, 2.0, c->real_bounds, 3 * s);
    auto th = titlebar_h;
    titlebar_h = ((float) titlebar_h) * (1.0 - (.2 * scalar));
    defer(titlebar_h = th);

    auto pre_title_backup = c->real_bounds;
    {
        defer(c->real_bounds = pre_title_backup);
        
        c->real_bounds.h = std::round(titlebar_h * s);
        int titlebar_mask = 12;
        Bounds titlebar_bounds = c->real_bounds;
        bool child_hovered = false;
        if (!c->children.empty()) {
            if (c->children[0]->state.mouse_hovering || c->children[0]->state.mouse_pressing) {
                titlebar_mask = 14;
                titlebar_bounds.w -= std::round(titlebar_bounds.h * titlebar_button_ratio());
                child_hovered = true;
            }
        }
        float fadea = scalar;
        if (hypriso->has_decorations(cid)) {
            fadea = 1.0f;
        }
        
        auto focused = color_titlebar_focused();
        focused.a = fadea;
        rect(titlebar_bounds, focused, titlebar_mask, roundingAmt * s, 2.0f, false);

        int icon_width = 0; 
        { // load icon
            TextureInfo *info = datum<TextureInfo>(c, std::to_string(rid) + "_icon");
            auto real_icon_h = std::round(titlebar_icon_h() * s * .9);
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
            draw_texture(*info, c->real_bounds.x + 8 * s, center_y(c, info->h), pull(slidetopos2, fadea));
        }

        std::string title_text = hypriso->title_name(cid);
        if (!title_text.empty()) {
            TextureInfo *focused = nullptr;
            TextureInfo *unfocused = nullptr;
            auto color_titlebar_textfo = color_titlebar_text_focused();
            auto titlebar_text = titlebar_text_h() * .9;
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
                        c->real_bounds.x + overflow, center_y(c, texture_info->h), pull(slidetopos2, fadea), clip_w);
                }
            }
        }

        if (!c->children.empty()) {
            auto ch = c->children[0];
            auto close_bounds = c->real_bounds;
            auto bw = std::round(close_bounds.h * titlebar_button_ratio());
            close_bounds.x += close_bounds.w - bw; 
            close_bounds.w = bw;
            if (ch->state.mouse_pressing) {
                rect(close_bounds, titlebar_closed_button_bg_pressed_color(), 13, roundingAmt * s, 2.0, false);
            } else if (ch->state.mouse_hovering) {
                rect(close_bounds, titlebar_closed_button_bg_hovered_color(), 13, roundingAmt * s, 2.0, false);
            } else {
                auto co = color_titlebar_focused();
                co.a = fadea;
                rect(close_bounds, co, 13, roundingAmt * s, 2.0, false);
            }
            auto icon = "\ue8bb";
            auto ico_color = titlebar_closed_button_icon_color_hovered_pressed();
            ico_color.a = fadea;
            auto closed = get_cached_texture(root, root, "close_close_invariant", "Segoe Fluent Icons", 
                icon, ico_color, titlebar_button_icon_h() * .9);

            auto texture_info = closed;
            if (texture_info->id != -1) {
                clip(to_parent(root, c), s);
                draw_texture(*texture_info, 
                    close_bounds.x + close_bounds.w * .5 - texture_info->w * .5, 
                    close_bounds.y + close_bounds.h * .5 - texture_info->h * .5,
                    1.0);
            }
        }
    }

    c->real_bounds.y += std::round(titlebar_h * s);
    c->real_bounds.h -= std::round(titlebar_h * s);
    
    hypriso->draw_thumbnail(cid, c->real_bounds, roundingAmt * s, 2.0, 3);

    if (c->state.mouse_hovering) {
        auto b = pre_title_backup;
        //b.shrink(std::round(2.0 * s));
        //border(b, {1, 1, 1, .5}, std::round(2.0 * s), 0, roundingAmt * s, 2.0, false);
        //rect(b, {1, 1, 1, .1}, 0, roundingAmt * s, 2.0, false);
    }
}

static void create_option(int cid, Container *parent, int monitor, long creation_time) {
    later_immediate([cid](Timer *) {
        hypriso->set_hidden(cid, true);
    });
    auto c = parent->child(::absolute, FILL_SPACE, FILL_SPACE);
    *datum<int>(c, "cid") = cid;
    *datum<bool>(c, "was_hovering") = false;
    *datum<long>(c, "time_since_hovering_change") = 0;
    *datum<float>(c, "snap_back_scalar") = 1.0;
    *datum<float>(c, "snap_back_initial_x") = 0.0;
    *datum<float>(c, "snap_back_initial_y") = 0.0;
    *datum<float>(c, "snap_back_current_x") = 0.0;
    *datum<float>(c, "snap_back_current_y") = 0.0;
    c->when_paint = [monitor, creation_time](Container *actual_root, Container *c) {
        paint_option(actual_root, c, monitor, creation_time);
    };
    c->receive_events_even_if_obstructed_by_one = true;
    c->when_drag_end_is_click = false;
    c->minimum_x_distance_to_move_before_drag_begins = 3;
    c->minimum_y_distance_to_move_before_drag_begins = 3;
    c->when_mouse_enters_container = paint {
    };
    c->when_clicked = paint {
        if (bounds_contains(c->children[0]->real_bounds, root->mouse_current_x, root->mouse_current_y))
            return;
        auto cid = *datum<int>(c, "cid");
        auto overview_data = (OverviewData *) c->parent->user_data;
        overview_data->clicked_cid = cid;
        //hypriso->bring_to_front(cid, true);
        c->z_index = 1000;
        later_immediate([](Timer *) {
            overview::close();
        });
    };
    c->when_drag_start = paint {
        *datum<float>(c, "snap_back_scalar") = 1.0;
        *datum<bool>(c, "started_on_close") = c->children[0]->state.mouse_hovering;
        
        auto overview_data = (OverviewData *) c->parent->user_data;
        consume_event(root, c);
    };
    c->when_drag = paint {
        consume_event(root, c);
    };
    c->when_drag_end = paint {
        auto overview_data = (OverviewData *) c->parent->user_data;
        *datum<float>(c, "snap_back_scalar") = 1.0;
        if (!*datum<bool>(c, "started_on_close")) {
            *datum<float>(c, "snap_back_scalar") = .999f;
            animate(datum<float>(c, "snap_back_scalar"), 0.0, 80.0f, c->lifetime); 
            *datum<float>(c, "snap_back_initial_x") = root->mouse_initial_x;
            *datum<float>(c, "snap_back_initial_y") = root->mouse_initial_y;
            *datum<float>(c, "snap_back_current_x") = root->mouse_current_x;
            *datum<float>(c, "snap_back_current_y") = root->mouse_current_y;
        }
        consume_event(root, c);
    };

    auto close = c->child(FILL_SPACE, FILL_SPACE);
    close->when_paint = [](Container *actual_root, Container *c) {
        return;
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
        
        auto w = titlebar_h * .9 * titlebar_button_ratio();
        c->wanted_bounds = {b.x + b.w - w, b.y, w, ((float) titlebar_h) * .9};
        c->real_bounds = c->wanted_bounds;
    };
    auto cid_copy = cid;
    close->when_clicked = [cid_copy](Container *root, Container *c) {
        close_window(cid_copy);
    };
}

static void layout_options(Container *actual_root, Container *c, const Bounds &b, long creation_time, float overx, float overy) {
    for (auto ch : c->children) {
        auto cid = *datum<int>(ch, "cid");
        auto final_bounds = *datum<Bounds>(ch, "final_bounds");
        final_bounds.x += overx * .5;
        final_bounds.y += overy * .5;
        auto bounds = real_bounds_client(cid);
        bounds.y -= titlebar_h;
        bounds.h += titlebar_h;
        
        auto overview_data = (OverviewData *) c->user_data;
        auto scalar = overview_data->scalar;
        scalar = pull(slidetopos2, scalar);
        
        auto was_hovering = datum<bool>(ch, "was_hovering");
        auto time_since_hovering_change = datum<long>(ch, "time_since_hovering_change");
        if (scalar >= 1.0) {
            bool hovering_state = ch->state.mouse_hovering;
            if (ch->state.mouse_pressing && !ch->children[0]->state.mouse_hovering) {
                hovering_state = false;
            }
            if (*was_hovering != hovering_state) {
                *was_hovering = hovering_state;
                *time_since_hovering_change = get_current_time_in_ms();
            }

            auto current = get_current_time_in_ms();
            auto scalar = ((float) (current - *time_since_hovering_change)) / 60.0f;
            if (scalar > 1.0)
                scalar = 1.0;

            if (hovering_state) {
                auto uu = std::round(final_bounds.w * (1.0 + (.03 * scalar)));
                auto hh = std::round(final_bounds.h * (1.0 + (.03 * scalar)));
                final_bounds.x -= (uu - final_bounds.w) * .5;
                final_bounds.y -= (hh - final_bounds.h) * .5;
                final_bounds.w = uu; 
                final_bounds.h = hh; 
            } else {
                auto uu = std::round(final_bounds.w * (1.03 - (.03 * scalar)));
                auto hh = std::round(final_bounds.h * (1.03 - (.03 * scalar)));
                final_bounds.x -= (uu - final_bounds.w) * .5;
                final_bounds.y -= (hh - final_bounds.h) * .5;
                final_bounds.w = uu; 
                final_bounds.h = hh; 
            }
        }
        auto lerped = lerp(bounds, final_bounds, scalar); 

        ch->wanted_bounds = lerped;
        ch->real_bounds = ch->wanted_bounds;
        ::layout(actual_root, ch, ch->real_bounds);
    }
}

void actual_open(int monitor) {
    hypriso->whitelist_on = true;
    
    auto over = actual_root->child(::absolute, FILL_SPACE, FILL_SPACE);
    auto overview_data = new OverviewData;
    animate(&overview_data->scalar, 1.0, overview_anim_time, over->lifetime); 
    auto order = get_window_stacking_order();
    for (auto o : order) {
        if (hypriso->alt_tabbable(o) && get_monitor(o) == monitor && hypriso->get_active_workspace_id_client(o) == hypriso->get_active_workspace_id(monitor)) {
            overview_data->order.push_back(o);
            hypriso->set_hidden(o, true);
        }
    }
    over->user_data = overview_data;
    over->custom_type = (int) TYPE::OVERVIEW;
    //consume_everything(over);
    over->when_mouse_down = nullptr;
    over->when_clicked = paint {
        later_immediate([](Timer *) {
            overview::close();
        });
    };
    over->when_mouse_up = nullptr;
    screenshot_loop();
    auto creation_time = get_current_time_in_ms();
    
    over->when_paint = [monitor, creation_time](Container *actual_root, Container *c) {
        paint_over_wallpaper(actual_root, c, monitor, creation_time);
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS || monitor != rid)
            return;
        renderfix
        hypriso->damage_entire(monitor);
    };
    over->pre_layout = [monitor, creation_time](Container *actual_root, Container *c, const Bounds &b) {
        c->real_bounds = bounds_reserved_monitor(monitor);

        auto order = get_window_stacking_order();
        std::reverse(order.begin(), order.end());
        for (int i = c->children.size() - 1; i >= 0; i--) {
            auto child = c->children[i];
            auto cid = *datum<int>(child, "cid");
            bool found = false;
            for (auto option : order) {
                if (option == cid)
                    found = true;
            }
            if (!found) {
                delete child;
                c->children.erase(c->children.begin() + i);
                request_damage(actual_root, c);
            }
        }

        auto workspace_monitor = hypriso->get_active_workspace_id(monitor);

        // add if doesn't exist yet
        for (int i = order.size() - 1; i >= 0; i--) {
            auto option = order[i];
            if (hypriso->alt_tabbable(option) && hypriso->get_active_workspace_id_client(option) == workspace_monitor) {
                bool found = false;
                for (auto child : c->children) {
                    if (option == *datum<int>(child, "cid")) {
                        found = true;
                    }
                }
                if (!found) {
                    create_option(option, c, monitor, creation_time);
                    request_damage(actual_root, c);
                }
            }
        }

        auto reserved = bounds_reserved_monitor(monitor);

        ExpoLayout layout;
        auto s = scale(monitor);
        std::vector<ExpoCell *> cells;
        for (int i = 0; i < c->children.size(); i++) {
            auto o = *datum<int>(c->children[i], "cid");
            auto size = hypriso->thumbnail_size(o);
            auto height = size.h * (1/s) + titlebar_h;
            auto width = size.w * (1/s);
            auto x = bounds_client(o).x - reserved.x;
            auto y = bounds_client(o).y - reserved.y;
            auto cell = new DemoCell(i, x, y, width, height);
            cells.push_back(cell);
        }

        float pad = 120;

        layout.setCells(cells);
        layout.setAreaSize(reserved.w - reserved.x - pad, reserved.h - reserved.y - pad);
        layout.calculate();

        int minX = INT_MAX;
        int minY = INT_MAX;
        int maxW = 0;
        int maxH = 0;
        for (int i = 0; i < cells.size(); i++) {
            auto cell = cells[i];
            auto rect = ((DemoCell *) cell)->result();
            *datum<Bounds>(c->children[i], "final_bounds") = Bounds(rect.x, rect.y, rect.w, rect.h);
            if (rect.x < minX) 
                minX = rect.x;
            if (rect.y < minY) 
                minY = rect.y;                 
            if (rect.x + rect.w > maxW) 
                maxW = rect.x + rect.w;
            if (rect.y + rect.h > maxH) 
                maxH = rect.y + rect.h;
        }

        auto overx = reserved.w - minX - maxW;
        auto overy = reserved.h - minY - maxH;

        layout_options(actual_root, c, b, creation_time, overx, overy);
    };
    hypriso->damage_entire(monitor);
    hypriso->all_lose_focus();
}

struct Anim {
    float *value = nullptr;
    float start_value;
    float target;
    long start_time;
    float time_ms;
    std::weak_ptr<bool> lifetime;
    std::function<void(bool)> on_completion = nullptr;
};

static void animate(float *value, float target, float time_ms, std::shared_ptr<bool> lifetime, std::function<void(bool)> on_completion) {
    static std::vector<Anim *> anims;
    for (auto anim : anims) {
        if (anim->value == value) {
            anim->start_value = *value;
            anim->target = target;
            anim->start_time = get_current_time_in_ms();
            anim->time_ms = time_ms;
            anim->lifetime = lifetime;
            anim->on_completion = on_completion;
            return;
        }
    }
    
    auto anim = new Anim;
    anim->value = value;
    anim->start_value = *value;
    anim->target = target;
    anim->start_time = get_current_time_in_ms();
    anim->time_ms = time_ms;
    anim->lifetime = lifetime;
    anim->on_completion = on_completion;
    
    // TODO: this creates a later per animation which is dumb, they should all be combined into one that calls over a vec of anims
    later(1000.0f / 165.0f, [anim](Timer *t) {
        t->keep_running = true;
        
        if (anim->lifetime.lock()) {
            long delta = get_current_time_in_ms() - anim->start_time;
            float delta_ms = (float) delta;
            float scalar = delta_ms / anim->time_ms;
            if (scalar > 1.0) {
                t->keep_running = false;
                scalar = 1.0;
            }
            auto diff = (anim->target - anim->start_value) * scalar;
            *anim->value = anim->start_value + diff;
            if (!t->keep_running && anim->on_completion) {
                *anim->value = anim->target;
                anim->on_completion(true);
            }
        } else {
            delete anim;
            t->keep_running = false;
            if (anim->on_completion)
                anim->on_completion(false);
        }
    });
}

void overview::open(int monitor) {
    if (running) {
        for (auto c: actual_root->children) {
            if (c->custom_type == (int) TYPE::OVERVIEW) {
                auto overview_data = (OverviewData *) c->user_data;
                if (overview_data->scalar != 1.0) {
                    animate(&overview_data->scalar, 1.0, overview_anim_time, c->lifetime); 
                }
            }
        }
        return;
    }
    later_immediate([monitor](Timer *) {
        screenshotting_wallpaper = true;
        hypriso->screenshot_wallpaper(monitor);
        screenshotting_wallpaper = false;
        hypriso->screenshot_all(); 

        actual_open(monitor);
    });
}

static void actual_overview_stop() {
    running = false;
    hypriso->whitelist_on = false;
    auto m = actual_root;
    bool removed = false;
    for (int i = m->children.size() - 1; i >= 0; i--) {
        auto c = m->children[i];
        if (c->custom_type == (int) TYPE::OVERVIEW) {
            auto o_data = (OverviewData *) c->user_data;
            auto dragged_cid = -1;
            for (auto ch : c->children) {
                if (ch->state.mouse_dragging) {
                    dragged_cid = *datum<int>(ch, "cid");
                }
            }
            for (auto o : o_data->order) {
                hypriso->set_hidden(o, false);
            }
            if (dragged_cid != -1) {
                hypriso->set_hidden(dragged_cid, false);
                hypriso->bring_to_front(dragged_cid, true);
            } else if (o_data->clicked_cid != -1) {
                hypriso->set_hidden(o_data->clicked_cid, false);
                hypriso->bring_to_front(o_data->clicked_cid, true);
            }

            removed = true;
            delete c;
            m->children.erase(m->children.begin() + i);
        }
    }
    damage_all();
    if (removed)
        later_immediate([](Timer *) {
            hypriso->all_gain_focus();
        });
}

void overview::close() {
    if (running) {
        for (auto c: actual_root->children) {
            if (c->custom_type == (int) TYPE::OVERVIEW) {
                auto overview_data = (OverviewData *) c->user_data;
                animate(&overview_data->scalar, 0.0, overview_anim_time * .75, c->lifetime, [](bool normal_end) {
                    if (normal_end) {
                        actual_overview_stop();
                    }
                });
            }
        }
    }
}

void overview::click(int id, int button, int state, float x, float y) {
    return;
    if (state == 0) {
        overview::close();
        damage_all();
    }
}

bool overview::is_showing() {
    return running;
}

