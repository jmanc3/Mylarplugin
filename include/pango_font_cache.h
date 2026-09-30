#ifndef MYLAR_PANGO_FONT_CACHE_H
#define MYLAR_PANGO_FONT_CACHE_H

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
// lookup lazily starts a new cache. Normal thread exit does this automatically,
// so after all rendering threads exit all their caches have been released.
// A shutdown coordinator must not free another thread's in-use layouts.
void cleanup_cached_pango_fonts();

#endif
