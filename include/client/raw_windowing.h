#ifndef windowing_h_INCLUDED
#define windowing_h_INCLUDED

#include <functional>
#include <string>
#include <cairo.h>

struct PolledFunction {
    int fd = 0;
    int revents = 0;
    std::function<void(PolledFunction f)> func = nullptr;
};

struct RawApp {
    int id = -1;

    void print_monitors();
};

struct PositioningInfo {
    int x = 0;
    int y = 0;
    int w = 800;
    int h = 600;
    
    int side = 0; // for docks
};

struct RawWindowSettings {
    std::string name;
    PositioningInfo pos;
};

struct RawWindow {
    int id = -1;

    float dpi = 1.0;

    RawApp *creator = nullptr;
    
    RawWindow *parent = nullptr;
    std::vector<RawWindow *> children;

    cairo_t *cr = nullptr;

    std::function<bool(RawWindow *, float x, float y)> on_mouse_move = nullptr;

    std::function<bool(RawWindow *, int button, int state, float x, float y)> on_mouse_press = nullptr;

    std::function<bool(RawWindow *, int source, int axis, int direction, double delta, int discrete, bool mouse)> on_scrolled = nullptr;

    std::function<bool(RawWindow *, int key, int state, bool update_mods)> on_key_press = nullptr;
    
    std::function<bool(RawWindow *, float x, float y)> on_mouse_enters = nullptr;
    
    std::function<bool(RawWindow *, float x, float y)> on_mouse_leaves = nullptr;

    std::function<bool(RawWindow *, bool gained)> on_keyboard_focus = nullptr;

    std::function<void(RawWindow *, int w, int h)> on_render = nullptr;

    std::function<void(RawWindow *, int w, int h)> on_resize = nullptr;
    
    std::function<void(RawWindow *, float dpi)> on_scale_change = nullptr;
};

enum struct WindowType {
    NONE,
    NORMAL,
    DOCK,
};

namespace windowing {
    RawApp *open_app();

    void add_fb(RawApp *app, int fd, std::function<void(PolledFunction pf)> func);
    
    RawWindow *open_window(RawApp *app, WindowType type, RawWindowSettings settings);
     
    void main_loop(RawApp *app);

    void wake_up(RawWindow *window);
    void redraw(RawWindow *window);

    void close_window(RawWindow *window);   
    
    void close_app(RawApp *app);
};

#endif // windowing_h_INCLUDED
