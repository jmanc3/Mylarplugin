
#include "splash.h"

#include "second.h"
#include <xcb/shape.h>

static bool showing = true;

void splash::input() {
    return;
    if (showing) {
        for (auto m : actual_monitors) {
            hypriso->damage_entire(*datum<int>(m, "cid"));
        }
    }
    showing = false;
}

double easeInOutSine(double x) {
    return -(std::cos(M_PI * x) - 1) / 2;
}

void splash::render(int id, int stage) {
    int current_monitor = current_rendering_monitor();
    int current_window = current_rendering_window();
    int active_id = current_window == -1 ? current_monitor : current_window;
    static long start_time = get_current_time_in_ms();
    static bool started_rending = false;
    return;
    if (hypriso->started_rendering(current_monitor) && !started_rending) {
        started_rending = true;
        start_time = get_current_time_in_ms();
    }
    if (!started_rending)
        return;
    auto current = get_current_time_in_ms();
    float progress = ((float) (current - start_time)) / 800.0f;
    
    if (showing && stage == (int) STAGE::RENDER_LAST_MOMENT) {
       if (progress > 1.0) {
           showing = false;
           progress = 1.0f;
        } 
        progress = easeInOutSine(progress);
        auto b = bounds_monitor(current_monitor);
        b.x = 0;
        b.y = 0;
        b.scale(scale(current_monitor));
        rect(b, {0, 0, 0, 1.0f - progress}, 0, 0, 2.0f, false);
        hypriso->damage_entire(current_monitor);
    } 
}

