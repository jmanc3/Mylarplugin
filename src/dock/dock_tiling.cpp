#include "dock/dock_tiling.h"

#include "dock/dock_internal.h"

static int tiling_workspace(const std::string &monitor_name) {
    for (auto monitor : actual_monitors) {
        const int id = *datum<int>(monitor, "cid");
        if (hypriso->monitor_name(id) == monitor_name)
            return hypriso->get_active_workspace_id(id);
    }
    return -1;
}

// Called on the compositor thread while the dock's app mutex is held.
void update_tiling_menu(Dock *dock) {
    dock->tiling_settings = {
        .is_tiling = hypriso->is_space_tiling_id(tiling_workspace(dock->creation_settings.monitor_name)),
        .all_workspaces = set->tile_all_workspaces,
        .new_workspace_is_tiling = set->new_workspace_is_tiling,
        .border_hint = set->active_window_border_hint,
        .window_gap = set->tiling_window_gap,
    };
    if (dock->tiling)
        windowing::redraw(dock->tiling->raw_window);
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

static void tiling_segments(Container *parent, const std::string &first, const std::string &second,
                            std::function<bool(const STilingMenuState &)> first_selected, std::function<void(bool)> change) {
    auto row = tiling_row(parent, 36);
    row->pre_layout = [](Container *root, Container *c, const Bounds &) {
        const auto dpi = static_cast<Dock *>(root->user_data)->tiling->raw_window->dpi;
        c->wanted_bounds.h = 36 * dpi;
        c->spacing = 4 * dpi;
    };
    for (bool is_first : {true, false}) {
        auto segment = row->child(FILL_SPACE, FILL_SPACE);
        segment->when_paint = [first, second, is_first, first_selected](Container *root, Container *c) {
            const auto dock = static_cast<Dock *>(root->user_data);
            const auto cr = dock->tiling->raw_window->drawing_context;
            const auto dpi = dock->tiling->raw_window->dpi;
            const bool selected = first_selected(dock->tiling_settings) == is_first;
            set_argb(cr, selected ? RGBA(.91, .96, 1, 1) : RGBA(.95, .95, .95, 1));
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 6 * dpi, 1);
            cr->fill();
            const std::string text = (selected ? "\u2713  " : "") + (is_first ? first : second);
            const auto bounds = draw_text(cr, 0, 0, text, 12 * dpi, false);
            draw_text(cr, c->real_bounds.x + (c->real_bounds.w - bounds.w) * .5,
                c->real_bounds.y + (c->real_bounds.h - bounds.h) * .5, text, 12 * dpi, true, set->font, -1, -1,
                selected ? accent : RGBA(0, 0, 0, 1));
        };
        segment->when_clicked = [is_first, change](Container *, Container *) {
            change(is_first);
        };
    }
}

static void tiling_shortcut(Container *parent, const std::string &action, const std::string &shortcut) {
    auto row = tiling_row(parent, 30);
    row->when_paint = [action, shortcut](Container *root, Container *c) {
        const auto dock = static_cast<Dock *>(root->user_data);
        paint_tiling_text(dock, c, action);
        paint_tiling_text(dock, c, shortcut, true, RGBA(.4, .4, .4, 1));
    };
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
    tiling_toggle(parent, "Automatically tile windows (Alt + Space)", [](const STilingMenuState &state) { return state.is_tiling; },
        [](const std::string &monitor) {
            main_thread([monitor]() {
                const int space = tiling_workspace(monitor);
                if (space == -1)
                    return;
                hypriso->set_space_tiling_id(space, !hypriso->is_space_tiling_id(space));
            });
        });

    tiling_label(parent, "Above toggle applies to");
    tiling_segments(parent, "All workspaces", "Current workspace", [](const STilingMenuState &state) { return state.all_workspaces; },
        [](bool all) {
            main_thread([all]() {
                if (set->tile_all_workspaces == all)
                    return;
                set->tile_all_workspaces = all;
                hypriso->apply_tiling_settings();
            });
        });

    tiling_label(parent, "New workspace behavior");
    tiling_segments(parent, "Tiled", "Floating", [](const STilingMenuState &state) { return state.new_workspace_is_tiling; },
        [](bool tiled) {
            main_thread([tiled]() {
                hypriso->set_new_workspace_tiling(tiled);
            });
        });

    auto gap_label = tiling_row(parent, 28);
    gap_label->when_paint = [](Container *root, Container *c) {
        const auto dock = static_cast<Dock *>(root->user_data);
        paint_tiling_text(dock, c, "Window gap while tiling");
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

    tiling_row(parent, 4);
    tiling_shortcut(parent, "Navigate windows", "Super + arrows");
    tiling_shortcut(parent, "Move window", "Shift + Super + arrows");
    tiling_shortcut(parent, "Toggle floating window", "Super + G");
    tiling_shortcut(parent, "Reposition tiled window", "Alt + Mouse Left Click");
    tiling_row(parent, 4);

    tiling_toggle(parent, "Active window border hint", [](const STilingMenuState &state) { return state.border_hint; },
        [](const std::string &) {
            main_thread([]() {
                set->active_window_border_hint = !set->active_window_border_hint;
                hypriso->apply_tiling_settings();
            });
        });
}
