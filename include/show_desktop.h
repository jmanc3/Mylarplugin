#pragma once

namespace show_desktop {
    bool is_opened();
    
    void start();
    void stop();
    
    void set_scalar(float scalar);
    float get_scalar();

    void render();
}

