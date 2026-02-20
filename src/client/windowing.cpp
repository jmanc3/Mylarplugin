#include "client/windowing.h"

#include "heart.h"
#include "events.h"
#include <wayland-server-protocol.h>

std::vector<MylarWindow *> mylar_windows;

static MylarWindow *mylar(RawWindow *rw) {
    for (auto m : mylar_windows) {
        if (m->raw_window == rw) {
            return m;
        }
    }
    return nullptr;
}

bool on_mouse_move(RawWindow *rw, float x, float y) {
    std::lock_guard<std::mutex> lock(rw->creator->mutex);
    log("on_mouse_move");
    x *= rw->dpi;
    y *= rw->dpi;
    auto m = mylar(rw);
    if (!m) return false;
    ::layout(m->root, m->root, m->root->real_bounds);
    Event event(x, y);
    move_event(m->root, event);
    return false;
}

bool on_mouse_press(RawWindow *rw, int button, int state, float x, float y) {
    std::lock_guard<std::mutex> lock(rw->creator->mutex);
    log("on_mouse_press");
    x *= rw->dpi;
    y *= rw->dpi;
    auto m = mylar(rw);
    if (!m) return false;
    ::layout(m->root, m->root, m->root->real_bounds);
    Event event(x, y, button, state);
    mouse_event(m->root, event);
    return false;
}

bool on_scrolled(RawWindow *rw, int source, int axis, int direction, double delta, int discrete, bool mouse) {
    std::lock_guard<std::mutex> lock(rw->creator->mutex);
    // delta 3820
    log("on_scrolled");
    auto m = mylar(rw);
    if (!m) return false;
    ::layout(m->root, m->root, m->root->real_bounds);
    Event event;
    event.x = m->root->mouse_current_x;
    event.y = m->root->mouse_current_y;
    event.scroll = true;
    event.source = source;
    event.axis = axis;
    event.direction = direction;
    event.delta = delta;
    event.descrete = discrete;
    event.from_mouse = mouse;
    mouse_event(m->root, event);
    return false;
}

bool on_key_press(RawWindow *rw, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text) {
    std::lock_guard<std::mutex> lock(rw->creator->mutex);
    //on_key_press(rw, key, pressed, sym, mods, is_text, text);
    auto m = mylar(rw);
    if (!m) return false;
    ::layout(m->root, m->root, m->root->real_bounds);
    key_press(m->root, key, pressed, sym, mods, is_text, text);

    log("on_key_press");
    return false;
}
    
bool on_mouse_enters(RawWindow *rw, float x, float y) {
    //std::lock_guard<std::mutex> lock(rw->creator->mutex);
    log("on_mouse_enters");
    on_mouse_move(rw, x, y);
    return false;
}
    
bool on_mouse_leaves(RawWindow *rw, float x, float y) {
    //std::lock_guard<std::mutex> lock(rw->creator->mutex);
    log("on_mouse_leaves");
    on_mouse_move(rw, -1000, -1000);
    return false;
}

bool on_keyboard_focus(RawWindow *rw, bool gained) {
    std::lock_guard<std::mutex> lock(rw->creator->mutex);
    log("on_keyboard_focus");
    return false;
}
    
void on_render(RawWindow *rw, int w, int h) {
    std::lock_guard<std::mutex> lock(rw->creator->mutex);
    log("on_render");
    auto m = mylar(rw);
    if (!m) return;
    m->root->real_bounds = Bounds(0, 0, w, h);
    m->root->wanted_bounds = m->root->real_bounds;
    ::layout(m->root, m->root, m->root->real_bounds);
    auto cr = rw->cr;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR); 
    cairo_paint(cr);
    cairo_restore(cr);
    paint_root(m->root);
}

void on_resize(RawWindow *rw, int w, int h) {
    std::lock_guard<std::mutex> lock(rw->creator->mutex);
    log("on_resize");
    auto m = mylar(rw);
    if (!m) return;
    m->root->real_bounds = Bounds(0, 0, w, h);
}

void on_close(RawWindow *rw) {
    std::lock_guard<std::mutex> lock(rw->creator->mutex);
    auto m = mylar(rw);
    if (!m) return;
    for (int i = mylar_windows.size() - 1; i >= 0; i--)
        if (m == mylar_windows[i])
            mylar_windows.erase(mylar_windows.begin() + i);
}

static void wire_handlers(MylarWindow *m) {
    m->raw_window->on_mouse_move = on_mouse_move;
    m->raw_window->on_mouse_press = on_mouse_press;
    m->raw_window->on_scrolled = on_scrolled;
    m->raw_window->on_key_press = on_key_press;
    m->raw_window->on_mouse_enters = on_mouse_enters;
    m->raw_window->on_mouse_leaves = on_mouse_leaves;
    m->raw_window->on_keyboard_focus = on_keyboard_focus;
    m->raw_window->on_render = on_render;
    m->raw_window->on_resize = on_resize;
    m->raw_window->on_close = on_close;
}

MylarWindow *open_mylar_window(RawApp *app, WindowType type, RawWindowSettings settings) {
    auto m = new MylarWindow;
    m->raw_window = windowing::open_window(app, type, settings);
    assert(m->raw_window);
    m->root = new Container();
    m->root->real_bounds = Bounds(0, 0, settings.pos.w, settings.pos.h);
    wire_handlers(m);
    mylar_windows.push_back(m);
    return m;
}

MylarWindow *open_mylar_popup(MylarWindow *parent, RawWindowSettings settings) {
    if (!parent || !parent->raw_window)
        return nullptr;

    auto m = new MylarWindow;
    m->raw_window = windowing::open_popup(parent->raw_window, settings);
    assert(m->raw_window);
    m->root = new Container();
    m->root->real_bounds = Bounds(0, 0, settings.pos.w, settings.pos.h);
    wire_handlers(m);
    mylar_windows.push_back(m);
    return m;
}
