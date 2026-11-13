#include "dock/dock_tiling.h"

#include "dock/dock_internal.h"
#include "settings.h"
#include "shortcuts.h"

static int tiling_workspace(const std::string &monitor_name) {
    for (auto monitor : actual_monitors) {
        const int id = *datum<int>(monitor, "cid");
        if (hypriso->monitor_name(id) == monitor_name)
            return hypriso->get_active_workspace_id(id);
    }
    return -1;
}

static std::string shortcut_keys(const std::string &id) {
    for (const auto &shortcut : shortcuts::defaults())
        if (shortcut.id == id)
            return shortcuts::keys_for(set, shortcut);
    return "";
}

static std::string pretty_keys(std::string keys) {
    if (keys.empty())
        return "Not set";
    std::string out;
    size_t start = 0;
    while (start <= keys.size()) {
        size_t end = keys.find(" + ", start);
        std::string part = keys.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "bracketleft")
            part = "[";
        else if (part == "bracketright")
            part = "]";
        else if (part.size() > 1) {
            for (auto &ch : part)
                ch = (char) std::tolower((unsigned char) ch);
            part[0] = (char) std::toupper((unsigned char) part[0]);
        } else {
            for (auto &ch : part)
                ch = (char) std::toupper((unsigned char) ch);
        }
        out += (out.empty() ? "" : " + ") + part;
        if (end == std::string::npos)
            break;
        start = end + 3;
    }
    return out;
}

// Joins shortcuts that only differ in their last key, "Super + H / L".
static std::string shared_keys(const std::vector<std::string> &ids, const std::string &shown_last = "") {
    std::vector<std::string> keys;
    for (const auto &id : ids)
        keys.push_back(pretty_keys(shortcut_keys(id)));
    std::string prefix;
    std::string lasts;
    for (size_t i = 0; i < keys.size(); i++) {
        const auto split = keys[i].rfind(" + ");
        const std::string head = split == std::string::npos ? "" : keys[i].substr(0, split);
        const std::string last = split == std::string::npos ? keys[i] : keys[i].substr(split + 3);
        if (keys[i] == "Not set" || (i > 0 && head != prefix)) {
            std::string joined;
            for (const auto &key : keys)
                joined += (joined.empty() ? "" : ", ") + key;
            return joined;
        }
        prefix = head;
        lasts += (lasts.empty() ? "" : " / ") + last;
    }
    if (!shown_last.empty())
        lasts = shown_last;
    return prefix.empty() ? lasts : prefix + " + " + lasts;
}

static std::vector<std::pair<std::string, std::string>> tiling_shortcuts(const std::string &layout) {
    std::vector<std::pair<std::string, std::string>> list = {
        // Alt + Space is handled directly in heart.cpp's key handler.
        {"Toggle tiling", "Alt + Space"},
        {"Focus window", shared_keys({"focus_left", "focus_right", "focus_up", "focus_down"}, "Arrows")},
        {"Move window", shared_keys({"move_left", "move_right", "move_up", "move_down"}, "Arrows")},
        {"Toggle floating", pretty_keys(shortcut_keys("toggle_floating"))},
        {"Drag a tiled window", "Alt + Left drag"},
    };
    if (layout == "Scrolling") {
        list.push_back({"Column width", shared_keys({"scrolling_width_next", "scrolling_width_previous"})});
        list.push_back({"Center column", pretty_keys(shortcut_keys("scrolling_center"))});
        list.push_back({"Into column left / right", shared_keys({"scrolling_consume_previous", "scrolling_consume_next"})});
        list.push_back({"Own column", pretty_keys(shortcut_keys("scrolling_promote"))});
        list.push_back({"Move column", shared_keys({"scrolling_swap_column_left", "scrolling_swap_column_right"})});
        list.push_back({"Full width column", pretty_keys(shortcut_keys("scrolling_full_width"))});
        list.push_back({"Fit visible columns", pretty_keys(shortcut_keys("scrolling_fit_visible"))});
    } else {
        list.push_back({"Focus next / previous", shared_keys({"layout_next", "layout_previous"})});
        list.push_back({"Swap with master", pretty_keys(shortcut_keys("layout_swap_master"))});
        list.push_back({"Shrink / grow master", shared_keys({"layout_shrink_master", "layout_grow_master"})});
    }
    return list;
}

// Called on the compositor thread while the dock's app mutex is held.
void update_tiling_menu(Dock *dock) {
    dock->tiling_settings = {
        .is_tiling = hypriso->is_space_tiling_id(tiling_workspace(dock->creation_settings.monitor_name)),
        .all_workspaces = set->tile_all_workspaces,
        .new_workspace_is_tiling = set->new_workspace_is_tiling,
        .border_hint = set->active_window_border_hint,
        .window_gap = set->tiling_window_gap,
        .layout = set->tiling_layout,
        .shortcuts = tiling_shortcuts(set->tiling_layout),
    };
    if (auto info = container_by_name("tiling_info", dock->window->root))
        info->exists = dock->tiling_settings.is_tiling;
    if (dock->tiling)
        windowing::redraw(dock->tiling->raw_window);
    if (dock->tiling_info)
        windowing::redraw(dock->tiling_info->raw_window);
}

static Container *tiling_row(Container *parent, double height) {
    auto row = parent->child(::hbox, FILL_SPACE, height);
    row->pre_layout = [height](Container *root, Container *c, const Bounds &) {
        const auto dock = static_cast<Dock *>(root->user_data);
        c->wanted_bounds.h = height * dock->tiling->raw_window->dpi;
    };
    return row;
}

static void paint_tiling_text(Dock *dock, Container *c, const std::string &text, bool right = false, RGBA color = {0, 0, 0, 1}) {
    const auto cr = dock->tiling->raw_window->drawing_context;
    const auto dpi = dock->tiling->raw_window->dpi;
    const auto bounds = draw_text(cr, 0, 0, text, 12 * dpi, false);
    draw_text(cr, right ? c->real_bounds.right() - bounds.w : c->real_bounds.x,
        c->real_bounds.y + (c->real_bounds.h - bounds.h) * .5, text, 12 * dpi, true, set->font, -1, -1, color);
}

static void tiling_label(Container *parent, const std::string &text, double height = 28) {
    auto row = tiling_row(parent, height);
    row->when_paint = [text](Container *root, Container *c) {
        paint_tiling_text(static_cast<Dock *>(root->user_data), c, text);
    };
}

static void tiling_toggle(Container *parent, const std::string &text, std::function<bool(const STilingMenuState &)> value,
                          std::function<void(const std::string &)> change) {
    auto row = tiling_row(parent, 44);
    auto label = row->child(FILL_SPACE, FILL_SPACE);
    label->when_paint = [text](Container *root, Container *c) {
        paint_tiling_text(static_cast<Dock *>(root->user_data), c, text);
    };
    auto toggle = row->child(44, FILL_SPACE);
    toggle->pre_layout = [](Container *root, Container *c, const Bounds &) {
        const auto dock = static_cast<Dock *>(root->user_data);
        c->wanted_bounds.w = 44 * dock->tiling->raw_window->dpi;
    };
    toggle->when_paint = [value](Container *root, Container *c) {
        const auto dock = static_cast<Dock *>(root->user_data);
        const auto dpi = dock->tiling->raw_window->dpi;
        const auto cr = dock->tiling->raw_window->drawing_context;
        const bool enabled = value(dock->tiling_settings);
        const double x = c->real_bounds.x;
        const double y = c->real_bounds.y + (c->real_bounds.h - 22 * dpi) * .5;
        set_argb(cr, enabled ? accent : RGBA(.7, .7, .7, 1));
        drawRoundedRect(cr, x, y, 44 * dpi, 22 * dpi, 11 * dpi, 1);
        cr->fill();
        set_argb(cr, {1, 1, 1, 1});
        cr->arc(x + (enabled ? 33 : 11) * dpi, y + 11 * dpi, 8 * dpi, 0, 2 * M_PI);
        cr->fill();
    };
    toggle->when_clicked = [change](Container *root, Container *) {
        const auto dock = static_cast<Dock *>(root->user_data);
        change(dock->creation_settings.monitor_name);
    };
}

static void tiling_segments(Container *parent, std::vector<std::string> options,
                            std::function<std::string(const STilingMenuState &)> selected, std::function<void(const std::string &)> change) {
    auto row = tiling_row(parent, 36);
    row->pre_layout = [](Container *root, Container *c, const Bounds &) {
        const auto dpi = static_cast<Dock *>(root->user_data)->tiling->raw_window->dpi;
        c->wanted_bounds.h = 36 * dpi;
        c->spacing = 4 * dpi;
    };
    for (const auto &option : options) {
        auto segment = row->child(FILL_SPACE, FILL_SPACE);
        segment->when_paint = [option, selected](Container *root, Container *c) {
            const auto dock = static_cast<Dock *>(root->user_data);
            const auto cr = dock->tiling->raw_window->drawing_context;
            const auto dpi = dock->tiling->raw_window->dpi;
            const bool is_selected = selected(dock->tiling_settings) == option;
            set_argb(cr, is_selected ? RGBA(.91, .96, 1, 1) : RGBA(.95, .95, .95, 1));
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 6 * dpi, 1);
            cr->fill();
            const auto bounds = draw_text(cr, 0, 0, option, 12 * dpi, false);
            draw_text(cr, c->real_bounds.x + (c->real_bounds.w - bounds.w) * .5,
                c->real_bounds.y + (c->real_bounds.h - bounds.h) * .5, option, 12 * dpi, true, set->font, -1, -1,
                is_selected ? accent : RGBA(0, 0, 0, 1));
        };
        segment->when_clicked = [option, change](Container *, Container *) {
            change(option);
        };
    }
}

void fill_tiling_container(Dock *dock) {
    auto root = dock->tiling->root;
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->tiling->raw_window->drawing_context;
        paint_popup_background(cr, c->real_bounds, dock->tiling->raw_window->dpi);
    };

    auto parent = root->child(::vbox, FILL_SPACE, FILL_SPACE);
    parent->pre_layout = [](Container *root, Container *c, const Bounds &) {
        const auto dpi = static_cast<Dock *>(root->user_data)->tiling->raw_window->dpi;
        c->wanted_pad = Bounds(16 * dpi, 12 * dpi, 16 * dpi, 12 * dpi);
        c->spacing = 6 * dpi;
    };
    tiling_toggle(parent, "Tile windows (Alt + Space)", [](const STilingMenuState &state) { return state.is_tiling; },
        [](const std::string &monitor) {
            main_thread([monitor]() {
                const int space = tiling_workspace(monitor);
                if (space == -1)
                    return;
                hypriso->set_space_tiling_id(space, !hypriso->is_space_tiling_id(space));
            });
        });

    tiling_toggle(parent, "Tile new workspaces", [](const STilingMenuState &state) { return state.new_workspace_is_tiling; },
        [](const std::string &) {
            main_thread([]() {
                hypriso->set_new_workspace_tiling(!set->new_workspace_is_tiling);
            });
        });

    tiling_label(parent, "Layout");
    tiling_segments(parent, {"Master", "Scrolling"}, [](const STilingMenuState &state) { return state.layout; },
        [dock](const std::string &layout) {
            if (dock->tiling_settings.layout == layout)
                return;
            dock->tiling_settings.layout = layout;
            windowing::redraw(dock->tiling->raw_window);
            main_thread([layout]() {
                if (set->tiling_layout == layout)
                    return;
                set->tiling_layout = layout;
                hypriso->apply_tiling_settings();
            });
        });

    auto gap_label = tiling_row(parent, 28);
    gap_label->when_paint = [](Container *root, Container *c) {
        const auto dock = static_cast<Dock *>(root->user_data);
        paint_tiling_text(dock, c, "Gap");
        paint_tiling_text(dock, c, std::to_string(dock->tiling_settings.window_gap), true);
    };
    const auto change_gap = [dock](Container *, float value) {
        const int gap = std::clamp((int) std::round(value * 32.0f), 0, 32);
        if (dock->tiling_settings.window_gap == gap)
            return;
        dock->tiling_settings.window_gap = gap;
        windowing::redraw(dock->tiling->raw_window);
        main_thread([gap]() {
            if (set->tiling_window_gap == gap)
                return;
            set->tiling_window_gap = gap;
            hypriso->apply_tiling_settings();
        });
    };
    auto gap_slider = make_self_sizing_slider(parent,
        [](Container *) { return "0"; },
        [](Container *) { return "32"; },
        change_gap,
        [dock](Container *) { return dock->tiling_settings.window_gap / 32.0f; },
        [](Dock *dock) { return dock->tiling; });
    gap_slider->children[1]->when_fine_scrolled = [change_gap](Container *root, Container *c, double, double scroll_y, bool) {
        if (scroll_y == 0)
            return;
        const auto dock = static_cast<Dock *>(root->user_data);
        change_gap(c, (dock->tiling_settings.window_gap + (scroll_y > 0 ? 1 : -1)) / 32.0f);
    };

    tiling_toggle(parent, "Highlight active window", [](const STilingMenuState &state) { return state.border_hint; },
        [](const std::string &) {
            main_thread([]() {
                set->active_window_border_hint = !set->active_window_border_hint;
                hypriso->apply_tiling_settings();
            });
        });
}

int tiling_info_height(Dock *dock) {
    return 12 + 36 + (int) dock->tiling_settings.shortcuts.size() * (28 + 6) + 12;
}

void fill_tiling_info_container(Dock *dock) {
    auto root = dock->tiling_info->root;
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        paint_popup_background(dock->tiling_info->raw_window->drawing_context, c->real_bounds, dock->tiling_info->raw_window->dpi);
    };

    auto parent = root->child(::vbox, FILL_SPACE, FILL_SPACE);
    parent->pre_layout = [](Container *root, Container *c, const Bounds &) {
        const auto dpi = static_cast<Dock *>(root->user_data)->tiling_info->raw_window->dpi;
        c->wanted_pad = Bounds(16 * dpi, 12 * dpi, 12 * dpi, 12 * dpi);
        c->spacing = 6 * dpi;
    };

    auto header = parent->child(::hbox, FILL_SPACE, 36);
    header->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = 36 * static_cast<Dock *>(root->user_data)->tiling_info->raw_window->dpi;
    };
    auto title = header->child(FILL_SPACE, FILL_SPACE);
    title->when_paint = [](Container *root, Container *c) {
        const auto dock = static_cast<Dock *>(root->user_data);
        const auto cr = dock->tiling_info->raw_window->drawing_context;
        const auto dpi = dock->tiling_info->raw_window->dpi;
        const std::string text = dock->tiling_settings.layout + " layout shortcuts";
        const auto bounds = draw_text(cr, 0, 0, text, 13 * dpi, false, set->font, -1, -1, {0, 0, 0, 1}, true);
        draw_text(cr, c->real_bounds.x, c->real_bounds.y + (c->real_bounds.h - bounds.h) * .5, text, 13 * dpi, true,
            set->font, -1, -1, {0, 0, 0, 1}, true);
    };
    auto gear = header->child(36, FILL_SPACE);
    gear->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 36 * static_cast<Dock *>(root->user_data)->tiling_info->raw_window->dpi;
    };
    gear->when_paint = [](Container *root, Container *c) {
        const auto dock = static_cast<Dock *>(root->user_data);
        const auto cr = dock->tiling_info->raw_window->drawing_context;
        const auto dpi = dock->tiling_info->raw_window->dpi;
        const auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
        set_argb(cr, {0, 0, 0, c->state.mouse_pressing ? .14f : (c->state.mouse_hovering ? .1f : .06f)});
        cr->fill();
        const std::string glyph = "";
        const auto t = draw_text(cr, 0, 0, glyph, 13 * dpi, false, icon_font);
        draw_text(cr, center_x(c, t.w), center_y(c, t.h), glyph, 13 * dpi, true, icon_font, -1, -1, {0, 0, 0, .8});
    };
    gear->when_clicked = [](Container *root, Container *) {
        auto dock = (Dock *) root->user_data;
        windowing::close_window(dock->tiling_info->raw_window);
        main_thread([]() {
            settings::start("Shortcuts");
        });
    };

    // The popup is sized for the list when opened, so the rows are built once.
    for (size_t index = 0; index < dock->tiling_settings.shortcuts.size(); index++) {
        auto row = parent->child(FILL_SPACE, 28);
        row->pre_layout = [](Container *root, Container *c, const Bounds &) {
            c->wanted_bounds.h = 28 * static_cast<Dock *>(root->user_data)->tiling_info->raw_window->dpi;
        };
        row->when_paint = [index](Container *root, Container *c) {
            const auto dock = static_cast<Dock *>(root->user_data);
            if (index >= dock->tiling_settings.shortcuts.size())
                return;
            const auto &[action, keys] = dock->tiling_settings.shortcuts[index];
            const auto cr = dock->tiling_info->raw_window->drawing_context;
            const auto dpi = dock->tiling_info->raw_window->dpi;
            const auto ab = draw_text(cr, 0, 0, action, 12 * dpi, false);
            draw_text(cr, c->real_bounds.x, c->real_bounds.y + (c->real_bounds.h - ab.h) * .5, action, 12 * dpi, true, set->font,
                -1, -1, RGBA(0, 0, 0, 1));
            const auto kb = draw_text(cr, 0, 0, keys, 12 * dpi, false);
            draw_text(cr, c->real_bounds.right() - kb.w - 4 * dpi, c->real_bounds.y + (c->real_bounds.h - kb.h) * .5, keys,
                12 * dpi, true, set->font, -1, -1, RGBA(.4, .4, .4, 1));
        };
    }
}
