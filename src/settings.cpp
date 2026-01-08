
#include "settings.h"

#include "heart.h"
#include "client/raw_windowing.h"
#include "client/windowing.h"

#include <thread>

static RawApp *settings_app = nullptr;

void actual_start() {
    settings_app = windowing::open_app();
    RawWindowSettings settings;
    settings.pos.w = 800;
    settings.pos.h = 600;
    settings.name = "Settings";
    auto mylar = open_mylar_window(settings_app, WindowType::NORMAL, settings);
    mylar->root->user_data = mylar;
    mylar->root->when_paint = paint {
        auto mylar = (MylarWindow*)root->user_data;
        auto cr = mylar->raw_window->cr;
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_restore(cr);

        cairo_rectangle(cr, root->real_bounds.x, root->real_bounds.y, root->real_bounds.w, root->real_bounds.h);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_fill(cr);
    };
    
    windowing::main_loop(settings_app);

    settings_app = nullptr;
};

void settings::start() {
    if (settings_app)
        return;
    std::thread t(actual_start);
    t.detach();
}

void settings::stop() {
    if (settings_app) {
        windowing::close_app(settings_app);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
