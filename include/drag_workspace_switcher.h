#ifndef drag_workspace_switcher_h_INCLUDED
#define drag_workspace_switcher_h_INCLUDED

namespace drag_workspace_switcher {
    void open();
    void close();
    void force_hold_open(bool state);

    void click(int id, int button, int state, float x, float y);
    void on_mouse_move(int x, int y); 
};


#endif // drag_workspace_switcher_h_INCLUDED
