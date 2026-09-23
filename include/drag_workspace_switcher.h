#ifndef drag_workspace_switcher_h_INCLUDED
#define drag_workspace_switcher_h_INCLUDED

#include <string>

struct Bounds;

namespace drag_workspace_switcher {
    void open();
    void close();
    void close_visually();
    bool drop_window(int cid);
    
    void force_hold_open(bool state);
    void set_overwrite_monitor(int rid);

    // does a click animation on the container uuid 
    void press(std::string uuid);

    void click(int id, int button, int state, float x, float y);
    void on_mouse_move(int x, int y); 

    void begin_drag(int cid, bool from_overview = false);
    void end_drag(int cid);
    void update_drag();
    // Overview supplies logical bounds; native drags use the live window bounds.
    void transform_thumbnail(int cid, Bounds &bounds, float &alpha);
    bool replaces_window(int cid);
    bool omit_from_workspace_snapshot(int cid);
    // Paint the native replacement at its old stacking position, or the raised
    // copy after the entire switcher has painted, just before the cursor.
    void paint_drag(int monitor, bool above_switcher);
};


#endif // drag_workspace_switcher_h_INCLUDED
