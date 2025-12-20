
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

double
easeIn(double t) {
    double t2 = t * t;
    return t * t2 * t2;
}

void splash::render(int id, int stage) {
    int current_monitor = current_rendering_monitor();
    float prog = hypriso->zoom_progress(current_monitor);
    if (prog == 1.0f)
        return;
    prog = easeIn(prog);
    static long start_time = get_current_time_in_ms();
    static bool started_rending = false;

    if (showing && stage == (int) STAGE::RENDER_LAST_MOMENT) {
        auto b = bounds_monitor(current_monitor);
        b.x = 0;
        b.y = 0;
        b.scale(scale(current_monitor));
        rect(b, {0, 0, 0, 1.0f - prog}, 0, 0, 2.0f, false);
        hypriso->damage_entire(current_monitor);
    } 
}

