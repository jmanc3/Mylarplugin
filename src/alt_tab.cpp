
#include "alt_tab.h"

#include "heart.h"
#include "drag.h"
#include "icons.h"
#include "titlebar.h"
#include "layout_thumbnails.h"

#define LAST_TIME_ACTIVE "last_time_active"

static float sd = .65;
Bounds max_thumb = { 510 * sd, 310 * sd, 510 * sd, 310 * sd };
static int active_index = 0;
static long show_time = 0;
static long show_delay = 40;

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


void alt_tab::on_window_open(int id) {
    Container *c = get_cid_container(id);

    //later_immediate([](Timer *) {
        //hypriso->screenshot_all();
    //});

    assert(c && "alt_tab::on_window_open assumes Container for id has already been created");

    if (!get_data<long>(c->uuid, LAST_TIME_ACTIVE))
        *datum<long>(c, LAST_TIME_ACTIVE) = get_current_time_in_ms();
}

void alt_tab::on_window_closed(int id) {
    
}

void paint_tab_option(Container *actual_root, Container *c) {
    if (get_current_time_in_ms() - show_time < show_delay)
        return;

    auto backup = c->real_bounds;
    c->real_bounds.y += titlebar_h;
    c->real_bounds.h -= titlebar_h;
    defer(c->real_bounds = backup);
    
    auto root = get_rendering_root();
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);

    if (stage != (int) STAGE::RENDER_POST_WINDOWS)
        return;
    renderfix
    if (*datum<bool>(c, "dragging")) {
        auto current_m = mouse();
        auto start_m = *datum<Bounds>(c, "drag_start");
        auto diff_x = current_m.x - start_m.x; 
        auto diff_y = current_m.y - start_m.y;
        c->real_bounds.x += diff_x * s;
        c->real_bounds.y += diff_y * s;
        hypriso->damage_entire(rid);
    }

    int real_active_index = active_index % c->parent->children.size();
    int index = 0;
    for (int i = 0; i < c->parent->children.size(); i++) {
        if (c->parent->children[i] == c) {
            index = i;
            break;
        }
    }

    //rect(c->real_bounds, {1, 0, 1, 1});
    auto cid = *datum<int>(c, "cid");
    hypriso->draw_thumbnail(cid, c->real_bounds, hypriso->get_rounding(cid) * s, 2.0f, 3);
    auto above = c->real_bounds;
    above.y -= std::round(titlebar_h * s);
    above.h = std::round(titlebar_h * s);
    c->real_bounds = above;
    
    if (real_active_index == index) {
        rect(c->real_bounds, color_titlebar_focused(), 12, hypriso->get_rounding(cid) * s, 2.0f, false, 0.0);
    } else {
        rect(c->real_bounds, color_titlebar_unfocused(), 12, hypriso->get_rounding(cid) * s, 2.0f, false, 0.0);
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
        if (real_active_index == index || true) {
            focus_alpha = color_titlebar_text_focused().a;
        } else {
            focus_alpha = color_titlebar_text_unfocused().a;
        }
        clip(to_parent(root, c), s);
        draw_texture(*info, c->real_bounds.x + 8 * s, center_y(c, info->h), 1.0 * focus_alpha);
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
        if (real_active_index != index)
            texture_info = unfocused;

        if (texture_info->id != -1) {
            auto overflow = std::max((c->real_bounds.h - texture_info->h), 0.0);
            auto overflow_amount = std::max((c->real_bounds.h - texture_info->h), 0.0);
            if (icon_width != 0)
                overflow = icon_width + 16 * s;

            auto clip_w = c->real_bounds.w - overflow - overflow_amount;
            if (clip_w > 0) {
                draw_texture(*texture_info, 
                    above.x + overflow, above.y + above.h * .5 - texture_info->h * .5, 1.0, clip_w);
            }
        }
    }

    c->real_bounds = backup;

    bool any_hovered = false;
    for (auto ch : c->parent->children) {
        if (ch->state.mouse_hovering)
            any_hovered = true;
    }

    if (real_active_index == index) {
        Bounds bo = c->real_bounds;
        bo.scale(s);
        bo.grow(std::round(3 * s));
        bo.round();
        auto bb = bo;
        bo.grow(std::round(2 * s));
        bo.round();
        auto rounding = hypriso->get_rounding(cid);
        hypriso->clip = false;
        border(bo, {.1, .6, .84, 1}, std::round(2 * s), 0, rounding * s);
        border(bb, {0, 0, 0, 1}, std::round(2 * s), 0, rounding * s);
    }
}

void create_tab_option(int cid, Container *parent) {
    bool is_first = parent->children.size() <= 1;
    auto c = parent->child(::absolute, FILL_SPACE, FILL_SPACE);
    if (is_first) {
        std::weak_ptr<bool> r = c->lifetime;
        auto timer = later(1000.0f / 30.0f, [cid, r](Timer *timer) {
            if (!r.lock())
                timer->keep_running = false;
            hypriso->screenshot(cid);
        });
        timer->keep_running = true;
    }
    
    *datum<int>(c, "cid") = cid;
    c->when_paint = paint_tab_option;
    c->receive_events_even_if_obstructed_by_one = true;
    c->when_drag_end_is_click = false;
    c->when_mouse_enters_container = paint {
        int index = 0;
        for (int i = 0; i < c->parent->children.size(); i++) {
            if (c->parent->children[i] == c) {
                index = i;
                break;
            }
        }
        active_index = index;
    };
    c->when_clicked = paint {
        int index = 0;
        for (int i = 0; i < c->parent->children.size(); i++) {
            if (c->parent->children[i] == c) {
                index = i;
                break;
            }
        }
        active_index = index;

        later_immediate([](Timer *) {
            alt_tab::close(true);
        });
    };
    c->when_drag_start = paint {
        //*datum<bool>(c, "dragging") = true;
        //*datum<Bounds>(c, "drag_start") = mouse();
    };
    c->when_drag = paint {
        
    };
    c->when_drag_end = paint {
        //*datum<bool>(c, "dragging") = false;
    };

    auto close = c->child(FILL_SPACE, FILL_SPACE);
    close->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        renderfix

        int index = 0;
        for (int i = 0; i < c->parent->parent->children.size(); i++) {
            if (c->parent->parent->children[i] == c->parent) {
                index = i;
                break;
            }
        }

        if (active_index != index)
            return;
        if (c->state.mouse_pressing) {
            rect(c->real_bounds, titlebar_closed_button_bg_pressed_color(), 13, 10 * s, 2.0);
        } else if (c->state.mouse_hovering) {
            rect(c->real_bounds, titlebar_closed_button_bg_hovered_color(), 13, 10 * s, 2.0);
        } else {
            rect(c->real_bounds, color_titlebar_focused(), 13, 10 * s, 2.0);
        }

        auto icon = "\ue8bb";
        auto closed = get_cached_texture(root, root, "close_close_invariant", "Segoe Fluent Icons", 
            icon, titlebar_closed_button_icon_color_hovered_pressed(), titlebar_button_icon_h());

        auto texture_info = closed;
        if (texture_info->id != -1) {
            clip(to_parent(root, c), s);
            draw_texture(*texture_info, center_x(c, texture_info->w), center_y(c, texture_info->h), 1.0);
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
    close->when_clicked = [cid](Container *root, Container *c) {
        later_immediate([cid](Timer *) { close_window(cid); });
    };
}

Bounds position_tab_options(Container *parent, int max_row_width) {
    int x = 0;
    int y = 0;
    int pad = 10;
    
    auto root = get_rendering_root();
    if (!root) return {0, 0, 1280, 720};
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);

    std::vector<Item> items;
    for (auto ch : parent->children) {
        auto cid = *datum<int>(ch, "cid");
        auto size = hypriso->thumbnail_size(cid);
        Item item;
        item.aspectRatio = size.w / (size.h + titlebar_h);
        items.push_back(item);
    }

    auto reserved = bounds_reserved_monitor(rid);
    LayoutParams params {
        .availableWidth = (int) reserved.w - 40,
        .availableHeight = (int) reserved.h - 40,
        .horizontalSpacing = (int) (12 * s),
        .verticalSpacing = (int) (12 * s),
        .margin = (int) (40 * s),
        .maxThumbWidth = (int) (350 * s * .85),
        .densityPresets = {
            { 4, (int) (200 * s * .85) },
            { 9, (int) (166 * s * .85)},
            { 16, (int) (133 * s * .85) },
            { INT_MAX, (int) (100 * s * .85) }
        }
    };

    auto result = layoutAltTabThumbnails(params, items);
    for (int i = 0; i < parent->children.size(); i++) {
        auto ch = parent->children[i];
        ch->wanted_bounds = result.items[i];
        ch->wanted_bounds.x += 20;
        ch->real_bounds = result.items[i];
        ch->real_bounds.x += 20;
        ::layout(actual_root, ch, ch->real_bounds);
    }

    return result.bounds;
}


void alt_tab_parent_pre_layout(Container *actual_root, Container *c, const Bounds &b) {
    Container *root = nullptr;
    auto creation_monitor = *datum<int>(c, "creation_monitor");
    for (auto m : actual_monitors) {
        if (*datum<int>(m, "cid") == creation_monitor) {
            root = m;
            break;
        }
    }
    if (!root) return;
    auto [rid, s, stage, active_id] = roots_info(actual_root, root);

    Bounds thumb_bounds;
    {  // Populate 
        auto order = get_window_stacking_order();
        // remove if no longer exists
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

        // add if doesn't exist yet
        for (int i = order.size() - 1; i >= 0; i--) {
            auto option = order[i];
            if (hypriso->alt_tabbable(option)) {
                bool found = false;
                for (auto child : c->children) {
                    if (option == *datum<int>(child, "cid")) {
                        found = true;
                    }
                }
                if (!found) {
                    create_tab_option(option, c);
                    request_damage(actual_root, c);
                }
            }
        }

        for (int index = 0; index < order.size(); index++) {
            for (int i = c->children.size() - 1; i >= 0; i--) {
                if (order[index] == (*datum<int>(c->children[i], "cid"))) {
                    *datum<int>(c->children[i], "stacking_index") = index;
                    *datum<long>(c->children[i], LAST_TIME_ACTIVE) = *datum<long>(get_cid_container(order[index]), LAST_TIME_ACTIVE);
                }
            }
        }

        // sort based on stacking order, but prefer recent activation time
        std::sort(c->children.begin(), c->children.end(), [](Container* a, Container* b) { 
            // not the cid_container so failing
            auto a_active = *datum<long>(a, LAST_TIME_ACTIVE);
            auto b_active = *datum<long>(b, LAST_TIME_ACTIVE);
            if (a_active == b_active) {
                auto a_index = *datum<int>(a, "stacking_index");
                auto b_index = *datum<int>(b, "stacking_index");
                return b_index < a_index;
            } else {
                return a_active > b_active;
            }
        });

        thumb_bounds = position_tab_options(c, root->real_bounds.w * .6); 
    }

    //c->wanted_bounds = Bounds(center_x(root, 600), center_y(root, 450), 600, 450);
    c->wanted_bounds = Bounds(thumb_bounds.x, thumb_bounds.y, thumb_bounds.w, thumb_bounds.h);
    c->real_bounds = c->wanted_bounds;
    c->real_bounds.x += 20;
    //modify_all(c, center_x(root, c->real_bounds.w), center_y(root, c->real_bounds.h));
    c->real_bounds.grow(20);

    // Don't draw if tab option fully outside
    //for (auto child : c->children) {
        //child->exists = overlaps(c->real_bounds, child->real_bounds);
    //}   
    //::layout(actual_root, c, c->real_bounds); 
}

void fill_root(Container *root, Container *alt_tab_parent) {
    *datum<bool>(alt_tab_parent, "shown_yet") = false;
    *datum<int>(alt_tab_parent, "creation_monitor") = hypriso->monitor_from_cursor();
    alt_tab_parent->custom_type = (int) TYPE::ALT_TAB;
    alt_tab_parent->type = ::vbox;
    alt_tab_parent->receive_events_even_if_obstructed = true;
    alt_tab_parent->automatically_paint_children = false;
    alt_tab_parent->when_mouse_down = consume_event;
    alt_tab_parent->when_mouse_motion = consume_event;
    alt_tab_parent->when_drag = consume_event;
    alt_tab_parent->when_mouse_up = consume_event;
    alt_tab_parent->when_mouse_enters_container = paint {
        hypriso->send_false_position(-1, -1);
        //hypriso->all_lose_focus();
        //notify("enteres");
        consume_event(root, c);
    };
    alt_tab_parent->when_mouse_leaves_container = paint {
        //hypriso->all_gain_focus();
        //notify("leaves");
        consume_event(root, c);
    };
    alt_tab_parent->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        alt_tab_parent_pre_layout(actual_root, c, b); 
    };
    alt_tab_parent->when_paint = [](Container *actual_root, Container *c) {
        if (get_current_time_in_ms() - show_time < show_delay) {
            request_damage(actual_root, c);
            return;
        }
        auto root = get_rendering_root();
        if (!root) return;

        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS)
            return;
        if (rid != *datum<int>(c, "creation_monitor"))
            return;
        c->automatically_paint_children = true;
        renderfix

        bool any_subpart_damaged = false;
        auto shown_yet = datum<bool>(c, "shown_yet");
        if (any_subpart_damaged || !(*shown_yet)) {
            request_damage(root, c); // request full redamage
        }
        *shown_yet = true;
        if (c->state.mouse_pressing) {
            //rect(c->real_bounds, {1, 1, 1, 1});
        } else if (c->state.mouse_hovering) {
            //rect(c->real_bounds, {1, 0, 1, 1});
        }
        auto b = c->real_bounds; 
        b.grow(10 * s);
        rect(b, {1, 1, 1, .4}, 0, 8 * s, 2.0);
    };
    alt_tab_parent->after_paint = [](Container *actual_root, Container *c) {
        c->automatically_paint_children = false;
    };
}

static bool is_showing = false;

void alt_tab::show() {
    //return;
    if (is_showing)
        return;
    if (drag::dragging()) {
        drag::end(drag::drag_window());
    }
    show_time = get_current_time_in_ms();
    active_index = 0;
    is_showing = true;
    later_immediate([](Timer *) {
        hypriso->screenshot_all(); 
    });
    {
        auto m = actual_root;
        auto alt_tab_parent = m->child(FILL_SPACE, FILL_SPACE);
        fill_root(m, alt_tab_parent);
    }
    for (auto m : actual_monitors) {
        hypriso->damage_entire(*datum<int>(m, "cid"));
    }
}

void alt_tab::close(bool focus) {
    if (!is_showing)
        return;
    is_showing = false;
    {
        auto m = actual_root;
        for (int i = m->children.size() - 1; i >= 0; i--) {
            auto c = m->children[i];
            if (c->custom_type == (int) TYPE::ALT_TAB) {
                if (focus) {
                    if (!c->children.empty()) {
                        int real_active_index = active_index % c->children.size();
                        auto cid = *datum<int>(c->children[real_active_index], "cid"); 
                        hypriso->set_hidden(cid, false);
                        hypriso->bring_to_front(cid);
                    }
                }
                request_damage(m, c);
                delete c;
                m->children.erase(m->children.begin() + i);
                hypriso->simulateMouseMovement();
            }
        }
    }
}

void alt_tab::move(int dir) {
    active_index += dir; 
    for (auto c : actual_root->children) {
        if (c->custom_type == (int) TYPE::ALT_TAB) {
            request_damage(actual_root, c);
            break;
        }
    }
}

void alt_tab::on_activated(int id) {
    Container *c = get_cid_container(id);
    if (*datum<bool>(c, "eat")) {
        *datum<bool>(c, "eat") = false;
        return;
    }

    assert(c && "alt_tab::on_activated assumes Container for id exists");    

    for (auto g : ((ClientInfo *)c->user_data)->grouped_with) {
        *datum<bool>(get_cid_container(id), "eat") = true;
        hypriso->bring_to_front(g, false);
    }

    auto current = get_current_time_in_ms();
    *datum<long>(c, LAST_TIME_ACTIVE) = current;
}

bool alt_tab::showing() {
    return showing;
}
