#include "pango_font_cache.h"

#include <list>
#include <memory>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

namespace {

struct ObjectUnref {
    template <typename T>
    void operator()(T *object) const { g_object_unref(object); }
};

struct DescriptionFree {
    void operator()(PangoFontDescription *description) const {
        pango_font_description_free(description);
    }
};

struct CachedFont {
    std::string name;
    int size;
    PangoWeight weight;
    bool italic;
    std::unique_ptr<PangoFontDescription, DescriptionFree> description;
    std::unique_ptr<PangoLayout, ObjectUnref> layout;

    CachedFont(PangoFontMap *map, const std::string &name, int size,
               PangoWeight weight, bool italic)
        : name(name), size(size), weight(weight), italic(italic),
          description(pango_font_description_new()) {
        pango_font_description_set_family(description.get(), name.c_str());
        pango_font_description_set_size(description.get(), size * PANGO_SCALE);
        pango_font_description_set_weight(description.get(), weight);
        pango_font_description_set_style(description.get(), italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
        std::unique_ptr<PangoContext, ObjectUnref> context(pango_font_map_create_context(map));
        layout.reset(pango_layout_new(context.get()));
    }
};

struct FontCache {
    // Declare the map first so layouts and their contexts are destroyed before it.
    // Owning a private map avoids changing the thread's default map used elsewhere.
    std::unique_ptr<PangoFontMap, ObjectUnref> map{pango_cairo_font_map_new()};
    std::list<CachedFont> fonts;
};

thread_local std::unique_ptr<FontCache> cache;
constexpr size_t max_cached_fonts = 64;

void prepare_layout(cairo_t *cr, CachedFont &font) {
    auto layout = font.layout.get();
    // No Cairo pointer is retained or used as an identity: buffer contexts may
    // be replaced, have their transform changed, or reuse a freed address.
    pango_cairo_update_layout(cr, layout);
    pango_layout_set_font_description(layout, font.description.get());
    pango_layout_set_attributes(layout, nullptr);
    pango_layout_set_text(layout, "", 0);
    pango_layout_set_width(layout, -1);
    pango_layout_set_height(layout, -1);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_indent(layout, 0);
    pango_layout_set_spacing(layout, 0);
    pango_layout_set_line_spacing(layout, 0);
    pango_layout_set_justify(layout, false);
    pango_layout_set_justify_last_line(layout, false);
    pango_layout_set_auto_dir(layout, true);
    pango_layout_set_single_paragraph_mode(layout, false);
    pango_layout_set_tabs(layout, nullptr);
}

}

PangoLayout *get_cached_pango_font(cairo_t *cr, const std::string &name,
                                 int pixel_height, PangoWeight weight, bool italic) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    if (!cache)
        cache = std::make_unique<FontCache>();

    auto &fonts = cache->fonts;
    for (auto it = fonts.begin(); it != fonts.end(); ++it) {
        if (it->name == name && it->size == pixel_height &&
            it->weight == weight && it->italic == italic) {
            fonts.splice(fonts.begin(), fonts, it);
            prepare_layout(cr, fonts.front());
            return fonts.front().layout.get();
        }
    }

    fonts.emplace_front(cache->map.get(), name, pixel_height, weight, italic);
    if (fonts.size() > max_cached_fonts)
        fonts.pop_back();
    prepare_layout(cr, fonts.front());
    return fonts.front().layout.get();
}

void cleanup_cached_pango_fonts() {
    cache.reset();
}
