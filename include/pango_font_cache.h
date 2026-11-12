#ifndef MYLAR_PANGO_FONT_CACHE_H
#define MYLAR_PANGO_FONT_CACHE_H

#include "drawing/context.h"

#include <pango/pangocairo.h>
#include <string>

// Borrowed scratch layout, confined to the calling thread. Finish measuring or
// drawing before the next lookup or cleanup on that thread. Do not unref it,
// retain it in widget state, or pass it to another thread. The caller must also
// keep cr confined to its rendering thread while using the returned layout.
// Each lookup restores layout defaults and applies the current Cairo context.
// The historical pixel_height argument retains its existing Pango point sizing.
PangoLayout *get_cached_pango_font(cairo_t *cr, const std::string &name,
                                 int pixel_height, PangoWeight weight, bool italic);

// Releases only the calling thread's layouts and private font map. Safe to call
// repeatedly once that thread has finished using its borrowed layouts; another
// lookup lazily starts a new cache. Every rendering thread must call this before
// exiting, and the compositor thread must call it during plugin shutdown.
// A shutdown coordinator must not free another thread's in-use layouts.
// Raw windowing apps: layouts are prepared against a thread-local scratch Cairo
// context; drawing them goes through drawing::Context::show_layout.
inline PangoLayout *get_cached_pango_font(drawing::Context *, const std::string &name,
                                          int pixel_height, PangoWeight weight, bool italic) {
    return get_cached_pango_font(drawing::scratch_cairo(), name, pixel_height, weight, italic);
}

void cleanup_cached_pango_fonts();

#endif
