#include "dock/dock_extra.h"

#include "dock/dock_internal.h"

#include "dock/dock_bluetooth.h"
#include "dock/dock_projection.h"
#include "dock/dock_volume.h"

#include <cstdlib>

bool nightlight_on = false;

// State handed to the short-lived timer that reopens a popup after the extra
// menu window has closed.
struct Reformed {
    MylarWindow *window = nullptr;
    Dock *dock = nullptr;
    float dpi = 1.0;
};

static MylarWindow *extra_window(Dock *dock) {
    return dock->extra;
}

static Container *add_extra_button(Container *parent, std::string icon,
                                   std::function<void(Container *root, Container *c)> on_clicked) {
    static constexpr float button_pad = 10;
    auto button = parent->child(FILL_SPACE, FILL_SPACE);
    button->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto s = window->raw_window->dpi;
        c->wanted_pad = Bounds(button_pad * s, button_pad * s, button_pad * s, button_pad * s);
    };
    auto icon_button = button->child(FILL_SPACE, FILL_SPACE);
    icon_button->when_clicked = std::move(on_clicked);
    icon_button->when_paint = [icon = std::move(icon)](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto cr = window->raw_window->cr;
        auto dpi = window->raw_window->dpi;
        if (c->state.mouse_pressing || c->state.mouse_hovering) {
            if (c->state.mouse_pressing) {
                set_argb(cr, {0, 0, 0, .2});
            } else {
                set_argb(cr, {0, 0, 0, .1});
            }
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dpi, 1.0);
            cairo_fill(cr);
        }
        set_argb(cr, {0, 0, 0, .2});
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dpi, 1.0);
        cairo_stroke(cr);

        auto b = draw_text(cr, 0, 0, icon, 36, false, icon_font, -1, -1, {0, 0, 0, 1});
        draw_text(cr, center_x(c, b.w), center_y(c, b.h), icon, 36, true, icon_font, -1, -1, {0, 0, 0, 1});
    };
    return button;
}

static void open_bluetooth_popup(Dock *dock) {
    auto window = extra_window(dock);
    windowing::close_window(window->raw_window);
    if (dock->window->popup_window == window)
        dock->window->popup_window = nullptr;

    auto mylar = dock->window;
    auto dpi = mylar->raw_window->dpi;

    auto reformed = new Reformed;
    reformed->dpi = dpi;
    reformed->window = mylar;
    reformed->dock = dock;

    windowing::timer(dock->app, 40, [](void *data) {
        auto r = std::unique_ptr<Reformed>(static_cast<Reformed *>(data));
        auto mylar = r->window;
        auto dock = r->dock;
        std::lock_guard<std::mutex> lock(dock->app->mutex);
        if (finished || !bluetooth_available.load() || mylar->popup_window || !windowing::has_window(mylar->raw_window))
            return;
        auto dpi = r->dpi;
        auto c = container_by_name("extra", dock->window->root);

        RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, 376, 448);

        dock->bluetooth = open_mylar_popup(mylar, settings);
        if (!dock->bluetooth)
            return;
        dock->bluetooth->root->user_data = dock;
        dock->bluetooth->root->wanted_bounds.w = FILL_SPACE;
        dock->bluetooth->root->wanted_bounds.h = FILL_SPACE;
        fill_bluetooth_container(dock);
        windowing::redraw(dock->bluetooth->raw_window);
    }, reformed);
}

static void open_projection_popup(Dock *dock) {
    auto window = extra_window(dock);
    windowing::close_window(window->raw_window);

    auto mylar = dock->window;
    auto dpi = mylar->raw_window->dpi;

    auto reformed = new Reformed;
    reformed->dpi = dpi;
    reformed->window = mylar;
    reformed->dock = dock;

    windowing::timer(dock->app, 40, [](void *data) {
        main_thread([data]() {
            auto r = std::unique_ptr<Reformed>(static_cast<Reformed *>(data));
            auto mylar = r->window;
            auto dock = r->dock;
            auto dpi = r->dpi;
            auto c = container_by_name("extra", dock->window->root);

            RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, volume_popup_w, volume_popup_w * .5);

            dock->projection = open_mylar_popup(mylar, settings);
            if (!dock->projection)
                return;
            dock->projection->root->on_closed = [](Container *root) {
                auto dock = (Dock *) root->user_data;
                dock->projection = nullptr;
            };
            dock->projection->root->user_data = dock;
            dock->projection->root->wanted_bounds.w = FILL_SPACE;
            dock->projection->root->wanted_bounds.h = FILL_SPACE;
            fill_projection_container(dock);
            windowing::redraw(dock->projection->raw_window);
        });
    }, reformed);
}

void fill_extra_container(Container *root) {
    root->type = ::vbox;
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto cr = window->raw_window->cr;
        paint_popup_background(cr, c->real_bounds, dock->extra->raw_window->dpi);
    };

    auto top = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    auto bluetooth = add_extra_button(top, "\uE702", [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        open_bluetooth_popup(dock);
    });
    bluetooth->name = "bluetooth";
    bluetooth->exists = bluetooth_available.load();
    add_extra_button(top, "\uE708", [](Container *, Container *) {
        if (nightlight_on) {
            system("killall hyprsunset");
        } else {
            std::thread t([]() {
                system("hyprsunset -t 5000");
            });
            t.detach();
        }
        nightlight_on = !nightlight_on;
    });
    add_extra_button(top, "\uE087", nullptr);

    auto middle = root->child(::hbox, FILL_SPACE, FILL_SPACE);
    add_extra_button(middle, "\uEBC6", [](Container *root, Container *) {
        auto dock = (Dock *) root->user_data;
        open_projection_popup(dock);
    });
    add_extra_button(middle, "\uE722", [](Container *root, Container *) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        windowing::close_window(window->raw_window);

        windowing::timer(dock->app, 100, [](void *) {
            system("hyprctl dispatch \"hl.plugin.mylar.screenshot_tool()\"");
        }, nullptr);
    });
    add_extra_button(middle, "\uF140", [](Container *root, Container *) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        windowing::close_window(window->raw_window);

        system("hyprctl kill");
    });

    auto bottom = root->child(FILL_SPACE, FILL_SPACE);
    bottom->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto s = window->raw_window->dpi;
        c->wanted_bounds = Bounds(0, 0, FILL_SPACE, 60 * s);
    };
    bottom->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto cr = window->raw_window->cr;
        set_argb(cr, {0, 0, 0, .08});
        drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, 10 * dock->extra->raw_window->dpi, 1.0);
        cairo_fill(cr);
    };
}
