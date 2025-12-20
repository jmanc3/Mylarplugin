
#include "splash.h"

#include "second.h"
#include <xcb/shape.h>

static bool showing = false;
static bool start = false;
static long start_time = get_current_time_in_ms();

void splash::input() {
    if (showing)
        start = true;
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

// {"anchors":[{"x":0,"y":1},{"x":0.525,"y":0.4},{"x":1,"y":0}],"controls":[{"x":0.45237856557239087,"y":0.8590353902180988},{"x":0.7485689587985109,"y":0.05514647589789487}]}
std::vector<float> fadein = { 0, 0.0050000000000000044, 0.01100000000000001, 0.017000000000000015, 0.02300000000000002, 0.030000000000000027, 0.03700000000000003, 0.04500000000000004, 0.052000000000000046, 0.061000000000000054, 0.06999999999999995, 0.07899999999999996, 0.08899999999999997, 0.09899999999999998, 0.10999999999999999, 0.122, 0.135, 0.14800000000000002, 0.16300000000000003, 0.17800000000000005, 0.19399999999999995, 0.21199999999999997, 0.23099999999999998, 0.252, 0.275, 0.30000000000000004, 0.32699999999999996, 0.359, 0.394, 0.43600000000000005, 0.487, 0.554, 0.613, 0.638, 0.661, 0.6839999999999999, 0.7070000000000001, 0.728, 0.748, 0.768, 0.786, 0.804, 0.821, 0.838, 0.853, 0.868, 0.882, 0.895, 0.907, 0.919, 0.9299999999999999, 0.94, 0.949, 0.958, 0.966, 0.973, 0.98, 0.986, 0.991, 0.996 };

static float pull(std::vector<float>& fls, float scalar) {
    if (fls.empty())
        return 0.0f; // or throw an exception

    // Clamp scalar between 0 and 1
    scalar = std::clamp(scalar, 0.0f, 1.0f);

    float fIndex = scalar * (fls.size() - 1); // exact position
    int   i0     = static_cast<int>(std::floor(fIndex));
    int   i1     = static_cast<int>(std::ceil(fIndex));

    if (i0 == i1 || i1 >= fls.size()) {
        return fls[i0];
    }

    float t = fIndex - i0; // fraction between the two indices
    return fls[i0] * (1.0f - t) + fls[i1] * t;
}

// {"anchors":[{"x":0,"y":1},{"x":1,"y":0}],"controls":[{"x":0.29521329248987305,"y":-0.027766935560437862}]}
std::vector<float> zoomin = { 0, 0.05600000000000005, 0.10899999999999999, 0.15800000000000003, 0.20499999999999996, 0.249, 0.29000000000000004, 0.32899999999999996, 0.366, 0.402, 0.43500000000000005, 0.46699999999999997, 0.497, 0.526, 0.554, 0.5800000000000001, 0.605, 0.628, 0.651, 0.673, 0.6930000000000001, 0.7130000000000001, 0.732, 0.749, 0.766, 0.782, 0.798, 0.812, 0.8260000000000001, 0.84, 0.852, 0.864, 0.875, 0.886, 0.896, 0.906, 0.915, 0.923, 0.931, 0.938, 0.945, 0.952, 0.958, 0.963, 0.969, 0.973, 0.978, 0.982, 0.985, 0.988, 0.991, 0.993, 0.995, 0.997, 0.998, 1, 1, 1.001, 1.001, 1.001 };

void splash::render(int id, int stage) {
    int current_monitor = current_rendering_monitor();
    long current = get_current_time_in_ms();
    static bool has_done_initial_zoom_anim = false;
    static long time = 3000;
    static float anim_time = 500.0f;
    static float max_zoom_perc = .1f;
    if (start) {
        //showing = true;
        //start = false;
        //start_time = current - time;
    }
    float delta = (float) ((current - start_time) - time);
    auto scalar = delta / anim_time;
    if (scalar >= 1.0f) {
        if (showing) {
            hypriso->set_zoom_factor(1.0);
        }
        showing = false;
        return;
    }
        
    if (current - start_time > time && scalar < 1.0) {
        if (scalar > 1.0)
            scalar = 1.0f;
        
        if (showing && stage == (int)STAGE::RENDER_LAST_MOMENT) {
            auto b = bounds_monitor(current_monitor);
            b.x = 0;
            b.y = 0;
            b.scale(scale(current_monitor));
            scalar = pull(fadein, scalar);
            auto zoom_scalar = pull(zoomin, scalar);
            hypriso->set_zoom_factor(1.0 + max_zoom_perc * (1.0 - zoom_scalar), true);
            rect(b, {0, 0, 0, 1.0f - scalar}, 0, 0, 2.0f, false);
            hypriso->damage_entire(current_monitor);
        }
    } else {
       if (showing && stage == (int)STAGE::RENDER_LAST_MOMENT) {
            auto b = bounds_monitor(current_monitor);
            b.x = 0;
            b.y = 0;
            b.scale(scale(current_monitor));
            hypriso->set_zoom_factor(1.0 + max_zoom_perc, true);
            rect(b, {0, 0, 0, 1.0f}, 0, 0, 2.0f, false);
            hypriso->damage_entire(current_monitor);
        } 
    }
}

