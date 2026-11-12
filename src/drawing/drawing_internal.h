#pragma once

#include "drawing/context.h"
#include <memory>
#include <string>
#include <vector>

namespace drawing {
// Identity of a cairo surface's current pixels. Expires when the surface is
// destroyed or invalidate_surface is called, so backend caches never reuse a
// stale texture (even if a new surface reuses a freed address).
std::shared_ptr<const char> surface_token(cairo_surface_t *surface);

// Everything that affects how a layout rasterizes.
std::string layout_key(PangoLayout *layout);
// True if the layout may produce glyphs whose color is not the source color
// (foreground attributes or color emoji).
bool layout_has_own_colors(PangoLayout *layout);

struct TextRaster {
    int x = 0, y = 0, width = 0, height = 0; // ink box relative to the layout origin
    bool rgba = false;                       // false: one coverage byte per pixel
    std::vector<unsigned char> pixels;       // rgba: straight RGBA bytes, premultiplied
};
// Cairo/Pango rasterization of a layout. Coverage rasters are tinted at draw
// time; RGBA rasters are rendered with the given color.
TextRaster rasterize_layout(PangoLayout *layout, bool rgba, const double color[4]);
} // namespace drawing
