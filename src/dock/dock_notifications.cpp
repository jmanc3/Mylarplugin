#include "dock/dock_notifications.h"

#include "dock/dock_internal.h"
#include "settings.h"

#include <pango/pangocairo.h>

namespace {

constexpr int kNotifLogicalWidth = 356;
constexpr int kNotifEdgeMargin = 12;
constexpr int kNotifStackGap = 12;

struct NotifRootData : UserData {
    DockNotification *notif = nullptr;
};

struct NotifLabelData : UserData {
    DockNotification *notif = nullptr;
    std::string text;
    int size = 11;
    bool bold = false;
    RGBA color = {0, 0, 0, 1};
};

struct NotifActionData : UserData {
    DockNotification *notif = nullptr;
    NotificationAction action;
};

static int measure_wrapped_text_height(const std::string &text, bool bold, int size_px, int max_width_px) {
    if (text.empty() || max_width_px <= 0)
        return 0;

    cairo_t *cr = drawing::scratch_cairo();
    PangoLayout *layout = get_cached_pango_font(cr, set->font, size_px,
                                                bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    auto initial_wrap = pango_layout_get_wrap(layout);
    auto initial_ellipse = pango_layout_get_ellipsize(layout);

    pango_layout_set_attributes(layout, nullptr);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_width(layout, max_width_px * PANGO_SCALE);
    pango_layout_set_height(layout, -1);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);

    std::string stripped = strip_html(text);
    pango_layout_set_text(layout, stripped.data(), (int) stripped.size());

    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);

    pango_layout_set_width(layout, -1);
    pango_layout_set_wrap(layout, initial_wrap);
    pango_layout_set_ellipsize(layout, initial_ellipse);

    return std::max(logical.height, size_px);
}

static void draw_wrapped_text(drawing::Context *cr, const Bounds &bounds, const std::string &text,
                              bool bold, int size_px, RGBA color) {
    if (!cr || text.empty() || bounds.w <= 0 || bounds.h <= 0)
        return;

    PangoLayout *layout = get_cached_pango_font(cr, set->font, size_px,
                                                bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    auto initial_wrap = pango_layout_get_wrap(layout);
    auto initial_ellipse = pango_layout_get_ellipsize(layout);

    pango_layout_set_attributes(layout, nullptr);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_width(layout, (int) std::round(bounds.w) * PANGO_SCALE);
    pango_layout_set_height(layout, (int) std::round(bounds.h) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    std::string stripped = strip_html(text);
    pango_layout_set_text(layout, stripped.data(), (int) stripped.size());

    set_argb(cr, color);
    cr->move_to(std::round(bounds.x), std::round(bounds.y));
    cr->show_layout(layout);

    pango_layout_set_width(layout, -1);
    pango_layout_set_height(layout, -1);
    pango_layout_set_wrap(layout, initial_wrap);
    pango_layout_set_ellipsize(layout, initial_ellipse);
}

static std::string resolve_notification_icon_path(const NotificationInfo &info, int icon_size) {
    auto exists_file = [](const std::string &path) {
        if (path.empty())
            return false;
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    };

    if (exists_file(info.icon_path))
        return info.icon_path;

    std::string raw_icon = info.app_icon;
    if (starts_with(raw_icon, "file://"))
        raw_icon = raw_icon.substr(7);
    if (exists_file(raw_icon))
        return raw_icon;

    if (!icons_loaded)
        return "";

    std::vector<IconTarget> targets;
    if (!raw_icon.empty())
        targets.emplace_back(raw_icon);
    if (!info.desktop_entry.empty())
        targets.emplace_back(info.desktop_entry);
    if (!info.app_name.empty())
        targets.emplace_back(info.app_name);

    if (targets.empty())
        return "";

    search_icons(targets);
    pick_best(targets, icon_size, IconContext::Statuses);
    for (const auto &t : targets) {
        if (exists_file(t.best_full_path))
            return t.best_full_path;
    }

    pick_best(targets, icon_size);
    for (const auto &t : targets) {
        if (exists_file(t.best_full_path))
            return t.best_full_path;
    }

    return "";
}

static void stop_notif_timer(DockNotification *notif) {
    if (!notif || !notif->dock || !notif->dock->app)
        return;
    if (notif->timer_fd >= 0) {
        windowing::timer_stop(notif->dock->app, notif->timer_fd);
        notif->timer_fd = -1;
    }
}

static void dismiss_notification(DockNotification *notif, NotificationReasonClosed reason, bool emit_signal) {
    if (!notif || notif->closed)
        return;
    notif->closed = true;
    stop_notif_timer(notif);

    if (emit_signal && !notif->close_signal_sent) {
        notif->close_signal_sent = true;
        notification_closed_signal(notif->info.id, notif->info.calling_dbus_client, reason);
    }

    if (notif->window && notif->window->raw_window)
        windowing::close_window(notif->window->raw_window);
}

static void on_notification_window_closed(DockNotification *notif) {
    if (!notif)
        return;
    stop_notif_timer(notif);
    notif->closed = true;

    Dock *dock = notif->dock;
    if (dock) {
        auto &list = dock->notifications;
        for (int i = 0; i < (int) list.size(); i++) {
            if (list[i] == notif) {
                int removed_step = notif->logical_h + kNotifStackGap;
                list.erase(list.begin() + i);
                // Older notifications sit above newer ones (at smaller indices).
                for (int j = i - 1; j >= 0; j--) {
                    auto *above = list[j];
                    above->margin_bottom = std::max(kNotifEdgeMargin, above->margin_bottom - removed_step);
                    if (above->window && above->window->raw_window) {
                        windowing::set_layer_margin(above->window->raw_window,
                                                    0, kNotifEdgeMargin, above->margin_bottom, 0);
                    }
                }
                break;
            }
        }
    }

    MylarWindow *win = notif->window;
    notif->window = nullptr;
    if (dock && dock->app && !dock->closing && !finished) {
        windowing::run_on_app(dock->app, [win, notif]() {
            if (win) {
                delete win->root;
                delete win;
            }
            delete notif;
        });
    } else {
        delete notif;
    }
}

static bool root_pierced_handler(Container *container, int mouse_x, int mouse_y) {
    if (auto c = container_by_name("actions_container", container)) {
        for (auto child : c->children) {
            if (bounds_contains(child->real_bounds, mouse_x, mouse_y))
                return false;
        }
    }
    if (auto c = container_by_name("close_button", container)) {
        if (bounds_contains(c->real_bounds, mouse_x, mouse_y))
            return false;
    }
    return bounds_contains(container->real_bounds, mouse_x, mouse_y);
}

static bool close_button_pierced_handler(Container *container, int mouse_x, int mouse_y) {
    return bounds_contains(container->real_bounds, mouse_x, mouse_y);
}

static void paint_notif_root(Container *root, Container *c) {
    auto *data = (NotifRootData *) root->user_data;
    if (!data || !data->notif || !data->notif->window)
        return;
    auto *rw = data->notif->window->raw_window;
    auto *cr = rw->drawing_context;
    paint_popup_background(cr, c->real_bounds, rw->dpi);
}

static void clicked_notif_root(Container *root, Container *c) {
    auto *data = (NotifRootData *) root->user_data;
    if (!data || !data->notif)
        return;
    auto *notif = data->notif;

    // Invoke the "default" action if the notification declared one.
    for (const auto &action : notif->info.actions) {
        if (action.id == "default") {
            if (action.callback) {
                action.callback(&notif->info);
            } else {
                notification_action_invoked_signal(notif->info.id, notif->info.calling_dbus_client, action.id);
            }
            dismiss_notification(notif, NotificationReasonClosed::DISMISSED_BY_USER, true);
            return;
        }
    }

    if (notif->info.on_ignore)
        notif->info.on_ignore(&notif->info);
    dismiss_notification(notif, NotificationReasonClosed::DISMISSED_BY_USER, true);
}

static void paint_interaction_header(Container *root, Container *c) {
    auto *data = (NotifRootData *) root->user_data;
    if (!data || !data->notif || !data->notif->window)
        return;
    auto *rw = data->notif->window->raw_window;
    auto *cr = rw->drawing_context;
    float dpi = rw->dpi;

    cr->save();
    clip_to_popup_background(cr, root->real_bounds, dpi);
    set_argb(cr, {0, 0, 0, .05});
    set_rect(cr, c->real_bounds);
    cr->fill();
    set_argb(cr, {0, 0, 0, .08});
    cr->rectangle(c->real_bounds.x, c->real_bounds.y + c->real_bounds.h - std::max(1.0, (double) dpi),
                  c->real_bounds.w, std::max(1.0, (double) dpi));
    cr->fill();
    cr->restore();

    std::string text = "Interaction Required";
    int font_size = (int) std::round(10 * dpi);
    auto b = draw_text(cr, 0, 0, text, font_size, false, set->font, -1, -1, {0, 0, 0, 1}, true);
    draw_text(cr,
              (int) std::round(c->real_bounds.x + (c->real_bounds.w - b.w) * 0.5),
              (int) std::round(c->real_bounds.y + (c->real_bounds.h - b.h) * 0.5),
              text, font_size, true, set->font, -1, -1, {0.15, 0.15, 0.15, 1}, true);
}

static void paint_notif_icon(Container *root, Container *c) {
    auto *data = (NotifRootData *) root->user_data;
    if (!data || !data->notif || !data->notif->window || !data->notif->icon_surface)
        return;
    auto *rw = data->notif->window->raw_window;
    auto *cr = rw->drawing_context;
    auto *surf = data->notif->icon_surface;
    double sw = cairo_image_surface_get_width(surf);
    double sh = cairo_image_surface_get_height(surf);
    double x = std::round(c->real_bounds.x + (c->real_bounds.w - sw) * 0.5);
    double y = std::round(c->real_bounds.y + (c->real_bounds.h - sh) * 0.5);
    cr->set_source_surface(surf, x, y);
    cr->paint_source();
}

static void paint_notif_label(Container *root, Container *c) {
    auto *data = (NotifLabelData *) c->user_data;
    if (!data || !data->notif || !data->notif->window)
        return;
    auto *rw = data->notif->window->raw_window;
    auto *cr = rw->drawing_context;
    draw_wrapped_text(cr, c->real_bounds, data->text, data->bold,
                      (int) std::round(data->size * rw->dpi), data->color);
}

static void paint_notif_close(Container *root, Container *c) {
    auto *data = (NotifRootData *) root->user_data;
    if (!data || !data->notif || !data->notif->window)
        return;
    auto *rw = data->notif->window->raw_window;
    auto *cr = rw->drawing_context;
    float dpi = rw->dpi;

    bool hovered_anywhere = bounds_contains(root->real_bounds, root->mouse_current_x, root->mouse_current_y);
    if (!hovered_anywhere && !c->state.mouse_hovering && !c->state.mouse_pressing)
        return;

    if (c->state.mouse_pressing || c->state.mouse_hovering) {
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 6 * dpi, 1.0);
        set_argb(cr, c->state.mouse_pressing ? RGBA(0, 0, 0, .14) : RGBA(0, 0, 0, .07));
        cr->fill();
    }

    RGBA icon_color = c->state.mouse_pressing ? RGBA(0, 0, 0, 0.95)
                    : c->state.mouse_hovering ? RGBA(0, 0, 0, 0.85)
                                              : RGBA(0, 0, 0, 0.45);
    int font_size = (int) std::round(9 * dpi);
    auto b = draw_text(cr, 0, 0, "\uE8BB", font_size, false, icon_font, -1, -1, icon_color, false);
    draw_text(cr,
              (int) std::round(c->real_bounds.x + (c->real_bounds.w - b.w) * 0.5),
              (int) std::round(c->real_bounds.y + (c->real_bounds.h - b.h) * 0.5),
              "\uE8BB", font_size, true, icon_font, -1, -1, icon_color, false);
}

static void clicked_notif_close(Container *root, Container *c) {
    auto *data = (NotifRootData *) root->user_data;
    if (!data || !data->notif)
        return;
    auto *notif = data->notif;
    if (notif->info.on_ignore)
        notif->info.on_ignore(&notif->info);
    dismiss_notification(notif, NotificationReasonClosed::DISMISSED_BY_USER, true);
}

static void paint_notif_action(Container *root, Container *c) {
    auto *data = (NotifActionData *) c->user_data;
    if (!data || !data->notif || !data->notif->window)
        return;
    auto *rw = data->notif->window->raw_window;
    auto *cr = rw->drawing_context;
    float dpi = rw->dpi;

    RGBA bg = {0, 0, 0, .06};
    if (c->state.mouse_pressing)
        bg = {0, 0, 0, .16};
    else if (c->state.mouse_hovering)
        bg = {0, 0, 0, .11};

    drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 6 * dpi, 1.0);
    set_argb(cr, bg);
    cr->fill();

    int font_size = (int) std::round(10.5 * dpi);
    RGBA fg = {0.1, 0.1, 0.1, 1.0};
    auto b = draw_text(cr, 0, 0, data->action.label, font_size, false, set->font, -1, -1, fg, false);
    draw_text(cr,
              (int) std::round(c->real_bounds.x + (c->real_bounds.w - b.w) * 0.5),
              (int) std::round(c->real_bounds.y + (c->real_bounds.h - b.h) * 0.5),
              data->action.label, font_size, true, set->font, -1, -1, fg, false);
}

static void clicked_notif_action(Container *root, Container *c) {
    auto *data = (NotifActionData *) c->user_data;
    if (!data || !data->notif)
        return;
    auto *notif = data->notif;

    if (data->action.callback) {
        data->action.callback(&notif->info);
    } else {
        notification_action_invoked_signal(notif->info.id, notif->info.calling_dbus_client, data->action.id);
    }

    stop_notif_timer(notif);
    notif->timer_fd = windowing::timer(notif->dock->app, 200, [notif](void *) {
        notif->timer_fd = -1;
        dismiss_notification(notif, NotificationReasonClosed::DISMISSED_BY_USER, true);
    }, nullptr);
}

} // namespace

std::string strip_html(const std::string &text) {
    std::string t = text;
    size_t pos = 0;
    while ((pos = t.find('<', pos)) != std::string::npos) {
        size_t endpos = t.find('>', pos);
        if (endpos == std::string::npos)
            break;
        t.erase(pos, endpos - pos + 1);
    }
    return t;
}

void close_notification_on_dock(Dock *dock, uint32_t id, bool emit_signal) {
    if (!dock || id == 0)
        return;
    std::lock_guard<std::mutex> lock(dock->app->mutex);
    for (auto *notif : dock->notifications) {
        if (notif && !notif->closed && notif->info.id == id) {
            dismiss_notification(notif, NotificationReasonClosed::CLOSED_BY_CLOSE_NOTIFICATION_CALL, emit_signal);
            return;
        }
    }
}

void cleanup_dock_notifications(Dock *dock) {
    if (!dock)
        return;
    auto list = std::move(dock->notifications);
    dock->notifications.clear();
    for (auto *notif : list) {
        if (!notif)
            continue;
        notif->dock = nullptr;
        notif->closed = true;
        if (notif->window) {
            if (notif->window->root)
                notif->window->root->on_closed = nullptr;
            delete notif->window->root;
            delete notif->window;
            notif->window = nullptr;
        }
        delete notif;
    }
}

void show_notification_on_dock(Dock *dock, const NotificationInfo &info) {
    if (!dock || !dock->app || dock->closing || finished)
        return;

    if (info.replaces_id != 0)
        close_notification_on_dock(dock, info.replaces_id, false);
    if (info.id != 0)
        close_notification_on_dock(dock, info.id, false);

    float dpi = 1.0f;
    std::string monitor_name;
    {
        std::lock_guard<std::mutex> lock(dock->app->mutex);
        if (dock->closing || finished || !dock->window || !dock->window->raw_window)
            return;
        dpi = dock->window->raw_window->dpi;
        monitor_name = dock->creation_settings.monitor_name;
    }
    if (dpi <= 0.0f)
        dpi = 1.0f;

    std::string title_text = info.summary;
    std::string body_text = info.body;
    std::string subtitle_text = info.app_name;

    if (title_text.empty() && !subtitle_text.empty()) {
        title_text = subtitle_text;
        subtitle_text.clear();
    }
    if (title_text.empty() && subtitle_text.empty() && !body_text.empty()) {
        title_text = body_text;
        body_text.clear();
    }
    if (title_text.empty() && body_text.empty() && subtitle_text.empty())
        return;

    auto *notif = new DockNotification;
    notif->dock = dock;
    notif->info = info;

    int width_px = (int) std::round(kNotifLogicalWidth * dpi);
    int icon_size = (int) std::round(48 * dpi);
    std::string icon_path = resolve_notification_icon_path(info, icon_size);
    if (!icon_path.empty()) {
        notif->info.icon_path = icon_path;
        load_icon_full_path(&notif->icon_surface, icon_path, icon_size);
    }
    bool has_icon = notif->icon_surface != nullptr;

    int pad_x = (int) std::round(16 * dpi);
    int icon_col_w = has_icon ? (pad_x + icon_size + (int) std::round(14 * dpi)) : pad_x;
    int close_btn_size = (int) std::round(24 * dpi);
    int close_pad = (int) std::round(10 * dpi);
    int close_col_w = close_btn_size + close_pad * 2;
    int max_text_width = std::max(60, width_px - icon_col_w - close_col_w);

    int title_height = measure_wrapped_text_height(title_text, true, (int) std::round(11 * dpi), max_text_width);
    int body_height = measure_wrapped_text_height(body_text, false, (int) std::round(11 * dpi), max_text_width);
    int max_body_height = (int) std::round(200 * dpi);
    if (body_height > max_body_height)
        body_height = max_body_height;
    int subtitle_height = measure_wrapped_text_height(subtitle_text, false, (int) std::round(9 * dpi), max_text_width);

    int text_block_h = title_height + body_height + subtitle_height;
    int gap_title_body = (!title_text.empty() && !body_text.empty()) ? (int) std::round(3 * dpi) : 0;
    int gap_subtitle = (!subtitle_text.empty() && (!title_text.empty() || !body_text.empty())) ? (int) std::round(4 * dpi) : 0;
    text_block_h += gap_title_body + gap_subtitle;

    int vert_pad = (int) std::round(16 * dpi);
    int content_row_h = text_block_h + vert_pad * 2;
    if (has_icon)
        content_row_h = std::max(content_row_h, icon_size + vert_pad * 2);
    content_row_h = std::max(content_row_h, close_btn_size + close_pad * 2);

    bool show_interaction_header = (info.expire_timeout_in_milliseconds == 0);
    int header_h = show_interaction_header ? (int) std::round(34 * dpi) : 0;

    int actions_h = 0;
    int action_btn_h = (int) std::round(32 * dpi);
    int action_gap = (int) std::round(6 * dpi);
    int action_bottom_pad = (int) std::round(12 * dpi);
    if (!info.actions.empty()) {
        actions_h = (int) info.actions.size() * action_btn_h +
                    ((int) info.actions.size() - 1) * action_gap +
                    action_bottom_pad;
    }

    int total_h_px = header_h + content_row_h + actions_h;
    notif->logical_w = kNotifLogicalWidth;
    notif->logical_h = std::max(1, (int) std::ceil(total_h_px / dpi));
    notif->margin_bottom = kNotifEdgeMargin;

    // Shift existing notifications on this dock upward to make room at the bottom right.
    int shift_up = notif->logical_h + kNotifStackGap;
    {
        std::lock_guard<std::mutex> lock(dock->app->mutex);
        for (auto *existing : dock->notifications) {
            if (!existing || existing->closed)
                continue;
            existing->margin_bottom += shift_up;
            if (existing->window && existing->window->raw_window) {
                windowing::set_layer_margin(existing->window->raw_window,
                                            0, kNotifEdgeMargin, existing->margin_bottom, 0);
            }
        }
    }

    RawWindowSettings settings;
    settings.pos.w = notif->logical_w;
    settings.pos.h = notif->logical_h;
    settings.name = "Notification";
    settings.monitor_name = monitor_name;
    settings.scale_hint = dpi;
    settings.layer_margins.right = kNotifEdgeMargin;
    settings.layer_margins.bottom = notif->margin_bottom;

    // Must not hold dock->app->mutex across open_mylar_window (wl_display_dispatch may run on_render).
    notif->window = open_mylar_window(dock->app, WindowType::NOTIFICATION, settings);
    if (!notif->window || !notif->window->raw_window) {
        delete notif;
        return;
    }

    std::lock_guard<std::mutex> lock(dock->app->mutex);

    Container *root = notif->window->root;
    root->type = ::vbox;
    auto *root_data = new NotifRootData;
    root_data->notif = notif;
    root->user_data = root_data;
    root->when_paint = paint_notif_root;
    root->when_clicked = clicked_notif_root;
    root->handles_pierced = root_pierced_handler;
    root->receive_events_even_if_obstructed = true;
    root->on_closed = [notif](Container *) {
        on_notification_window_closed(notif);
    };

    if (show_interaction_header) {
        auto *header = root->child(FILL_SPACE, header_h);
        header->when_paint = paint_interaction_header;
    }

    auto *main_row = root->child(::hbox, FILL_SPACE, content_row_h);

    if (has_icon) {
        auto *icon_box = main_row->child(icon_col_w, FILL_SPACE);
        icon_box->when_paint = paint_notif_icon;
    } else {
        main_row->child(icon_col_w, FILL_SPACE);
    }

    auto *text_col = main_row->child(::vbox, FILL_SPACE, FILL_SPACE);
    text_col->child(FILL_SPACE, FILL_SPACE);
    if (!title_text.empty()) {
        auto *lbl = text_col->child(FILL_SPACE, title_height);
        auto *ld = new NotifLabelData;
        ld->notif = notif;
        ld->text = title_text;
        ld->size = 11;
        ld->bold = true;
        ld->color = {0.0, 0.0, 0.0, 1.0};
        lbl->user_data = ld;
        lbl->when_paint = paint_notif_label;
    }
    if (!body_text.empty()) {
        if (gap_title_body > 0)
            text_col->child(FILL_SPACE, gap_title_body);
        auto *lbl = text_col->child(FILL_SPACE, body_height);
        auto *ld = new NotifLabelData;
        ld->notif = notif;
        ld->text = body_text;
        ld->size = 11;
        ld->bold = false;
        ld->color = {0.18, 0.18, 0.18, 1.0};
        lbl->user_data = ld;
        lbl->when_paint = paint_notif_label;
    }
    if (!subtitle_text.empty()) {
        if (gap_subtitle > 0)
            text_col->child(FILL_SPACE, gap_subtitle);
        auto *lbl = text_col->child(FILL_SPACE, subtitle_height);
        auto *ld = new NotifLabelData;
        ld->notif = notif;
        ld->text = subtitle_text;
        ld->size = 9;
        ld->bold = false;
        ld->color = {0.45, 0.45, 0.45, 1.0};
        lbl->user_data = ld;
        lbl->when_paint = paint_notif_label;
    }
    text_col->child(FILL_SPACE, FILL_SPACE);

    auto *close_col = main_row->child(::vbox, close_col_w, FILL_SPACE);
    close_col->wanted_pad = Bounds(close_pad, close_pad, close_pad, 0);
    auto *close_btn = close_col->child(close_btn_size, close_btn_size);
    close_btn->name = "close_button";
    close_btn->when_paint = paint_notif_close;
    close_btn->when_clicked = clicked_notif_close;
    close_btn->handles_pierced = close_button_pierced_handler;

    if (!info.actions.empty()) {
        auto *actions_box = root->child(::vbox, FILL_SPACE, actions_h);
        actions_box->name = "actions_container";
        actions_box->spacing = action_gap;
        actions_box->wanted_pad = Bounds(pad_x, 0, pad_x, action_bottom_pad);
        for (const auto &action : info.actions) {
            auto *btn = actions_box->child(FILL_SPACE, action_btn_h);
            auto *ad = new NotifActionData;
            ad->notif = notif;
            ad->action = action;
            btn->user_data = ad;
            btn->when_paint = paint_notif_action;
            btn->when_clicked = clicked_notif_action;
        }
    }

    dock->notifications.push_back(notif);
    windowing::redraw(notif->window->raw_window);

    int timeout_ms = info.expire_timeout_in_milliseconds;
    if (timeout_ms <= 0) {
        int text_length = (int) (info.summary.length() + info.body.length());
        timeout_ms = 60000 * text_length / 6 / 200 + 2000;
        if (info.expire_timeout_in_milliseconds == 0 && !info.actions.empty())
            timeout_ms += 5000;
        if (timeout_ms < 5000)
            timeout_ms = 5000;
    }

    notif->timer_fd = windowing::timer(dock->app, timeout_ms, [notif](void *) {
        notif->timer_fd = -1;
        if (notif->info.on_ignore)
            notif->info.on_ignore(&notif->info);
        dismiss_notification(notif, NotificationReasonClosed::EXPIRED, true);
    }, nullptr);
}
