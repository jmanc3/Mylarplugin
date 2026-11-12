#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

// Eases a scroll offset to where mouse wheel clicks sent it. Touchpads already
// deliver smooth deltas, so those should be applied directly after stop().
//
// Call step() once per layout/paint with the scroll variable; while it returns
// true another frame is needed. If anything else writes the scroll variable
// (a scrollbar drag, a jump to the selection, a reset) the animation notices
// on the next step and gives up, so callers never have to cancel it by hand.
struct SmoothScroll {
    static constexpr double duration_ms = 220;

    double from = 0;
    double to = 0;
    double last = 0; // Value step() last wrote
    double start_ms = 0;
    bool active = false;

    static double now_ms() {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    // Animates from `current` to `target`.
    void start(double current, double target) {
        from = current;
        to = target;
        last = current;
        start_ms = now_ms();
        active = from != to;
    }

    // Moves the destination by `delta` from wherever the scroll is already headed.
    void scroll_by(double current, double delta, double min, double max) {
        double base = active ? to : current;
        start(current, std::clamp(base + delta, min, max));
    }

    void stop() { active = false; }

    template <typename T>
    bool step(T &scroll) {
        if (!active)
            return false;
        if (std::abs((double) scroll - last) > .5) {
            active = false;
            return false;
        }
        double t = std::clamp((now_ms() - start_ms) / duration_ms, 0.0, 1.0);
        double eased = 1.0 - std::pow(1.0 - t, 3);
        scroll = (T) (from + (to - from) * eased);
        last = (double) scroll;
        if (t >= 1.0)
            active = false;
        return active;
    }
};
