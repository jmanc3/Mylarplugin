#pragma once

struct Bounds;

namespace desktop_icons {
    void start();
    void stop();
    void deselect();
    void paint_overview(int monitor, const Bounds &bounds, float alpha);
}
