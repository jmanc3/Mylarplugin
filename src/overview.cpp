#include "overview.h"

#include "heart.h"
#include "hypriso.h"
#include "icons.h"
#include "titlebar.h"
#include "layout_thumbnails.h"
#include "drag_workspace_switcher.h"

#include <climits>
#include <cmath>
#include <linux/input-event-codes.h>
#include <math.h>
#include <system_error>

static float shrink_factor = 1.0;

struct OverviewData : UserData {
    std::vector<int> order;
    float scalar = 0.0;
    int clicked_cid = -1;
};

struct ThumbData : UserData {
    int cid;
};


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

// {"anchors":[{"x":0,"y":1},{"x":0.30000000000000004,"y":0.675},{"x":1,"y":0}],"controls":[{"x":0.20596153063651845,"y":0.9462499830457899},{"x":0.4476281973031851,"y":0.06847220526801215}]}
static std::vector<float> snapback = { 0, 0.0050000000000000044, 0.010000000000000009, 0.017000000000000015, 0.02400000000000002, 0.03300000000000003, 0.04300000000000004, 0.05400000000000005, 0.06699999999999995, 0.08099999999999996, 0.09599999999999997, 0.11399999999999999, 0.134, 0.15600000000000003, 0.18200000000000005, 0.20999999999999996, 0.243, 0.281, 0.32499999999999996, 0.387, 0.43999999999999995, 0.486, 0.527, 0.563, 0.596, 0.626, 0.654, 0.679, 0.7030000000000001, 0.725, 0.745, 0.764, 0.782, 0.798, 0.8140000000000001, 0.8280000000000001, 0.842, 0.855, 0.867, 0.878, 0.888, 0.898, 0.908, 0.917, 0.925, 0.933, 0.94, 0.947, 0.953, 0.959, 0.964, 0.969, 0.974, 0.978, 0.982, 0.986, 0.989, 0.993, 0.995, 0.998, 1 };


void screenshot_loop() {
    running = true;
    later(1.0f, [](Timer *t) { 
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
        for (auto c : actual_root->children) {
            if (c->custom_type == (int) TYPE::OVERVIEW) {
                for (auto ch : c->children) {
                    if (ch->state.mouse_dragging || ch->state.mouse_hovering || ch->state.mouse_pressing) {
                        auto cid = *datum<int>(ch, "cid");
                        hypriso->screenshot(cid);
                    }
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
    //scalar = pull(slidetopos2, scalar);
    
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
    //scalar = pull(slidetopos2, scalar);
   
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
    auto fade_in_a = *datum<float>(c, "alpha_fade_in");
    //scalar = pull(slidetopos2, scalar);
    
    auto cid = *datum<int>(c, "cid");
    bool snap_back = false;

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
            snap_back = true;
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
    if (!*datum<bool>(c, "opaque")) {
        rect(c->real_bounds, {0, 0, 0, 0}, 0, roundingAmt * s, 2.0, true);
    }
    render_drop_shadow(monitor, 1.0, {0, 0, 0, .3f * scalar * fade_in_a}, roundingAmt * s, 2.0, c->real_bounds, 7 * s);
    auto th = titlebar_h;
    titlebar_h = std::round(titlebar_h * shrink_factor);
    defer(titlebar_h = th);

    auto pre_title_backup = c->real_bounds;
    {
        defer(c->real_bounds = pre_title_backup);
        
        c->real_bounds.h = std::round(titlebar_h * s);
        if (snap_back)
            c->real_bounds.h += std::round(1 * s);
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
        focused.a = fadea * fade_in_a;
        rect(titlebar_bounds, focused, titlebar_mask, roundingAmt * s, 2.0f, false);

        int icon_width = 0; 
        { // load icon
            TextureInfo *info = datum<TextureInfo>(c, std::to_string(rid) + "_icon");
            auto real_icon_h = std::round(titlebar_icon_h() * s * shrink_factor);
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
            //draw_texture(*info, c->real_bounds.x + 8 * s, center_y(c, info->h), pull(slidetopos2, fadea));
            draw_texture(*info, c->real_bounds.x + 8 * s, center_y(c, info->h), fadea * fade_in_a);
        }

        std::string title_text = hypriso->title_name(cid);
        if (!title_text.empty()) {
            TextureInfo *focused = nullptr;
            TextureInfo *unfocused = nullptr;
            auto color_titlebar_textfo = color_titlebar_text_focused();
            auto titlebar_text = titlebar_text_h() * shrink_factor;
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
                clip_w -= c->children[0]->real_bounds.w;
                if (clip_w > 0) {
                    draw_texture(*texture_info, 
                        c->real_bounds.x + overflow, center_y(c, texture_info->h), fadea * fade_in_a, clip_w);
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
                //auto co = color_titlebar_focused();
                //co.a = fadea;
                //rect(close_bounds, co, 13, roundingAmt * s, 2.0, false);
            }
            auto icon = "\ue8bb";
            auto ico_color = titlebar_closed_button_icon_color_hovered_pressed();
            ico_color.a = fadea;
            auto closed = get_cached_texture(root, root, "close_close_invariant", "Segoe Fluent Icons", 
                icon, ico_color, titlebar_button_icon_h() * shrink_factor);

            auto texture_info = closed;
            if (texture_info->id != -1) {
                clip(to_parent(root, c), s);
                draw_texture(*texture_info, 
                    close_bounds.x + close_bounds.w * .5 - texture_info->w * .5, 
                    close_bounds.y + close_bounds.h * .5 - texture_info->h * .5,
                    1.0 * fade_in_a);
            }
        }
    }

    if (false) {
        auto b = c->real_bounds;
        b.round();
        auto info = *datum<TextureInfo>(actual_root, "overview_gradient");
        auto mou = mouse();
        std::vector<MatteCommands> commands;
        MatteCommands command;
        command.bounds = b;
        command.type = 1;
        command.roundness = 8 * s;
        commands.push_back(command);
        draw_texture_matted(info,
            std::round(mou.x * s - info.w * .5),
            std::round(mou.y * s - info.h * .5),
            commands, scalar);
    }

    c->real_bounds.y += std::round(titlebar_h * s);
    c->real_bounds.h -= std::round(titlebar_h * s);
    
    hypriso->draw_thumbnail(cid, c->real_bounds, roundingAmt * s, 2.0, 3, fade_in_a);

    if (c->state.mouse_hovering) {
        auto b = pre_title_backup;

        //b.shrink(std::round(2.0 * s));
        //border(b, {1, 1, 1, .5}, std::round(2.0 * s), 0, roundingAmt * s, 2.0, false);
        //rect(b, {1, 1, 1, .1}, 0, roundingAmt * s, 2.0, false);
    }
}

static void create_option(int cid, Container *parent, int monitor, long creation_time) {
    auto overview_data = (OverviewData *) parent->user_data;

    later_immediate([cid](Timer *) {
        hypriso->set_hidden(cid, true);
    });
    auto c = parent->child(::absolute, FILL_SPACE, FILL_SPACE);
    // Windows added after overview open, put at front so when overview closes, they wont be on top
    if (overview_data->scalar >= 1.0) {
        for (int i = 0; parent->children.size(); i++) {
            auto ch = parent->children[i];
            if (ch == c) {
                parent->children.erase(parent->children.begin() + i);
                parent->children.insert(parent->children.begin(), c);
                break;
            }
        }
    }
    *datum<int>(c, "cid") = cid;
    *datum<bool>(c, "was_hovering") = false;
    *datum<bool>(c, "opaque") = hypriso->is_opaque(cid);
    *datum<long>(c, "time_since_hovering_change") = 0;
    *datum<float>(c, "snap_back_scalar") = 1.0;
    *datum<float>(c, "snap_back_initial_x") = 0.0;
    *datum<float>(c, "snap_back_initial_y") = 0.0;
    *datum<float>(c, "snap_back_current_x") = 0.0;
    *datum<float>(c, "snap_back_current_y") = 0.0;
    *datum<Bounds>(c, "previous_bounds") = Bounds(-1, -1, -1, -1);
    if (overview_data->scalar == 1.0) {
        *datum<float>(c, "alpha_fade_in") = 0.0;
        animate(datum<float>(c, "alpha_fade_in"), 1.0, overview_anim_time, c->lifetime);
    } else {
        *datum<float>(c, "alpha_fade_in") = 1.0;
    }
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

        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            titlebar::titlebar_right_click(cid);
            return;
        }
        
        auto overview_data = (OverviewData *) c->parent->user_data;
        overview_data->clicked_cid = cid;
        //hypriso->bring_to_front(cid, true);
        c->z_index = 1000;
        later(45, [](Timer *) {
            overview::close();
        });
    };
    c->when_drag_start = paint {
        *datum<float>(c, "snap_back_scalar") = 1.0;
        *datum<bool>(c, "started_on_close") = c->children[0]->state.mouse_hovering;
        
        auto overview_data = (OverviewData *) c->parent->user_data;
        consume_event(root, c);
        drag_workspace_switcher::force_hold_open(true);
    };
    c->when_drag = paint {
        consume_event(root, c);
    };
    c->when_drag_end = [monitor](Container *root, Container *c) {
        auto cid = *datum<int>(c, "cid");
        for (auto c : actual_root->children) {
            if (c->custom_type == (int) TYPE::WORKSPACE_SWITCHER) {
                for (auto ch : c->children) {
                    if (bounds_contains(ch->real_bounds, actual_root->mouse_current_x, actual_root->mouse_current_y)) {
                        auto space = *datum<int>(ch, "workspace");
                        if (space == -1) {
                            // next avaialable
                            auto spaces = hypriso->get_workspaces(monitor);
                            int next = 1;
                            if (!spaces.empty())
                                next = spaces[spaces.size() - 1] + 1;
                            later_immediate([cid, next](Timer *) {
                                overview::instant_close();
                                hypriso->move_to_workspace(cid, next, false);
                                //hypriso->bring_to_front(cid);
                            });
                        } else {
                            later_immediate([cid, space](Timer *) {
                                overview::instant_close();
                                hypriso->move_to_workspace(cid, hypriso->space_id_to_raw(space), false);
                                //hypriso->bring_to_front(cid);
                            });
                        }
                        break;
                    }
                }
            }
        }

        auto overview_data = (OverviewData *) c->parent->user_data;
        *datum<float>(c, "snap_back_scalar") = 1.0;
        if (!*datum<bool>(c, "started_on_close")) {
            *datum<float>(c, "snap_back_scalar") = .999f;
            animate(datum<float>(c, "snap_back_scalar"), 0.0, 220.0f, c->lifetime, nullptr, 
                [](float scalar) { return pull(snapback, scalar); }); 
            *datum<float>(c, "snap_back_initial_x") = root->mouse_initial_x;
            *datum<float>(c, "snap_back_initial_y") = root->mouse_initial_y;
            *datum<float>(c, "snap_back_current_x") = root->mouse_current_x;
            *datum<float>(c, "snap_back_current_y") = root->mouse_current_y;
        }
        consume_event(root, c);
        drag_workspace_switcher::force_hold_open(false);
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
        
        auto w = titlebar_h * shrink_factor * titlebar_button_ratio();
        c->wanted_bounds = {b.x + b.w - w, b.y, w, ((float) titlebar_h) * shrink_factor};
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
        
        // Animate to new positions when another window is added or removed
        {
            auto animating_final = datum<bool>(ch, "animating_final");
            auto final_animation_scalar = datum<float>(ch, "final_animation_scalar");
            auto final_animation_start = datum<Bounds>(ch, "final_animation_start");

            // if previous final_bounds.x and final_bounds.y diff, animate to next pos
            auto previous_final = datum<Bounds>(ch, "previous_bounds");
            if (previous_final->w == -1 && previous_final->h == -1) {
                //notify(fz("{}", hypriso->title_name(cid)));
                // first frame should not animate
                *previous_final = final_bounds;
                //notify(fz("{} {} {} {} {} {} {} {}", previous_final->x, previous_final->y, previous_final->w, previous_final->h, final_bounds.x, final_bounds.y, final_bounds.w, final_bounds.h));
                
            }
            auto fade = *datum<float>(ch, "alpha_fade_in");
            if ((std::abs(previous_final->x - final_bounds.x) > 3 || std::abs(previous_final->y - final_bounds.y) > 3) && fade >= 1.0) {
                if (!*animating_final) {
                    // this detected a change in final bounds
                    // start an animation from previous to final
                    *animating_final = true;
                    *final_animation_scalar = 0.0;
                    // todo final_animation_start should actually be generated if was in the middle of animation actually
                    *final_animation_start = *previous_final;
                    animate(final_animation_scalar, 1.0, 200, ch->lifetime, [ch](bool normal_end) {
                        if (normal_end) {
                            auto animating_final = datum<bool>(ch, "animating_final");
                            *animating_final = false;
                        }
                    }, [](float scalar) {
                        return pull(snapback, scalar);
                    });
                }
            }
            *previous_final = final_bounds;
            if (*animating_final) {
                auto diffx = (final_bounds.x - final_animation_start->x);
                auto diffy = (final_bounds.y - final_animation_start->y);
                auto diffw = (final_bounds.w - final_animation_start->w);
                auto diffh = (final_bounds.h - final_animation_start->h);
                //notify(fz("{} {} {} {} {}", hypriso->title_name(cid), diffx, diffy, diffw, diffh));
                final_bounds.x -= diffx - (diffx * *final_animation_scalar);
                final_bounds.y -= diffy - (diffy * *final_animation_scalar);
                final_bounds.w -= diffw - (diffw * *final_animation_scalar);
                final_bounds.h -= diffh - (diffh * *final_animation_scalar);
            }
        }

        auto overview_data = (OverviewData *) c->user_data;
        auto scalar = overview_data->scalar;

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
    drag_workspace_switcher::open();
    hypriso->all_lose_focus();
    
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
        auto overview_data = (OverviewData *) c->user_data;
        auto bounds = bounds_monitor(rid);
        auto r = bounds_monitor(rid);
        
        auto actual_w = 450 * s;
        auto actual_h = 130 * s;
        //auto scalar = pull(slidetopos2, overview_data->scalar);
        auto scalar = overview_data->scalar;
        Bounds b = {bounds.x + bounds.w * .5 * s - actual_w * .5,
              10 * s * scalar - actual_h + actual_h * scalar,
              actual_w,
              actual_h};
        //render_drop_shadow(rid, 1.0, {0, 0, 0, .2f * scalar}, 12 * s, 2.0, b);
        //rect(b, {1, 1, 1, .1f * scalar}, 0, 12 * s, 2.0, true, 1.0 * scalar);
        hypriso->damage_entire(monitor);
        
        auto ids = hypriso->get_workspaces(monitor);
        float x = 0;
        for (auto id : ids) {
            auto rw = (bounds.h / bounds.w) * 200 * s;
            //hypriso->draw_workspace(rid, id, {x, 0, 1.0f * 200 * s, rw});
            x += rw;
        }

        //testDraw();
    };
    over->after_paint = [monitor, creation_time](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS || monitor != rid)
            return;
        renderfix
        auto overview_data = (OverviewData *) c->user_data;
        /*for (auto ch : c->children) {
            auto info = *datum<TextureInfo>(actual_root, "overview_gradient");
            auto mou = mouse();
            std::vector<MatteCommands> commands;
            MatteCommands command;
            auto b = ch->real_bounds;
            command.bounds = b.scale(s);
            command.bounds.round();
            command.type = 1;
            command.roundness = 8 * s;
            commands.push_back(command);
            draw_texture_matted(info, 
                std::round(mou.x * s - info.w * .5), 
                std::round(mou.y * s - info.h * .5), 
                commands, 1.0);
        }*/
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
        auto overview_data = ((OverviewData *) c->user_data);

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
}

void overview::open(int monitor) {
    if (running) {
        for (auto c: actual_root->children) {
            if (c->custom_type == (int) TYPE::OVERVIEW) {
                auto overview_data = (OverviewData *) c->user_data;
                if (overview_data->scalar != 1.0) {
                    animate(&overview_data->scalar, 1.0, overview_anim_time, c->lifetime, nullptr, [](float scalar) {
                        return pull(slidetopos2, scalar);
                    }); 
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
        auto ids = hypriso->get_workspaces(monitor);
        for (auto id : ids)
            hypriso->screenshot_space(monitor, id);
        int size = scale(monitor) * bounds_monitor(monitor).w * .35;
        RGBA center = {1, 1, 1, .3};
        RGBA edge = {1, 1, 1, 0};
        //*datum<TextureInfo>(actual_root, "overview_gradient") = gen_gradient_texture(center, edge, size);

        actual_open(monitor);
    });
}

void fade_in_min_max(int cid) {
    if (auto c = get_cid_container(cid)) {
        float *min_max = datum<float>(c, "min_max_fade");
        *min_max = 0.0;
        animate(min_max, 1.0, 200.0f, c->lifetime);
    }
}

static void actual_overview_stop(bool focus) {
    running = false;
    hypriso->whitelist_on = false;
    auto m = actual_root;
    bool removed = false;
    //auto info = *datum<TextureInfo>(actual_root, "overview_gradient");
    //free_text_texture(info.id);
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
                fade_in_min_max(o);
            }
            if (dragged_cid != -1) {
                hypriso->set_hidden(dragged_cid, false);
                if (focus)
                    hypriso->bring_to_front(dragged_cid, true);
                fade_in_min_max(dragged_cid);
            } else if (o_data->clicked_cid != -1) {
                hypriso->set_hidden(o_data->clicked_cid, false);
                if (focus)
                    hypriso->bring_to_front(o_data->clicked_cid, true);
                fade_in_min_max(o_data->clicked_cid);
            } else {
                removed = true;
            }
            delete c;
            m->children.erase(m->children.begin() + i);
        }
    }
    damage_all();
    if (removed && focus)
        later_immediate([](Timer *) {
            hypriso->all_gain_focus();
        });
}

void overview::instant_close() {
    actual_overview_stop(false);
}

void overview::close(bool focus) {
    if (running) {
        drag_workspace_switcher::close();
        for (auto c: actual_root->children) {
            if (c->custom_type == (int) TYPE::OVERVIEW) {
                auto overview_data = (OverviewData *) c->user_data;
                animate(&overview_data->scalar, 0.0, overview_anim_time * 1.2, c->lifetime, [focus](bool normal_end) {
                    if (normal_end) {
                        actual_overview_stop(focus);
                    }
                }, [](float scalar) {
                    return pull(slidetopos2, scalar);
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

