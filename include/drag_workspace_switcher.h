#ifndef drag_workspace_switcher_h_INCLUDED
#define drag_workspace_switcher_h_INCLUDED

#include <string>

namespace drag_workspace_switcher {
    void open();
    void close();
    void close_visually();
    
    void force_hold_open(bool state);

    // does a click animation on the container uuid 
    void press(std::string uuid);

    void click(int id, int button, int state, float x, float y);
    void on_mouse_move(int x, int y); 
};


#endif // drag_workspace_switcher_h_INCLUDED
