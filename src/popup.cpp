#include "popup.h"

#include "heart.h"
#include "icons.h"

#include <algorithm>
#include <unordered_set>
#include <linux/input-event-codes.h>

namespace {

struct PopOptionData : UserData {
    PopOption p;
};

Container *find_popup(const std::string& uuid) {
    for (auto *child : actual_root->children)
        if (child->custom_type == (int)TYPE::OUR_POPUP && child->uuid == uuid)
            return child;
    return nullptr;
}

PopupUserData *popup_data(Container *panel) {
    return panel ? (PopupUserData *) panel->user_data : nullptr;
}

void update_popup_cursor() {
    for (auto *panel : actual_root->children) {
        if (panel->custom_type != (int)TYPE::OUR_POPUP || !panel->exists)
            continue;
        if (*datum<bool>(panel, "setting_cursor")) {
            setCursorImageUntilUnset("default");
            return;
        }
    }
    unsetCursorImage(true);
}

void close_now(const std::string& uuid) {
    auto *panel = find_popup(uuid);
    if (!panel)
        return;
    if (auto *data = popup_data(panel)) {
        const auto child_uuid = data->child_uuid;
        if (!child_uuid.empty())
            close_now(child_uuid);
        if (auto *parent = popup_data(find_popup(data->parent_uuid)))
            if (parent->child_uuid == uuid)
                parent->child_uuid.clear();
    }
    std::erase(actual_root->children, panel);
    delete panel;
}

void mark_closing(Container *panel) {
    if (!panel)
        return;
    panel->exists = false;
    if (auto *data = popup_data(panel)) {
        data->closing = true;
        data->requested_row_uuid.clear();
        mark_closing(find_popup(data->child_uuid));
    }
}

Container *build_menu(const std::vector<PopOption>& options, int x, int y, int mid, int cid,
                      Container *parent = nullptr, Container *owner = nullptr);

void request_submenu(Container *row) {
    auto *panel = row->parent;
    auto *data = popup_data(panel);
    if (!data || data->closing)
        return;
    data->requested_row_uuid = row->uuid;
    // Event dispatch holds container snapshots. Replace branches after it finishes.
    later_immediate([panel_uuid = panel->uuid, row_uuid = row->uuid](Timer *) {
        auto *panel = find_popup(panel_uuid);
        auto *data = popup_data(panel);
        if (!data || data->closing || data->requested_row_uuid != row_uuid)
            return;
        Container *row = nullptr;
        for (auto *candidate : panel->children)
            if (candidate->uuid == row_uuid)
                row = candidate;
        if (!row)
            return;
        if (auto *child = popup_data(find_popup(data->child_uuid)))
            if (!child->closing && child->owner_row_uuid == row_uuid)
                return;
        const auto old_child = data->child_uuid;
        close_now(old_child);
        auto *option = (PopOptionData *)row->user_data;
        if (option && !option->p.submenu.empty()) {
            auto *child = build_menu(option->p.submenu, panel->real_bounds.right(), row->real_bounds.y,
                                     data->mid, data->cid, panel, row);
            data->child_uuid = child->uuid;
        }
        damage_all();
    });
}

} // namespace

void popup::close(std::string uuid) {
    mark_closing(find_popup(uuid));
    later_immediate([uuid](Timer *) {
        close_now(uuid);
        damage_all();
    });
}

bool popup::dismiss_outside(int x, int y) {
    std::unordered_set<std::string> inside;
    std::vector<std::string> outside;
    for (auto *panel : actual_root->children) {
        if (panel->custom_type != (int)TYPE::OUR_POPUP || !panel->exists)
            continue;
        if (bounds_contains(panel->real_bounds, x, y)) {
            auto *data = popup_data(panel);
            inside.insert(data ? data->root_uuid : panel->uuid);
        }
    }
    for (auto *panel : actual_root->children) {
        if (panel->custom_type != (int)TYPE::OUR_POPUP)
            continue;
        auto *data = popup_data(panel);
        if (!inside.contains(data ? data->root_uuid : panel->uuid))
            outside.push_back(panel->uuid);
    }
    // Called before mouse event dispatch starts, so immediate deletion is safe.
    for (const auto& uuid : outside)
        close_now(uuid);
    return !outside.empty();
}

namespace {

Container *build_menu(const std::vector<PopOption>& options, int x, int y, int mid, int cid,
                      Container *parent, Container *owner) {
    // Container geometry is logical; painting applies the monitor scale once.
    auto s = scale(mid);
    float option_height = 24 * s;
    float seperator_size = 5 * s;
    float height = 0;
    for (const auto& option : options)
        height += option.seperator ? seperator_size : option_height;

    auto p = actual_root->child(::vbox, 277, height);
    p->z_index = parent ? parent->z_index + 1 : 100;
    consume_everything(p);
    p->receive_events_even_if_obstructed = true;
    p->custom_type = (int)TYPE::OUR_POPUP;
    auto pud = new PopupUserData;
    pud->mid = mid;
    pud->cid = cid;
    pud->root_uuid = parent ? popup_data(parent)->root_uuid : p->uuid;
    pud->parent_uuid = parent ? parent->uuid : "";
    pud->owner_row_uuid = owner ? owner->uuid : "";
    p->user_data = pud;
    const auto mb = bounds_reserved_monitor(mid);
    if (parent && x + p->wanted_bounds.w > mb.right())
        x = parent->real_bounds.x - p->wanted_bounds.w;
    p->real_bounds = Bounds(
        std::clamp<double>(x, mb.x, std::max(mb.x, mb.right() - p->wanted_bounds.w)),
        std::clamp<double>(y, mb.y, std::max(mb.y, mb.bottom() - height)),
        p->wanted_bounds.w, height);

    p->on_closed = [](Container *c) {
        auto *data = popup_data(c);
        if (!data->child_uuid.empty())
            popup::close(data->child_uuid);
        *datum<bool>(c, "setting_cursor") = false;
        update_popup_cursor();
    };
    *datum<bool>(p, "setting_cursor") = false;
    p->when_mouse_enters_container = paint {
        //hypriso->all_lose_focus();
        *datum<bool>(c, "setting_cursor") = true;
        setCursorImageUntilUnset("default");
        hypriso->send_false_position(-1, -1);
        consume_event(root, c);
    };
    p->when_mouse_leaves_container = paint {
        *datum<bool>(c, "setting_cursor") = false;
        //hypriso->all_gain_focus();
        update_popup_cursor();
        consume_event(root, c);
    };
    p->when_mouse_down = paint {
        consume_event(root, c);
    };
    p->when_mouse_up = paint {
        consume_event(root, c);
    };

    //p->wanted_pad = Bounds(7, 7, 7, 7);
    p->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int) STAGE::RENDER_POST_WINDOWS) {
            renderfix
            auto pud = (PopupUserData *) c->user_data;
            auto b = c->real_bounds;
            render_drop_shadow(rid, 1.0, {0, 0, 0, 1.0f}, 7 * s, 2.0f, b);
            rect(b, {1, 1, 1, .75}, 0, 7 * s, 2.0, true);
        }
    };
    p->after_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int) STAGE::RENDER_POST_WINDOWS) {
            renderfix
            auto b = c->real_bounds;
            b.shrink(1);
            border(b, {0, 0, 0, .2}, 1, 0, 7 * s);
        }
    };
    
    p->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        ::layout(actual_root, c, c->real_bounds);
    };
    for (const auto& pop_option : options) {
        auto option = p->child(FILL_SPACE, option_height);
        option->when_mouse_enters_container = [](Container *root, Container *c) {
            consume_event(root, c);
            request_submenu(c);
        };
        if (pop_option.seperator) {
            option->wanted_bounds.h = seperator_size;
            option->when_paint = [](Container *actual_root, Container *c) {
                auto root = get_rendering_root();
                if (!root)
                    return;
                auto [rid, s, stage, active_id] = roots_info(actual_root, root);
                if (stage == (int)STAGE::RENDER_POST_WINDOWS) {
                    renderfix
                    auto b = c->real_bounds;
                    b.y += std::floor(b.h * .5);
                    b.h = 1.0;
                    b.x += 8 * s;
                    b.w -= 16 * s;
                    rect(b, {0, 0, 0, 0.3}, 0, 1 * s, 2.0, false);
                }
            };
            continue;
        }
        auto popdata = new PopOptionData;
        popdata->p = pop_option;
        option->user_data = popdata;
        option->when_paint = [](Container *actual_root, Container *c) {
            auto root = get_rendering_root();
            if (!root) return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            if (stage == (int) STAGE::RENDER_POST_WINDOWS) {
                renderfix

                auto *child = popup_data(find_popup(popup_data(c->parent)->child_uuid));
                const bool expanded = child && !child->closing && child->owner_row_uuid == c->uuid;
                if (c->state.mouse_hovering || expanded) {
                    auto b = c->real_bounds;
                    rect(b, {0, 0, 0, .1}, 0, 7 * s, 2.0f, false);
                }
                if (c->state.mouse_button_pressed == BTN_LEFT) {
                    if (c->state.mouse_pressing) {
                        auto b = c->real_bounds;
                        rect(b, {0, 0, 0, .2}, 0, 7 * s, 2.0, false);
                    }
                }

                auto popdata = (PopOptionData*)c->user_data;
                auto& pop_option = popdata->p;

                if (!pop_option.has_attempted_loadin_icon) {
                    pop_option.has_attempted_loadin_icon = true;
                    if (!pop_option.icon_left.empty()) {
                        if (pop_option.is_text_icon) {

                        } else {
                            auto icon = pop_option.icon_left;
                            pop_option.icon_path= one_shot_icon(14 * s, {icon, to_lower(icon), c3ic_fix_wm_class(icon), to_lower(icon)});
                        }
                    }
                }
                if (pop_option.checked) {
                    auto info = gen_text_texture(mylar_font, "✓", 14 * s, {0, 0, 0, 1});
                    draw_texture(info, c->real_bounds.x + 20 * s - info.w * .5, center_y(c, info.h));
                    free_text_texture(info.id);
                } else if (pop_option.is_text_icon) {
                    auto info = gen_text_texture("Segoe Fluent Icons", pop_option.icon_left, 14 * s, {0, 0, 0, 1});
                    draw_texture(info, c->real_bounds.x + (40 * s * .5) - info.w * .5, center_y(c, info.h));
                    free_text_texture(info.id);
                } else if (!pop_option.icon_path.empty()) {
                    auto info = gen_texture(pop_option.icon_path, 18 * s);
                    draw_texture(info, c->real_bounds.x + (40 * s * .5) - info.w * .5, center_y(c, info.h));
                    free_text_texture(info.id);
                }

                if (!pop_option.submenu.empty()) {
                    auto arrow = gen_text_texture(mylar_font, "›", 18 * s, {0, 0, 0, 1});
                    draw_texture(arrow, c->real_bounds.right() - 16 * s - arrow.w * .5, center_y(c, arrow.h));
                    free_text_texture(arrow.id);
                }
                auto info = gen_text_texture(mylar_font, pop_option.text, 14 * s, {0, 0, 0, 1},
                                             c->real_bounds.w - 72 * s, c->real_bounds.h);
                draw_texture(info, c->real_bounds.x + 40 * s, center_y(c, info.h));
                free_text_texture(info.id);
            }
        };
        option->when_clicked = paint {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            auto *data = popup_data(c->parent);
            if (data->closing)
                return;
            consume_event(root, c);
            auto pop_option = ((PopOptionData *)c->user_data)->p;
            if (!pop_option.submenu.empty()) {
                request_submenu(c);
                return;
            }
            const auto root_uuid = data->root_uuid;
            if (pop_option.on_clicked)
                pop_option.on_clicked();
            if (pop_option.closes_on_click)
                popup::close(root_uuid);
        };
    }
    ::layout(actual_root, p, p->real_bounds);
    return p;
}

} // namespace

void popup::open(std::vector<PopOption> root, int x, int y, int cid) {
    if (root.empty())
        return;
    build_menu(root, x, y, hypriso->monitor_from_cursor(), cid);
    if (hypriso->on_mouse_move) {
        const auto m = mouse();
        hypriso->on_mouse_move(0, m.x, m.y);
    }
    damage_all();
}
