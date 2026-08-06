#pragma once

extern int minimize_gesture_count;

namespace show_desktop {
    bool is_opened();
    
    void start();
    void stop();
    
    void set_scalar(float scalar);
    float get_scalar();

    void minimize_animate_out(long start, long end, float y_offset, float scalar_at_start);

    void stop_animation();
    void start_animation();

    void render();
}

