#ifndef windowing_h_INCLUDED
#define windowing_h_INCLUDED

#include <functional>
#include <string>
#include <mutex>
#include <cairo.h>
#include <xkbcommon/xkbcommon.h>

enum Modifier : uint32_t {
    MOD_NONE  = 0,
    MOD_SHIFT = 1u << 0,
    MOD_CTRL  = 1u << 1,
    MOD_ALT   = 1u << 2,
    MOD_SUPER = 1u << 3,
    MOD_CAPS  = 1u << 4,
};

struct PolledFunction {
    int fd = 0;
    int revents = 0;
    void *data = nullptr;
    std::string name;
    std::function<void(PolledFunction f)> func = nullptr;
};

struct RawApp {
    int id = -1;
    std::mutex mutex;

    void print_monitors();
};

struct PositioningInfo {
    int x = 0;
    int y = 0;
    int w = 800;
    int h = 600;
    int min_w = 0;
    int min_h = 0;

    int side = 0; // for docks
};

struct RawWindowSettings {
    std::string name;
    std::string monitor_name;
    PositioningInfo pos;
    int alignment = 0; // 0 none, 1 top, clockwise + 1
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

    std::function<bool(RawWindow *, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text)> on_key_press = nullptr;
    
    std::function<bool(RawWindow *, float x, float y)> on_mouse_enters = nullptr;
    
    std::function<bool(RawWindow *, float x, float y)> on_mouse_leaves = nullptr;

    std::function<bool(RawWindow *, bool gained)> on_keyboard_focus = nullptr;

    std::function<void(RawWindow *, int w, int h)> on_render = nullptr;

    std::function<void(RawWindow *, int w, int h)> on_resize = nullptr;
    
    std::function<void(RawWindow *, float dpi)> on_scale_change = nullptr;
    
    std::function<void(RawWindow *)> on_close = nullptr;
};

enum struct WindowType {
    NONE,
    NORMAL,
    DOCK,
};

namespace windowing {
    RawApp *open_app();

    RawWindow *open_window(RawApp *app, WindowType type, RawWindowSettings settings);
     
    void main_loop(RawApp *app);

    void wake_up(RawWindow *window);
    void set_size(RawWindow *window, int width, int height);
    void redraw(RawWindow *window);

    void close_window(RawWindow *window);   
    
    void close_app(RawApp *app);

    int timer(RawApp *, int ms, std::function<void(void *data)> func, void *data);
    void timer_update(int fd, int ms);
    void timer_stop(RawApp *, int fd);
};

#endif // windowing_h_INCLUDED
