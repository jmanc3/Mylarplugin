#ifndef titlebar_h_INCLUDED
#define titlebar_h_INCLUDED

#include <string>

namespace titlebar {
    void on_window_open(int id);
    void on_window_closed(int id);
    void on_draw_decos(std::string name, int monitor, int id, float a, int deco_offset_x, int deco_offset_y);
    void on_activated(int id);

    // bottom_y >= 0 places the menu so its bottom edge sits at bottom_y (global logical px).
    void titlebar_right_click(int cid, bool centered = false, int bottom_y = -1);
}

struct Container;
struct RGBA;
struct TextureInfo;

TextureInfo *get_cached_texture(Container *root_with_scale, Container *container_texture_saved_on, std::string needle, std::string font, std::string text, RGBA color, int wanted_h);
#endif // titlebar_h_INCLUDED
