#ifndef screenshot_h_INCLUDED
#define screenshot_h_INCLUDED

#include <functional>

namespace screenshot_tool {
    void open(bool draw = false);
    void pick_window(std::function<void(int)> on_picked);
    void close();
    bool is_open();
    bool is_drawing();
    bool is_picking_window();
};

#endif // screenshot_h_INCLUDED
