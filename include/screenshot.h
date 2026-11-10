#ifndef screenshot_h_INCLUDED
#define screenshot_h_INCLUDED

namespace screenshot_tool {
    void open(bool draw = false);
    void close();
    bool is_drawing();
};

#endif // screenshot_h_INCLUDED
