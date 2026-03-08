#pragma once

namespace alt_tab {
    void on_window_open(int id);
    void on_window_closed(int id);
    void on_activated(int id); // on a window become the focused window that is
    
    void show();
    void close(bool focus);
    void show_reticle(bool state);
    
    void visual_offset(float scalar);
    
    void move(int dir);
    
    void move_x(int dir);
    void move_y(int dir);

    bool at_start_row();
    bool at_end_row();
    bool at_start_col();
    bool at_end_col();

    bool showing();
}
