#pragma once

#include <vector>
#include <xkbcommon/xkbcommon.h>
#include <string>

struct Container;

struct Event {
    float x;
    float y;

    int button;
    int state;

    int source = 0;
    bool scroll = false;
    int axis = 0;
    int direction = 0;
    double delta = 0.0;
    int descrete = 0;
    bool from_mouse = false;
 

    Event(float x, float y, int button, int state) : x(x), y(y), button(button), state(state) {
       ; 
    }
    
    Event(float x, float y) : x(x), y(y) {
       ; 
    }

    Event () { 
        ;
    }
};

void mouse_entered(Container*, const Event&);
void mouse_left(Container*, const Event&);
void move_event(Container*, const Event&);
void mouse_event(Container*, const Event&);
void key_press(Container*, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text, std::string text);

void set_active(Container* root, const std::vector<Container*>& active_containers, Container* c, bool state, bool viakey);

void paint_root(Container*);
void paint_outline(Container*, Container*);

std::vector<Container*> pierced_containers(Container* root, int x, int y);

