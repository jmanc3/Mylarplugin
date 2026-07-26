#pragma once

namespace show_desktop {
    bool is_opened();
    
    void start();
    void stop(bool animate = true);

    void render();
}

