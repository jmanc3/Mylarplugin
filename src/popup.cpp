#include "popup.h"

#include "heart.h"
#include "icons.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <linux/input-event-codes.h>

namespace {

struct PopOptionTextures {
    float scale = -1.0f;
    float max_w = -1.0f;
    float max_h = -1.0f;

    bool left_loaded = false;
    TextureInfo left;

    bool arrow_loaded = false;
    TextureInfo arrow;

    bool label_loaded = false;
    TextureInfo label;

    void clear() {
        if (left.id != -1) {
            free_text_texture(left.id);
            left = {};
        }
        if (arrow.id != -1) {
            free_text_texture(arrow.id);
            arrow = {};
        }
        if (label.id != -1) {
            free_text_texture(label.id);
            label = {};
        }
        left_loaded = false;
        arrow_loaded = false;
        label_loaded = false;
        scale = -1.0f;
        max_w = -1.0f;
        max_h = -1.0f;
    }
};

struct PopOptionData : UserData {
    PopOption p;
    std::unordered_map<int, PopOptionTextures> textures;

    ~PopOptionData() override {
        for (auto &[rid, tex] : textures)
            tex.clear();
    }
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

constexpr int kAboveOverlaysZ = 3000;

int paint_stage(Container *panel) {
    auto *data = popup_data(panel);
    return data && data->above_overlays ? (int) STAGE::RENDER_PRE_CURSOR : (int) STAGE::RENDER_POST_WINDOWS;
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
    const bool any_popup = std::any_of(actual_root->children.begin(), actual_root->children.end(), [](Container *c) {
        return c->custom_type == (int)TYPE::OUR_POPUP;
    });
    if (!any_popup)
        set_input_takeover("popup", false);
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
    later(150, [panel_uuid = panel->uuid, row_uuid = row->uuid](Timer *) {
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

bool popup::is_open() {
    for (auto *panel : actual_root->children) {
        if (panel->custom_type == (int)TYPE::OUR_POPUP && panel->exists)
            return true;
    }
    return false;
}

void popup::set_above_overlays() {
    for (auto *panel : actual_root->children) {
        if (panel->custom_type != (int)TYPE::OUR_POPUP)
            continue;
        auto *data = popup_data(panel);
        if (!data || data->above_overlays)
            continue;
        data->above_overlays = true;
        panel->z_index += kAboveOverlaysZ;
    }
    damage_all();
}

bool popup::contains(int x, int y) {
    for (auto *panel : actual_root->children) {
        if (panel->custom_type == (int)TYPE::OUR_POPUP && panel->exists && bounds_contains(panel->real_bounds, x, y))
            return true;
    }
    return false;
}

static constexpr float OPTION_HEIGHT = 36;
static constexpr float SEPERATOR_SIZE = 5;
static constexpr float TASKS_HEADER_HEIGHT = 34;
static constexpr float TASKS_SECTION_GAP = 14;
static constexpr float TASKS_LEFT_PAD = 14;

float popup::menu_height(const std::vector<PopOption>& options) {
    float height = 0;
    for (const auto& option : options)
        height += option.seperator ? SEPERATOR_SIZE : OPTION_HEIGHT;
    return height;
}

float popup::tasks_menu_height(const std::vector<PopOption>& tasks, const std::vector<PopOption>& options) {
    float height = menu_height(options);
    if (!tasks.empty())
        height += TASKS_HEADER_HEIGHT + menu_height(tasks) + (options.empty() ? 0 : TASKS_SECTION_GAP);
    return height;
}

namespace {

Container *create_menu_panel(float height, int x, int y, int mid, int cid,
                             Container *parent = nullptr, Container *owner = nullptr) {
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
    pud->above_overlays = parent && popup_data(parent)->above_overlays;
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
        if (stage == paint_stage(c)) {
            renderfix
            auto b = c->real_bounds;
            render_drop_shadow(rid, 1.0, {0, 0, 0, .0108f}, 7 * s, 2.0f, b, std::round(16 * s));
            rect(b, {1, 1, 1, .75}, 0, 7 * s, 2.0, true);
        }
    };
    p->after_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == paint_stage(c)) {
            renderfix
            auto b = c->real_bounds;
            b.shrink(1);
            border(b, {0, 0, 0, .2}, 1, 0, 7 * s);
        }
    };

    p->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        ::layout(actual_root, c, c->real_bounds);
    };
    return p;
}

void add_option_row(Container *p, const PopOption& pop_option) {
    auto option = p->child(FILL_SPACE, OPTION_HEIGHT);
    option->when_mouse_enters_container = [](Container *root, Container *c) {
        consume_event(root, c);
        request_submenu(c);
    };
    if (pop_option.seperator) {
        option->wanted_bounds.h = SEPERATOR_SIZE;
        option->when_paint = [](Container *actual_root, Container *c) {
            auto root = get_rendering_root();
            if (!root)
                return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            auto *pdata = popup_data(c->parent);
            if (!pdata || rid != pdata->mid) return;
            if (stage == paint_stage(c->parent)) {
                renderfix
                auto b = c->real_bounds;
                b.y += std::floor(b.h * .5);
                b.h = 1.0;
                b.x += 8 * s;
                b.w -= 16 * s;
                rect(b, {0, 0, 0, 0.3}, 0, 1 * s, 2.0, false);
            }
        };
        return;
    }
    auto popdata = new PopOptionData;
    popdata->p = pop_option;
    option->user_data = popdata;
    option->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        auto *pdata = popup_data(c->parent);
        if (!pdata || rid != pdata->mid) return;
        if (stage == paint_stage(c->parent)) {
            renderfix

            auto *child = popup_data(find_popup(pdata->child_uuid));
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
                if (!pop_option.icon_left.empty() && !pop_option.is_text_icon) {
                    auto icon = pop_option.icon_left;
                    pop_option.icon_path = one_shot_icon(14 * s, {icon, to_lower(icon), c3ic_fix_wm_class(icon), to_lower(icon)});
                }
            }

            auto& tex = popdata->textures[rid];
            if (tex.scale != s) {
                tex.clear();
                tex.scale = s;
            }

            if (!tex.left_loaded) {
                tex.left_loaded = true;
                if (pop_option.checked) {
                    tex.left = gen_text_texture(set->font, "✓", 14 * s, {0, 0, 0, 1});
                } else if (pop_option.is_text_icon) {
                    tex.left = gen_text_texture(icon_font, pop_option.icon_left, 14 * s, {0, 0, 0, 1});
                } else if (!pop_option.icon_path.empty()) {
                    tex.left = gen_texture(pop_option.icon_path, 18 * s);
                }
            }
            if (tex.left.id != -1) {
                if (pop_option.checked) {
                    draw_texture(tex.left, c->real_bounds.x + 20 * s - tex.left.w * .5, center_y(c, tex.left.h));
                } else {
                    draw_texture(tex.left, c->real_bounds.x + (40 * s * .5) - tex.left.w * .5, center_y(c, tex.left.h));
                }
            }

            if (!pop_option.submenu.empty()) {
                if (!tex.arrow_loaded) {
                    tex.arrow_loaded = true;
                    tex.arrow = gen_text_texture(set->font, "›", 18 * s, {0, 0, 0, 1});
                }
                if (tex.arrow.id != -1) {
                    draw_texture(tex.arrow, c->real_bounds.right() - 16 * s - tex.arrow.w * .5, center_y(c, tex.arrow.h));
                }
            }

            constexpr float text_x = 40.0f;
            const float right_pad = pop_option.submenu.empty() ? 16.0f : 32.0f;
            const float label_max_w = c->real_bounds.w - (text_x + right_pad) * s;
            const float label_max_h = c->real_bounds.h;
            if (!tex.label_loaded || tex.max_w != label_max_w || tex.max_h != label_max_h) {
                if (tex.label.id != -1) {
                    free_text_texture(tex.label.id);
                    tex.label = {};
                }
                tex.label_loaded = true;
                tex.max_w = label_max_w;
                tex.max_h = label_max_h;
                tex.label = gen_text_texture(set->font, pop_option.text, 14 * s, {0, 0, 0, 1}, label_max_w, label_max_h);
            }
            if (tex.label.id != -1) {
                draw_texture(tex.label, c->real_bounds.x + text_x * s, center_y(c, tex.label.h));
            }
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

Container *build_menu(const std::vector<PopOption>& options, int x, int y, int mid, int cid,
                      Container *parent, Container *owner) {
    // Container geometry is logical; painting applies the monitor scale once.
    // Heights are logical like the width (277): scaling them here as well
    // made rows twice as tall at scale 2 (painting scales them again).
    auto p = create_menu_panel(popup::menu_height(options), x, y, mid, cid, parent, owner);
    for (const auto& pop_option : options)
        add_option_row(p, pop_option);
    ::layout(actual_root, p, p->real_bounds);
    return p;
}

Container *build_tasks_menu(const std::string& tasks_title,
                            const std::vector<PopOption>& tasks,
                            const std::vector<PopOption>& options,
                            int x, int y, int mid, int cid) {
    auto p = create_menu_panel(popup::tasks_menu_height(tasks, options), x, y, mid, cid);
    if (!tasks.empty()) {
        auto header = p->child(FILL_SPACE, TASKS_HEADER_HEIGHT);
        header->user_data = new PopOptionData;
        header->when_mouse_enters_container = paint {
            consume_event(root, c);
        };
        header->when_paint = [tasks_title](Container *actual_root, Container *c) {
            auto root = get_rendering_root();
            if (!root)
                return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            auto *pdata = popup_data(c->parent);
            if (!pdata || rid != pdata->mid) return;
            if (stage == paint_stage(c->parent)) {
                renderfix
                auto *header_data = (PopOptionData *)c->user_data;
                auto &tex = header_data->textures[rid];
                const float max_w = c->real_bounds.w - (TASKS_LEFT_PAD * 2) * s;
                const float max_h = c->real_bounds.h;
                if (tex.scale != s) {
                    tex.clear();
                    tex.scale = s;
                }
                if (!tex.label_loaded || tex.max_w != max_w || tex.max_h != max_h) {
                    if (tex.label.id != -1) {
                        free_text_texture(tex.label.id);
                        tex.label = {};
                    }
                    tex.label_loaded = true;
                    tex.max_w = max_w;
                    tex.max_h = max_h;
                    tex.label = gen_text_texture(set->font, tasks_title, 13 * s, {0, 0, 0, .58}, max_w, max_h);
                }
                if (tex.label.id != -1) {
                    draw_texture(tex.label, c->real_bounds.x + TASKS_LEFT_PAD * s, c->real_bounds.bottom() - tex.label.h - 6 * s);
                }
            }
        };
        for (const auto& task : tasks)
            add_option_row(p, task);
        if (!options.empty()) {
            auto gap = p->child(FILL_SPACE, TASKS_SECTION_GAP);
            gap->when_mouse_enters_container = paint {
                consume_event(root, c);
            };
        }
    }
    for (const auto& pop_option : options)
        add_option_row(p, pop_option);
    ::layout(actual_root, p, p->real_bounds);
    return p;
}

void finish_open() {
    // A menu can open over a fullscreen game (titlebar right click), which
    // would otherwise keep the hidden cursor, its pointer lock and the keys.
    set_input_takeover("popup", true);
    if (hypriso->on_mouse_move) {
        const auto m = mouse();
        hypriso->on_mouse_move(0, m.x, m.y);
    }
    damage_all();
}

} // namespace

void popup::open(std::vector<PopOption> root, int x, int y, int cid) {
    if (root.empty())
        return;
    build_menu(root, x, y, hypriso->monitor_from_cursor(), cid);
    finish_open();
}

void popup::open_tasks_menu(const std::string& tasks_title,
                            const std::vector<PopOption>& tasks,
                            const std::vector<PopOption>& options,
                            int x, int y, int cid) {
    if (tasks.empty() && options.empty())
        return;
    build_tasks_menu(tasks_title, tasks, options, x, y, hypriso->monitor_from_cursor(), cid);
    finish_open();
}
