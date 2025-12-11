#include "titlebar.h"

#include "second.h"
#include "hypriso.h"
#include "events.h"
#include "drag.h"
#include "icons.h"
#include "defer.h"
#include "popup.h"
#include "resizing.h"

#include "process.hpp"

#include <assert.h>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <unistd.h>
#include <math.h>
#include <linux/input-event-codes.h>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

void create_titlebar(Container *root, Container *parent);
   
static float titlebar_button_ratio() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_button_ratio", 1.4375f);
}

float titlebar_text_h() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_text_h", 15);
}

static float titlebar_icon_h() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_icon_h", 21);
}

float titlebar_button_icon_h() {
    return hypriso->get_varfloat("plugin:mylardesktop:titlebar_button_icon_h", 13);
}

RGBA color_titlebar_focused() {
    static RGBA default_color("ffffffff");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_focused_color", default_color);
}
RGBA color_titlebar_unfocused() {
    static RGBA default_color("f0f0f0ff");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_unfocused_color", default_color);
}
RGBA color_titlebar_text_focused() {
    static RGBA default_color("000000ff");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_focused_text_color", default_color);
}
RGBA color_titlebar_text_unfocused() {
    static RGBA default_color("303030ff");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_unfocused_text_color", default_color);
}
RGBA titlebar_button_bg_hovered_color() {
    static RGBA default_color("rgba(ff0000ff)");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_button_bg_hovered_color", default_color);
}
RGBA titlebar_button_bg_pressed_color() {
    static RGBA default_color("rgba(0000ffff)");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_button_bg_pressed_color", default_color);
}
RGBA titlebar_closed_button_bg_hovered_color() {
    static RGBA default_color("rgba(ff0000ff)");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_closed_button_bg_hovered_color", default_color);
}
RGBA titlebar_closed_button_bg_pressed_color() {
    static RGBA default_color("rgba(0000ffff)");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_closed_button_bg_pressed_color", default_color);
}
RGBA titlebar_closed_button_icon_color_hovered_pressed() {
    static RGBA default_color("rgba(999999ff)");
    return hypriso->get_varcolor("plugin:mylardesktop:titlebar_closed_button_icon_color_hovered_pressed", default_color);
}

void titlebar_pre_layout(Container* root, Container* self, const Bounds& bounds) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto rid = *datum<int>(root, "cid");
    auto cid = *datum<int>(self, "cid");
    auto s = scale(rid);
    self->wanted_bounds.h = titlebar_h; 
    self->children[1]->wanted_bounds.w = std::round(titlebar_h * titlebar_button_ratio());
    self->children[2]->wanted_bounds.w = std::round(titlebar_h * titlebar_button_ratio());
    self->children[3]->wanted_bounds.w = std::round(titlebar_h * titlebar_button_ratio());
}

void titlebar::titlebar_right_click(int cid, bool centered) {
    auto m = mouse();
    std::vector<PopOption> root;
    {
        PopOption pop;
        if (hypriso->is_pinned(cid)) {
            pop.icon_left = ":Papirus:checkbox-checked-symbolic";
        } else {
            //pop.icon_left = ":Papirus:checkbox-symbolic";
        }
        pop.text = "Keep above others";
        pop.on_clicked = [cid]() {
            if (hypriso->is_pinned(cid)) {
                hypriso->pin(cid, false);
            } else {
                hypriso->pin(cid, true);
            }
            update_restore_info_for(cid);
        };
        root.push_back(pop);
    }
    {
        PopOption pop;
        if (hypriso->is_fake_fullscreen(cid)) {
            pop.icon_left = ":Papirus:checkbox-checked-symbolic";
        } else {
            //pop.icon_left = ":Papirus:checkbox-symbolic";
        }
        pop.text = "Fake fullscreen";   
        
        pop.on_clicked = [cid]() {
            if (hypriso->is_fake_fullscreen(cid)) {
                hypriso->fake_fullscreen(cid, false);
            } else {
                hypriso->fake_fullscreen(cid, true);
            }
            auto b = bounds_client(cid);
            hypriso->move_resize(cid, b.x, b.y, b.w, b.h);
            update_restore_info_for(cid);
        };
        root.push_back(pop);
    }
    {
        PopOption pop;
        if (hypriso->has_decorations(cid)) {
            //pop.icon_left = ":Papirus:checkbox-symbolic";
        } else {
            pop.icon_left = ":Papirus:checkbox-checked-symbolic";
        }
        pop.text = "Remove titlebar";

        pop.on_clicked = [cid]() {
            auto client = get_cid_container(cid);
            auto info = (ClientInfo *) client->user_data;
            if (hypriso->has_decorations(cid)) {
                hypriso->remove_decorations(cid);
                if (auto c = get_cid_container(cid)) {
                    delete c->children[0];
                    c->children.erase(c->children.begin());
                }
                auto b = bounds_client(cid);
                hypriso->move_resize(cid, b.x, b.y - titlebar_h, b.w, b.h + titlebar_h);
                hypriso->set_corner_rendering_mask_for_window(cid, 0);
            } else {
                hypriso->reserve_titlebar(cid, titlebar_h);
                if (auto c = get_cid_container(cid)) {
                    create_titlebar(actual_root, c);
                    *datum<float>(c, "titlebar_alpha") = 1.0;
                }
                auto b = bounds_client(cid);
                hypriso->move_resize(cid, b.x, b.y + titlebar_h, b.w, b.h - titlebar_h);
                hypriso->set_corner_rendering_mask_for_window(cid, 3);
            }
        };
        root.push_back(pop);        
    }
    {
        PopOption pop;
        pop.seperator = true;
        root.push_back(pop);        
    }
    {
        PopOption pop;
        auto info = &restore_infos[hypriso->class_name(cid)];
        if (info->remember_size)
            pop.icon_left = ":Papirus:checkbox-checked-symbolic";
        pop.text = "Remember size";
        pop.on_clicked = [cid]() {
            auto info = &restore_infos[hypriso->class_name(cid)];
            info->remember_size = !info->remember_size;
            update_restore_info_for(cid);
        };
        root.push_back(pop);
    }
    {
        PopOption pop;
        auto info = &restore_infos[hypriso->class_name(cid)];
        if (info->remember_workspace)
            pop.icon_left = ":Papirus:checkbox-checked-symbolic";
        pop.text = "Remember workspace";
        pop.on_clicked = [cid]() {
            auto info = &restore_infos[hypriso->class_name(cid)];
            info->remember_workspace = !info->remember_workspace;
            update_restore_info_for(cid);
        };
        root.push_back(pop);        
    }
    {
        PopOption pop;
        auto info = &restore_infos[hypriso->class_name(cid)];
        if (info->remove_titlebar)
            pop.icon_left = ":Papirus:checkbox-checked-symbolic";
        pop.text = "Never titlebar";
        pop.on_clicked = [cid]() {
            auto info = &restore_infos[hypriso->class_name(cid)];
            info->remove_titlebar = !info->remove_titlebar;
            bool has_titlebar = hypriso->has_decorations(cid);
            if (info->remove_titlebar && has_titlebar) {
                hypriso->remove_decorations(cid);
                if (auto c = get_cid_container(cid)) {
                    delete c->children[0];
                    c->children.erase(c->children.begin());
                }
                auto b = bounds_client(cid);
                hypriso->move_resize(cid, b.x, b.y - titlebar_h, b.w, b.h + titlebar_h);
                hypriso->set_corner_rendering_mask_for_window(cid, 0);
            }
            if (!info->remove_titlebar && !has_titlebar) {
                hypriso->reserve_titlebar(cid, titlebar_h);
                if (auto c = get_cid_container(cid)) {
                    create_titlebar(actual_root, c);
                    *datum<float>(c, "titlebar_alpha") = 1.0;
                }
                auto b = bounds_client(cid);
                hypriso->move_resize(cid, b.x, b.y + titlebar_h, b.w, b.h - titlebar_h);
                hypriso->set_corner_rendering_mask_for_window(cid, 3);
            }
            update_restore_info_for(cid);
        };
        root.push_back(pop);        
    }

    if (centered) {
        popup::open(root, m.x - (277 * .5), m.y, cid);
    } else {
        popup::open(root, m.x - 1, m.y + 1, cid);
    }
}

TextureInfo *get_cached_texture(Container *root_with_scale, Container *container_texture_saved_on, std::string needle, std::string font, std::string text, RGBA color, int wanted_h) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    //return {};
    
    auto rid = *datum<int>(root_with_scale, "cid");
    auto s = scale(rid);
    TextureInfo *info = datum<TextureInfo>(container_texture_saved_on, needle);
    //notify(needle + "                                                  ");
    //notify(needle + " " + std::to_string(info->id));
    
    int h = std::round(wanted_h * s);

    if (info->id != -1) { // regenerate if any of the following changes
        bool diff_color = info->cached_color != color;
        bool diff_h = info->cached_h != h;
        bool diff_text = info->cached_text != text;
        if (diff_color || diff_h || diff_text) {
            log(fz("{} {} {} {} {} {} cached:{} text:{}", needle, info->cached_h, h, diff_color, diff_h, diff_text, info->cached_text, text));
            free_text_texture(info->id);
            info->id = -1;
            info->reattempts_count = 0;
        }
    }
    
    if (info->id == -1) {
        if (info->reattempts_count < 10) {
            info->last_reattempt_time = get_current_time_in_ms();
            info->reattempts_count++;

            auto texture = gen_text_texture(font, text, h, color);
            if (texture.id != -1) {
                info->id = texture.id;
                info->w = texture.w;
                info->h = texture.h;
                info->cached_color = color;
                info->cached_h = h;
                info->cached_text = text;
                info->reattempts_count = 0;
            }
        }
    }

    return info; 
}

void paint_button(Container *actual_root, Container *c, std::string name, std::string icon, bool is_close = false) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    auto client = first_above_of(c, TYPE::CLIENT);
    auto cid = *datum<int>(client, "cid");

    if (active_id == cid && stage == (int) STAGE::RENDER_PRE_WINDOW) {
        renderfix

        auto b = c->real_bounds;
        b.round();
        auto a = *datum<float>(client, "titlebar_alpha");
        
        auto focused = get_cached_texture(root, root, name + "_focused", "Segoe Fluent Icons",
            icon, color_titlebar_text_focused(), titlebar_button_icon_h());
        auto unfocused = get_cached_texture(root, root, name + "_unfocused", "Segoe Fluent Icons", 
            icon, color_titlebar_text_unfocused(), titlebar_button_icon_h());
        auto closed = get_cached_texture(root, root, name + "_close_invariant", "Segoe Fluent Icons", 
            icon, titlebar_closed_button_icon_color_hovered_pressed(), titlebar_button_icon_h());

        int mask = 16;
        if (is_close)
            mask = 13;
        if (c->state.mouse_pressing) {
            auto color = is_close ? titlebar_closed_button_bg_pressed_color() : titlebar_button_bg_pressed_color();
            color.a = a;
            clip(to_parent(root, c), s);
            rect(b, color, mask, hypriso->get_rounding(cid) * s, 2.0f);
        } else if (c->state.mouse_hovering) {
            auto color = is_close ? titlebar_closed_button_bg_hovered_color() : titlebar_button_bg_hovered_color();
            color.a = a;
            clip(to_parent(root, c), s);
            rect(b, color, mask, hypriso->get_rounding(cid) * s, 2.0f);
        }

        auto texture_info = focused;
        if (!hypriso->has_focus(cid))
            texture_info = unfocused;
        if (is_close && c->state.mouse_pressing || c->state.mouse_hovering)
            texture_info = closed;
        if (texture_info->id != -1) {
            clip(to_parent(root, c), s);
            draw_texture(*texture_info, center_x(c, texture_info->w), center_y(c, texture_info->h), a);
        }
    }
}

void paint_titlebar(Container *actual_root, Container *c) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto root = get_rendering_root();
    if (!root) return;
    
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);
    auto client = first_above_of(c, TYPE::CLIENT);
    auto cid = *datum<int>(client, "cid");

    if (active_id == cid && stage == (int) STAGE::RENDER_PRE_WINDOW) {
        {
        renderfix
        auto a = *datum<float>(client, "titlebar_alpha");

        int icon_width = 0; 
        { // load icon
            TextureInfo *info = datum<TextureInfo>(client, std::to_string(rid) + "_icon");
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
            if (hypriso->has_focus(cid) || true) {
                focus_alpha = color_titlebar_text_focused().a;
            } else {
                focus_alpha = color_titlebar_text_unfocused().a;
            }
            clip(to_parent(root, c), s);
            draw_texture(*info, c->real_bounds.x + 8 * s, center_y(c, info->h), a * focus_alpha);
        }
        
        std::string title_text = hypriso->title_name(cid);
        if (!title_text.empty()) {
            TextureInfo *focused = nullptr;
            TextureInfo *unfocused = nullptr;
            auto color_titlebar_textfo = color_titlebar_text_focused();
            auto titlebar_text = titlebar_text_h();
            auto color_titlebar_textunfo = color_titlebar_text_unfocused();
            focused = get_cached_texture(root, client, std::to_string(rid) + "_title_focused", mylar_font, 
                title_text, color_titlebar_textfo, titlebar_text);
            unfocused = get_cached_texture(root, client, std::to_string(rid) + "_title_unfocused", mylar_font, 
                title_text, color_titlebar_textunfo, titlebar_text);
            
            auto texture_info = focused;
            if (!hypriso->has_focus(cid))
                texture_info = unfocused;
 
            if (texture_info->id != -1) {
                auto overflow = std::max((c->real_bounds.h - texture_info->h), 0.0);
                if (icon_width != 0)
                    overflow = icon_width + 16 * s;

                auto clip_w = c->real_bounds.w - overflow;
                if (clip_w > 0) {
                    draw_texture(*texture_info, 
                        c->real_bounds.x + overflow, center_y(c, texture_info->h), a, clip_w);
                }
            }
        }
        }

    }
    /*
    auto bb = bounds_client(cid);
    bb.y -= titlebar_h;
    bb.h = titlebar_h;
    bb.scale(s);
    //bb.w = 30;
    */
    //auto i = hypriso->pass_info(cid);
    //rect({i.cbx, i.cby - std::round(titlebar_h * s), i.cbw, std::round(titlebar_h * s)}, {1, 0, 0, 1});
}

void create_titlebar(Container *root, Container *parent) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    auto titlebar_parent = parent->child(::hbox, FILL_SPACE, FILL_SPACE); // actual wanted bounds set in pre_layout
    titlebar_parent->automatically_paint_children = false;
    titlebar_parent->pre_layout = titlebar_pre_layout;
    titlebar_parent->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        auto client = first_above_of(c, TYPE::CLIENT);

        auto already_painted = *datum<bool>(client, "already_painted");
        if (already_painted)
            return;        
        auto cid = *datum<int>(client, "cid");

        if (active_id == cid && stage == (int) STAGE::RENDER_PRE_WINDOW) {
            renderfix

            auto a = *datum<float>(client, "titlebar_alpha");
            
            auto titlebar_color = color_titlebar_focused();
            if (!hypriso->has_focus(cid))
                titlebar_color = color_titlebar_unfocused();
            titlebar_color.a *= a;

            auto bounds = c->real_bounds;
            bounds.round();
            bool being_animated = hypriso->being_animated(cid);
            if (being_animated || (drag::dragging() && drag::drag_window() == cid) || (resizing::resizing() && resizing::resizing_window() == cid))
                bounds.h += 1 * s;
            rect(bounds, titlebar_color, 12, hypriso->get_rounding(cid) * s, 2.0f);
        }
    };
    titlebar_parent->receive_events_even_if_obstructed_by_one = true;
    titlebar_parent->when_mouse_motion = request_damage;
    titlebar_parent->when_mouse_enters_container = titlebar_parent->when_mouse_motion;
    titlebar_parent->when_mouse_leaves_container = titlebar_parent->when_mouse_motion;

    auto titlebar = titlebar_parent->child(FILL_SPACE, FILL_SPACE);
    titlebar->when_paint = paint {
        paint_titlebar(root, c);
    };
    titlebar->when_mouse_down = paint {
        auto client = first_above_of(c, TYPE::CLIENT);
        auto cid = *datum<int>(client, "cid");
        consume_event(root, c);
        hypriso->bring_to_front(cid);        
        *datum<Bounds>(client, "initial_click_position") = mouse();
    };
    titlebar->when_mouse_up = consume_event;
    titlebar->minimum_x_distance_to_move_before_drag_begins = 3;
    titlebar->minimum_y_distance_to_move_before_drag_begins = 3;
    titlebar->when_clicked = paint {
        auto client = first_above_of(c, TYPE::CLIENT);
        auto cid = *datum<int>(client, "cid");
        
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            titlebar::titlebar_right_click(cid);
            c->when_mouse_down(root, c);
            return;
        }

        c->when_mouse_down(root, c);
        if (double_clicked(c, "max")) {
            // todo should actually transition from non max snap, to max and then unsnap?
            if (*datum<bool>(client, "snapped")) {
                drag::snap_window(hypriso->monitor_from_cursor(), cid, (int)SnapPosition::NONE);
            } else {
                drag::snap_window(hypriso->monitor_from_cursor(), cid, (int)SnapPosition::MAX);
            }
       }
    };
    titlebar->when_drag_end_is_click = false;
    titlebar->when_drag_start = paint {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        //notify("title drag start");
        auto client = first_above_of(c, TYPE::CLIENT);
        auto cid = *datum<int>(client, "cid");
        if (hypriso->is_fullscreen(cid))  
            return;
        if (auto c = get_cid_container(cid)) {
            *datum<bool>(client, "drag_from_titlebar") = true;
        }
        drag::begin(cid);
        root->consumed_event = false;
        hypriso->bring_to_front(cid);
    };
    titlebar->when_drag = paint {
        auto client = first_above_of(c, TYPE::CLIENT);
        auto cid = *datum<int>(client, "cid");
        //drag::motion(cid);
        root->consumed_event = true;
    };
    titlebar->when_drag_end = paint {
        auto client = first_above_of(c, TYPE::CLIENT);
        auto cid = *datum<int>(client, "cid");
        //drag::end(cid);
        root->consumed_event = true;
    };
 
    titlebar_parent->alignment = ALIGN_RIGHT;
    auto min = titlebar_parent->child(FILL_SPACE, FILL_SPACE);
    min->when_paint = paint {
        paint_button(root, c, "min", "\ue921");
    };
    min->when_mouse_down = consume_event;
    min->when_mouse_up = consume_event;
    min->when_clicked = paint {
         auto client = first_above_of(c, TYPE::CLIENT);
         hypriso->set_hidden(*datum<int>(client, "cid"), true);
    };
    auto max = titlebar_parent->child(FILL_SPACE, FILL_SPACE);
    max->when_paint = paint {
        auto client = first_above_of(c, TYPE::CLIENT);
        assert(client);
        bool snapped = *datum<bool>(client, "snapped");
        //notify(fz("mouse {:.2f} {:.2f}                                           ", mouse().x, mouse().y));
        //notify(fz("{:.2f} {:.2f}                                           ", c->real_bounds.x, c->real_bounds.y));

        if (snapped) {
            paint_button(root, c, "max_snapped", "\ue923");
        } else {
            paint_button(root, c, "max_unsnapped", "\ue922");
        }
    };
    max->when_mouse_down = consume_event;
    max->when_mouse_up = consume_event;
    max->when_clicked = paint {
        auto client = first_above_of(c, TYPE::CLIENT);
        assert(client);
        int cid = *datum<int>(client, "cid");
        auto snapped = *datum<bool>(client, "snapped");
        auto rid = get_monitor(cid);
        if (snapped) {
            drag::snap_window(hypriso->monitor_from_cursor(), cid, (int) SnapPosition::NONE);
        } else {
            drag::snap_window(hypriso->monitor_from_cursor(), cid, (int) SnapPosition::MAX);
        }
        hypriso->bring_to_front(cid);
    };
    auto close = titlebar_parent->child(FILL_SPACE, FILL_SPACE);
    close->when_paint = paint {
        paint_button(root, c, "close", "\ue8bb", true);
    };
    close->when_clicked = paint {
        auto client = first_above_of(c, TYPE::CLIENT);
        assert(client);
        auto cid = *datum<int>(client, "cid");
        later_immediate([cid](Timer *t) { close_window(cid); });
    };
    close->when_mouse_down = consume_event;
    close->when_mouse_up = consume_event;
}

bool titlebar_disabled(int id) {
    auto info = &restore_infos[hypriso->class_name(id)];
    return info->remove_titlebar;
}

void titlebar::on_window_open(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
     if (hypriso->wants_titlebar(id) && !titlebar_disabled(id)) {
        hypriso->reserve_titlebar(id, titlebar_h);
        
        if (auto c = get_cid_container(id)) {
            create_titlebar(actual_root, c);
            *datum<float>(c, "titlebar_alpha") = 1.0;
        }
    } else {
        hypriso->set_corner_rendering_mask_for_window(id, 0);
    }
}

void titlebar::on_window_closed(int id) {
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::OUR_POPUP) {
            auto pud = (PopupUserData *) c->user_data;
            if (pud->cid == id) {
                popup::close(c->uuid);
            }
        }
    }
    if (hypriso->has_decorations(id)) {
        hypriso->remove_decorations(id);
    }
}

static void draw_text(std::string text, int x, int y) {
    return;
    TextureInfo first_info;
    {
        first_info = gen_text_texture("Monospace", text, 40, {0, 0, 0, 1});
        rect(Bounds(x, y, (double) first_info.w, (double) first_info.h), {1, 0, 1, 1});
        draw_texture(first_info, x + 3, y + 4);
        free_text_texture(first_info.id);
    }
    {
        auto info = gen_text_texture("Monospace", text, 40, {1, 1, 1, 1});
        draw_texture(info, x, y);
        free_text_texture(info.id);
    }
    
}

void titlebar::on_draw_decos(std::string name, int monitor, int id, float a) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    Container *c = get_cid_container(id);
    if (!c) return;
    
    Container *m = actual_root;
    if (!m) return;

    *datum<float>(c, "titlebar_alpha") = a;
    *datum<bool>(c, "already_painted") = false;

    auto stage = datum<int>(m, "stage"); 
    auto active_id = datum<int>(m, "active_id"); 
    
    int before_stage = *stage;
    int before_active_id = *active_id;
    
    *stage = (int) STAGE::RENDER_PRE_WINDOW;
    *active_id = id;

    c->children[0]->automatically_paint_children = true;
    paint_outline(actual_root, c);
    c->children[0]->automatically_paint_children = false;
    
    *datum<bool>(c, "already_painted") = true;

    *stage = before_stage;
    *active_id = before_active_id;
}

void titlebar::on_activated(int id) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (auto c = get_cid_container(id)) {
        request_damage(actual_root, c);
    }
}


