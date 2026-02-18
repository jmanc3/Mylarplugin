#ifndef windowing_h_INCLUDED
#define windowing_h_INCLUDED

#include <functional>
#include <string>
#include <mutex>
#include <vector>
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
    enum struct PopupAnchor {
        NONE,
        TOP,
        BOTTOM,
        LEFT,
        RIGHT,
        TOP_LEFT,
        BOTTOM_LEFT,
        TOP_RIGHT,
        BOTTOM_RIGHT,
    };

    enum struct PopupGravity {
        NONE,
        TOP,
        BOTTOM,
        LEFT,
        RIGHT,
        TOP_LEFT,
        BOTTOM_LEFT,
        TOP_RIGHT,
        BOTTOM_RIGHT,
    };

    enum PopupConstraintAdjustment : uint32_t {
        POPUP_CONSTRAINT_NONE = 0,
        POPUP_CONSTRAINT_SLIDE_X = 1u << 0,
        POPUP_CONSTRAINT_SLIDE_Y = 1u << 1,
        POPUP_CONSTRAINT_FLIP_X = 1u << 2,
        POPUP_CONSTRAINT_FLIP_Y = 1u << 3,
        POPUP_CONSTRAINT_RESIZE_X = 1u << 4,
        POPUP_CONSTRAINT_RESIZE_Y = 1u << 5,
    };

    struct PopupPositioner {
        bool use_explicit_anchor_rect = false;
        int anchor_rect_x = 0;
        int anchor_rect_y = 0;
        int anchor_rect_w = 1;
        int anchor_rect_h = 1;

        bool use_offset = false;
        int offset_x = 0;
        int offset_y = 0;

        PopupAnchor anchor = PopupAnchor::TOP_LEFT;
        PopupGravity gravity = PopupGravity::TOP_LEFT;
        uint32_t constraint_adjustment = POPUP_CONSTRAINT_NONE;
        bool reactive = false;

        bool use_parent_size = false;
        int parent_w = 0;
        int parent_h = 0;

        bool use_parent_configure = false;
        uint32_t parent_configure_serial = 0;
    };

    std::string name;
    std::string monitor_name;
    PositioningInfo pos;
    int alignment = 0; // 0 none, 1 top, clockwise + 1
    PopupPositioner popup;
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
    RawWindow *open_popup(RawWindow *parent, RawWindowSettings settings);

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
