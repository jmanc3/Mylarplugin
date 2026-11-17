#include "icon_browser.h"

#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "container.h"
#include "events.h"
#include "heart.h"
#include "hypriso.h"
#include "icons.h"
#include "pango_font_cache.h"
#include "settings.h"
#include "smooth_scroll.h"
#include "text_editor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <pango/pangocairo.h>
#include <thread>
#include <unordered_map>
#include <xkbcommon/xkbcommon-keysyms.h>

// One icon: a name in one theme.
struct BrowserIcon {
    std::string name;
    std::string lower;
    std::string theme;
    std::string path;
    IconContext context = IconContext::NotSet;
    // 0 for the theme that would be picked for this name, then the other themes in order.
    int rank = 0;
};

struct BrowserIndex {
    // Every name in every theme that has it.
    std::vector<BrowserIcon> variants;
    // Every name once, in the theme that would be picked for it, sorted by name.
    std::vector<BrowserIcon> best;
};

// Built on a thread of its own, since it looks at every icon of every theme.
struct IndexJob {
    std::atomic<bool> done = false;
    std::shared_ptr<BrowserIndex> result;
};

// "Add icon": the file chooser, copying the image into the local icons and the rescan,
// all on a thread of its own.
struct AddIconJob {
    std::atomic<bool> done = false;
    std::string name; // the new icon, empty when cancelled or failed
    std::string removed; // set instead when the job removed an icon
    std::string error;
    std::shared_ptr<BrowserIndex> index;
};

struct Category {
    IconContext context;
    const char *label;
};

// The same contexts yad-icon-browser lists, plus the icons that are in none of them.
static const Category categories[] = {
    {IconContext::Actions, "Actions"},
    {IconContext::Animations, "Animations"},
    {IconContext::Apps, "Applications"},
    {IconContext::Categories, "Categories"},
    {IconContext::Devices, "Devices"},
    {IconContext::Emblems, "Emblems"},
    {IconContext::Emotes, "Emotes"},
    {IconContext::Intl, "International"},
    {IconContext::Mimetypes, "MIME Types"},
    {IconContext::Places, "Places"},
    {IconContext::Statuses, "Status"},
    {IconContext::Panel, "Panel"},
    {IconContext::NotSet, "Other"},
};
static constexpr int category_count = sizeof(categories) / sizeof(categories[0]);

static const char *context_label(IconContext context) {
    for (auto &category : categories)
        if (category.context == context)
            return category.label;
    return "Other";
}

// Scroll position and selection of one of the painted lists.
struct ListState {
    float scroll = 0; // 0 at the top, negative once scrolled down
    float overflow = 0;
    float view_h = 0;
    SmoothScroll smooth;
    int hovered = -1;
    int selected = -1;
    bool dragging_scrollbar = false;
    float grab_offset = 0;
    long last_click = 0;
    int last_click_row = -1;
};

struct BrowserData : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    Container *field = nullptr;

    std::shared_ptr<IndexJob> job;
    std::shared_ptr<BrowserIndex> index;

    std::string query;
    std::vector<int> results; // indices into index->variants
    std::vector<int> by_category[category_count]; // indices into index->best
    std::vector<int> shown_categories; // indices into categories[], the non-empty ones
    int category = 0; // index into shown_categories

    ListState category_list;
    ListState icon_list;

    std::shared_ptr<AddIconJob> add_job;
    // "Remove icon" asks for a second click on the same icon before this time.
    std::string remove_armed_path;
    long remove_armed_until = 0;

    // A line in the footer ("Copied ...", "Added ..."), shown until notice_until.
    std::string notice;
    long notice_until = 0;
    bool notice_error = false;

    // Icons are loaded the first time their row shows.
    std::unordered_map<std::string, cairo_surface_t *> surfaces;
    int surfaces_size = 0;
    cairo_surface_t *preview = nullptr;
    std::string preview_path;
    int preview_size = 0;

    ~BrowserData() override {
        for (auto &[path, surface] : surfaces)
            if (surface)
                cairo_surface_destroy(surface);
        if (preview)
            cairo_surface_destroy(preview);
    }
};

static constexpr int row_h = 36;
static constexpr int row_icon_size = 24;
static constexpr int category_row_h = 32;
static constexpr int list_pad = 4;
static constexpr int footer_h = 76;
static constexpr int preview_icon_size = 48;
static const RGBA accent_color(.23, .6, 1, 1);

static BrowserData *browser(Container *root) {
    return static_cast<BrowserData *>(root->user_data);
}

static RawWindow *browser_window(Container *root) {
    return browser(root)->window->raw_window;
}

static void redraw(Container *root) {
    windowing::redraw(browser_window(root));
}

static void rounded_rect(drawing::Context *cr, double radius, double x, double y, double w, double h) {
    double degrees = M_PI / 180.0;
    radius = std::min(radius, std::min(w, h) * .5);
    cr->new_sub_path();
    cr->arc(x + w - radius, y + radius, radius, -90 * degrees, 0 * degrees);
    cr->arc(x + w - radius, y + h - radius, radius, 0 * degrees, 90 * degrees);
    cr->arc(x + radius, y + h - radius, radius, 90 * degrees, 180 * degrees);
    cr->arc(x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
    cr->close_path();
}

// Draws (or only measures) one line of text, ellipsized at max_w when given.
static Bounds text(drawing::Context *cr, double x, double y, const std::string &s, int size, RGBA color, bool bold = false,
                   bool draw = true, double max_w = -1, std::string font = "") {
    auto layout = get_cached_pango_font(cr, font.empty() ? set->font : font, size, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_text(layout, s.data(), s.size());
    pango_layout_set_wrap(layout, PANGO_WRAP_NONE);
    pango_layout_set_height(layout, -1);
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    if (max_w > 0) {
        pango_layout_set_width(layout, std::max(1, (int) (max_w * PANGO_SCALE)));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    } else {
        pango_layout_set_width(layout, -1);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    }
    PangoRectangle logical = {};
    pango_layout_get_pixel_extents(layout, nullptr, &logical);
    if (draw) {
        set_argb(cr, color);
        cr->move_to(std::round(x), std::round(y));
        cr->show_layout(layout);
    }
    return Bounds(0, 0, logical.width, logical.height);
}

static std::shared_ptr<BrowserIndex> build_index() {
    auto index = std::make_shared<BrowserIndex>();
    // The icon cache is loaded in the background when the desktop starts.
    for (int i = 0; i < 600 && !icons_loaded; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!icons_loaded)
        return index;

    std::vector<std::string> names;
    get_options(names, "", 0);
    std::vector<IconTarget> targets;
    targets.reserve(names.size());
    for (auto &name : names)
        targets.emplace_back(name);
    search_icons(targets);
    pick_best(targets, 32);

    auto strip = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
        return s;
    };
    for (auto &target : targets) {
        if (target.candidates.empty())
            continue;
        // Candidates are sorted best first, so the first one of each theme is the one to show.
        std::vector<std::string> themes;
        for (auto &candidate : target.candidates) {
            auto theme = strip(candidate.theme);
            if (std::find(themes.begin(), themes.end(), theme) != themes.end())
                continue;
            themes.push_back(theme);
            BrowserIcon icon;
            icon.name = strip(candidate.filename);
            icon.lower = to_lower(icon.name);
            icon.theme = theme;
            icon.path = candidate.full_path();
            icon.context = candidate.context;
            icon.rank = themes.size() - 1;
            if (icon.rank == 0)
                index->best.push_back(icon);
            index->variants.push_back(std::move(icon));
        }
    }
    std::sort(index->best.begin(), index->best.end(), [](const BrowserIcon &a, const BrowserIcon &b) {
        return a.lower != b.lower ? a.lower < b.lower : a.name < b.name;
    });
    return index;
}

// The last index built, shown right away the next time the window opens while a fresh one is built.
static std::mutex last_index_mutex;
static std::shared_ptr<BrowserIndex> last_index;

static bool searching(BrowserData *data) {
    return !data->query.empty();
}

static int row_count(BrowserData *data) {
    if (!data->index)
        return 0;
    if (searching(data))
        return data->results.size();
    if (data->category < 0 || data->category >= (int) data->shown_categories.size())
        return 0;
    return data->by_category[data->shown_categories[data->category]].size();
}

static const BrowserIcon *row_icon(BrowserData *data, int i) {
    if (i < 0 || i >= row_count(data))
        return nullptr;
    if (searching(data))
        return &data->index->variants[data->results[i]];
    return &data->index->best[data->by_category[data->shown_categories[data->category]][i]];
}

static void reset_list(ListState &list) {
    list.scroll = 0;
    list.smooth.stop();
    list.hovered = -1;
    list.selected = -1;
    list.dragging_scrollbar = false;
}

static void run_search(BrowserData *data) {
    data->results.clear();
    reset_list(data->icon_list);
    if (!data->index || data->query.empty())
        return;
    auto query = to_lower(data->query);
    auto &variants = data->index->variants;
    for (int i = 0; i < (int) variants.size(); i++)
        if (variants[i].lower.find(query) != std::string::npos)
            data->results.push_back(i);
    // Exact matches, then names starting with the text, then the shortest; each name's themes stay together.
    std::sort(data->results.begin(), data->results.end(), [&variants, &query](int ia, int ib) {
        auto &a = variants[ia];
        auto &b = variants[ib];
        bool ea = a.lower == query, eb = b.lower == query;
        if (ea != eb)
            return ea;
        bool pa = a.lower.starts_with(query), pb = b.lower.starts_with(query);
        if (pa != pb)
            return pa;
        if (a.lower.size() != b.lower.size())
            return a.lower.size() < b.lower.size();
        if (a.lower != b.lower)
            return a.lower < b.lower;
        if (a.name != b.name)
            return a.name < b.name;
        return a.rank < b.rank;
    });
    if (!data->results.empty())
        data->icon_list.selected = 0;
}

static void index_ready(BrowserData *data, std::shared_ptr<BrowserIndex> index) {
    // A fresh index can replace the one shown since the window opened: keep what the user is looking at.
    bool refresh = data->index != nullptr;
    IconContext context = IconContext::Apps;
    if (refresh && data->category >= 0 && data->category < (int) data->shown_categories.size())
        context = categories[data->shown_categories[data->category]].context;
    std::string selected_name, selected_theme;
    if (auto icon = row_icon(data, data->icon_list.selected)) {
        selected_name = icon->name;
        selected_theme = icon->theme;
    }
    float scroll = data->icon_list.scroll;
    float category_scroll = data->category_list.scroll;

    data->index = std::move(index);
    for (auto &list : data->by_category)
        list.clear();
    for (int i = 0; i < (int) data->index->best.size(); i++) {
        auto context = data->index->best[i].context;
        for (int c = 0; c < category_count; c++)
            if (categories[c].context == context)
                data->by_category[c].push_back(i);
    }
    data->shown_categories.clear();
    for (int c = 0; c < category_count; c++)
        if (!data->by_category[c].empty())
            data->shown_categories.push_back(c);
    // Start on Applications, like yad.
    data->category = 0;
    for (int i = 0; i < (int) data->shown_categories.size(); i++)
        if (categories[data->shown_categories[i]].context == context)
            data->category = i;
    data->category_list.selected = data->category;
    reset_list(data->icon_list);
    run_search(data);
    if (!refresh)
        return;
    data->category_list.scroll = category_scroll;
    data->icon_list.scroll = scroll;
    if (!selected_name.empty()) {
        data->icon_list.selected = -1;
        for (int i = 0, count = row_count(data); i < count; i++) {
            auto icon = row_icon(data, i);
            if (icon->name == selected_name && icon->theme == selected_theme) {
                data->icon_list.selected = i;
                break;
            }
        }
    }
}

static void watch_index(BrowserData *data) {
    auto app = data->app;
    auto job = data->job;
    windowing::timer(app, 50, [app, data, job](void *) {
        {
            std::lock_guard<std::mutex> lock(app->mutex);
            if (data->job != job)
                return;
            if (!job->done) {
                watch_index(data);
                return;
            }
            index_ready(data, job->result);
        }
        windowing::redraw(data->window->raw_window);
    }, nullptr);
}

static cairo_surface_t *icon_surface(BrowserData *data, const std::string &path, int size) {
    if (data->surfaces_size != size || data->surfaces.size() > 3000) {
        for (auto &[p, surface] : data->surfaces)
            if (surface)
                cairo_surface_destroy(surface);
        data->surfaces.clear();
        data->surfaces_size = size;
    }
    auto found = data->surfaces.find(path);
    if (found != data->surfaces.end())
        return found->second;
    cairo_surface_t *surface = nullptr;
    if (!path.empty())
        load_icon_full_path(&surface, path, size);
    data->surfaces[path] = surface;
    return surface;
}

// ---- Painted lists ----

static Bounds list_area(Container *c, double dpi) {
    double pad = list_pad * dpi;
    auto b = c->real_bounds;
    return Bounds(b.x + pad, b.y + pad, std::max(0.0, b.w - pad * 2), std::max(0.0, b.h - pad * 2));
}

struct Scrollbar {
    Bounds track;
    Bounds thumb;
};

static Scrollbar list_scrollbar(const ListState &list, Bounds area, double dpi) {
    Scrollbar bar;
    bar.track = Bounds(area.x + area.w - 10 * dpi, area.y, 10 * dpi, area.h);
    double content = area.h + list.overflow;
    double thumb_h = std::max(24.0 * dpi, content > 0 ? area.h * area.h / content : area.h);
    thumb_h = std::min(thumb_h, area.h);
    double y = area.y + (list.overflow > 0 ? (-list.scroll / list.overflow) * (area.h - thumb_h) : 0);
    bar.thumb = Bounds(bar.track.x, y, bar.track.w, thumb_h);
    return bar;
}

static void scroll_to_thumb_top(ListState &list, Bounds area, double dpi, double thumb_top) {
    auto bar = list_scrollbar(list, area, dpi);
    double range = bar.track.h - bar.thumb.h;
    double t = range > 0 ? std::clamp((thumb_top - bar.track.y) / range, 0.0, 1.0) : 0;
    list.smooth.stop();
    list.scroll = (float) (-t * list.overflow);
}

static void scroll_to_row(ListState &list, int row, double row_height) {
    if (row < 0)
        return;
    double top = row * row_height;
    list.smooth.stop();
    if (top + list.scroll < 0)
        list.scroll = -top;
    if (top + row_height + list.scroll > list.view_h)
        list.scroll = -(top + row_height - list.view_h);
    list.scroll = std::clamp(list.scroll, -list.overflow, 0.0f);
}

static int row_under_mouse(Container *root, Container *c, const ListState &list, int count, double row_height) {
    auto dpi = browser_window(root)->dpi;
    auto area = list_area(c, dpi);
    if (!bounds_contains(area, root->mouse_current_x, root->mouse_current_y))
        return -1;
    int i = (int) std::floor((root->mouse_current_y - area.y - list.scroll) / row_height);
    return i >= 0 && i < count ? i : -1;
}

// The card behind a list, the visible rows, and a scrollbar while it overflows.
static void paint_list(Container *root, Container *c, ListState &list, int count, double row_height,
                       const std::function<void(int, Bounds)> &paint_row) {
    auto window = browser_window(root);
    auto cr = window->drawing_context;
    auto dpi = window->dpi;
    auto b = c->real_bounds;
    if (b.w <= 0 || b.h <= 0)
        return;

    set_argb(cr, {1, 1, 1, 1});
    rounded_rect(cr, 8 * dpi, b.x, b.y, b.w, b.h);
    cr->fill();
    set_argb(cr, {0, 0, 0, .14});
    cr->set_line_width(std::max(1.0, (double) std::floor(dpi)));
    rounded_rect(cr, 8 * dpi, b.x + .5, b.y + .5, b.w - 1, b.h - 1);
    cr->stroke();

    auto area = list_area(c, dpi);
    list.view_h = area.h;
    list.overflow = (float) std::max(0.0, count * row_height - area.h);
    if (list.smooth.step(list.scroll))
        windowing::redraw(window);
    list.scroll = std::clamp(list.scroll, -list.overflow, 0.0f);

    cr->save();
    cr->rectangle(area.x, area.y, area.w, area.h);
    cr->clip_path();
    int first = std::max(0, (int) std::floor(-list.scroll / row_height));
    for (int i = first; i < count; i++) {
        double y = area.y + list.scroll + i * row_height;
        if (y > area.y + area.h)
            break;
        Bounds rb(area.x, y, area.w - (list.overflow > 0 ? 10 * dpi : 0), row_height);
        if (i == list.selected || i == list.hovered) {
            set_argb(cr, i == list.selected ? RGBA(accent_color.r, accent_color.g, accent_color.b, .2)
                                            : RGBA(0, 0, 0, .05));
            rounded_rect(cr, 5 * dpi, rb.x, rb.y, rb.w, rb.h);
            cr->fill();
        }
        paint_row(i, rb);
    }
    cr->restore();

    if (list.overflow > 0) {
        auto bar = list_scrollbar(list, area, dpi);
        bool hot = list.dragging_scrollbar ||
                   (c->state.mouse_hovering && bounds_contains(bar.track, root->mouse_current_x, root->mouse_current_y));
        double w = (hot ? 6 : 3) * dpi;
        set_argb(cr, {0, 0, 0, list.dragging_scrollbar ? .45 : hot ? .35 : .25});
        rounded_rect(cr, w * .5, bar.thumb.x + bar.thumb.w - w, bar.thumb.y, w, bar.thumb.h);
        cr->fill();
    }
}

static void focus_field(Container *root) {
    auto data = browser(root);
    if (data->field && !data->field->active)
        set_active(root, {data->field}, root, true, false);
}

// Mouse handling shared by both lists: hover, wheel, scrollbar dragging and clicks.
static void setup_list(Container *c, ListState BrowserData::*member, std::function<int(BrowserData *)> count,
                       std::function<double(double dpi)> row_height, std::function<void(Container *, int, bool)> on_click) {
    c->when_drag_end_is_click = false;
    c->when_mouse_motion = [member, count, row_height](Container *root, Container *c) {
        auto data = browser(root);
        auto &list = data->*member;
        int hovered = row_under_mouse(root, c, list, count(data), row_height(browser_window(root)->dpi));
        auto area = list_area(c, browser_window(root)->dpi);
        if (list.overflow > 0 && bounds_contains(list_scrollbar(list, area, browser_window(root)->dpi).track,
                                                 root->mouse_current_x, root->mouse_current_y))
            hovered = -1;
        list.hovered = hovered;
        redraw(root);
    };
    c->when_mouse_enters_container = c->when_mouse_motion;
    c->when_mouse_leaves_container = [member](Container *root, Container *c) {
        (browser(root)->*member).hovered = -1;
        redraw(root);
    };
    c->when_fine_scrolled = [member](Container *root, Container *c, double, double scroll_y, bool touchpad) {
        auto &list = browser(root)->*member;
        auto dpi = browser_window(root)->dpi;
        float delta = (float) (scroll_y * 3 * dpi);
        if (touchpad) {
            list.smooth.stop();
            list.scroll = std::clamp(list.scroll + delta, -list.overflow, 0.0f);
        } else {
            list.smooth.scroll_by(list.scroll, delta, -list.overflow, 0.0f);
        }
        redraw(root);
    };
    c->when_mouse_down = [member](Container *root, Container *c) {
        auto &list = browser(root)->*member;
        auto dpi = browser_window(root)->dpi;
        list.dragging_scrollbar = false;
        if (list.overflow <= 0)
            return;
        auto area = list_area(c, dpi);
        auto bar = list_scrollbar(list, area, dpi);
        if (!bounds_contains(bar.track, root->mouse_current_x, root->mouse_current_y))
            return;
        // Pressing the track away from the thumb jumps the thumb there first.
        if (!bounds_contains(bar.thumb, root->mouse_current_x, root->mouse_current_y)) {
            scroll_to_thumb_top(list, area, dpi, root->mouse_current_y - bar.thumb.h * .5);
            bar = list_scrollbar(list, area, dpi);
        }
        list.dragging_scrollbar = true;
        list.grab_offset = root->mouse_current_y - bar.thumb.y;
        redraw(root);
    };
    auto drag = [member](Container *root, Container *c) {
        auto &list = browser(root)->*member;
        if (!list.dragging_scrollbar)
            return;
        auto dpi = browser_window(root)->dpi;
        scroll_to_thumb_top(list, list_area(c, dpi), dpi, root->mouse_current_y - list.grab_offset);
        redraw(root);
    };
    c->when_drag_start = drag;
    c->when_drag = drag;
    auto end_drag = [member](Container *root, Container *c) {
        auto &list = browser(root)->*member;
        if (!list.dragging_scrollbar)
            return;
        list.dragging_scrollbar = false;
        focus_field(root);
        redraw(root);
    };
    c->when_drag_end = end_drag;
    c->when_mouse_up = end_drag;
    c->when_clicked = [member, count, row_height, on_click](Container *root, Container *c) {
        auto data = browser(root);
        auto &list = data->*member;
        auto dpi = browser_window(root)->dpi;
        auto area = list_area(c, dpi);
        if (list.dragging_scrollbar ||
            (list.overflow > 0 && bounds_contains(list_scrollbar(list, area, dpi).track, root->mouse_initial_x, root->mouse_initial_y))) {
            list.dragging_scrollbar = false;
            focus_field(root);
            return;
        }
        int i = row_under_mouse(root, c, list, count(data), row_height(dpi));
        if (i != -1) {
            long now = get_current_time_in_ms();
            bool double_click = i == list.last_click_row && now - list.last_click < 400;
            list.last_click = double_click ? 0 : now;
            list.last_click_row = i;
            on_click(root, i, double_click);
        }
        focus_field(root);
        redraw(root);
    };
}

// ---- Actions ----

// Shows a line in the footer for `ms` milliseconds, or until replaced when ms is 0.
static void show_notice(Container *root, std::string notice, long ms, bool error = false) {
    auto data = browser(root);
    data->notice = std::move(notice);
    data->notice_error = error;
    data->notice_until = ms > 0 ? get_current_time_in_ms() + ms : LONG_MAX;
    redraw(root);
    if (ms <= 0)
        return;
    auto app = data->app;
    auto window = browser_window(root);
    std::weak_ptr<bool> lifetime = root->lifetime;
    windowing::timer(app, ms + 100, [app, window, lifetime](void *) {
        std::lock_guard<std::mutex> lock(app->mutex);
        if (!lifetime.expired() && windowing::has_window(window))
            windowing::redraw(window);
    }, nullptr);
}

static bool notice_showing(BrowserData *data) {
    return !data->notice.empty() && get_current_time_in_ms() < data->notice_until;
}

static void copy_text(Container *root, const std::string &value) {
    if (value.empty() || !windowing::set_clipboard(browser_window(root), value))
        return;
    show_notice(root, "Copied “" + value + "” to the clipboard", 1500);
}

static void select_category(BrowserData *data, int category) {
    if (category < 0 || category >= (int) data->shown_categories.size())
        return;
    data->category = category;
    data->category_list.selected = category;
    reset_list(data->icon_list);
}

static void move_selection(Container *root, int delta) {
    auto data = browser(root);
    int count = row_count(data);
    if (count == 0)
        return;
    auto &list = data->icon_list;
    list.selected = list.selected < 0 ? (delta > 0 ? 0 : count - 1) : std::clamp(list.selected + delta, 0, count - 1);
    scroll_to_row(list, list.selected, row_h * browser_window(root)->dpi);
}

static void set_query(Container *root, std::string query) {
    auto data = browser(root);
    data->query = std::move(query);
    run_search(data);
    redraw(root);
}

// ---- Window contents ----

static void make_search_field(Container *root, Container *parent) {
    auto field = parent->child(FILL_SPACE, FILL_SPACE);
    field->name = "icon_browser_search";
    STextEditorOptions options;
    options.multiline = false;
    options.font_size = 11;
    options.window = [](Container *root) { return browser_window(root); };
    options.viewport = [](Container *root, Container *c) {
        auto dpi = browser_window(root)->dpi;
        auto bounds = c->real_bounds;
        bounds.x += 38 * dpi;
        bounds.w = std::max(0.0, bounds.w - 38 * dpi - 11 * dpi);
        return bounds;
    };
    options.on_change = [root](std::string text) { set_query(root, std::move(text)); };
    setup_text_editor(field, "", std::move(options));
    field->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = browser_window(root)->dpi;
        auto bounds = measure_text_editor(root, c);
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = std::round(bounds.h + 11 * dpi * 2 * .8);
    };
    field->when_paint = [](Container *root, Container *c) {
        auto window = browser_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        set_argb(cr, {1, 1, 1, 1});
        rounded_rect(cr, 10 * dpi, b.x, b.y, b.w, b.h);
        cr->fill();
        set_argb(cr, c->active ? accent_color : RGBA(0, 0, 0, .2));
        cr->set_line_width(std::max(1.0, (double) std::round(dpi)));
        rounded_rect(cr, 10 * dpi, b.x + .5, b.y + .5, b.w - 1, b.h - 1);
        cr->stroke();

        const std::string glyph = ""; // Search
        RGBA glyph_color(0, 0, 0, c->active ? .9 : .5);
        auto gb = text(cr, 0, 0, glyph, 13 * dpi, glyph_color, false, false, -1, icon_font);
        text(cr, b.x + 14 * dpi, b.y + (b.h - gb.h) * .5, glyph, 13 * dpi, glyph_color, false, true, -1, icon_font);

        if (static_cast<STextEditorData *>(c->user_data)->text.empty()) {
            const std::string prompt = "Search icons in every theme";
            auto pb = text(cr, 0, 0, prompt, 11 * dpi, {0, 0, 0, .4}, false, false);
            text(cr, b.x + 38 * dpi, b.y + (b.h - pb.h) * .5, prompt, 11 * dpi, {0, 0, 0, .4}, false, true, b.w - 50 * dpi);
        }
        paint_text_editor(root, c, RGBA(0, 0, 0, 1), RGBA(.2, .5, .8, .35));
    };
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods,
                                       bool is_text, std::string text) {
        if (!c->active)
            return;
        auto data = browser(root);
        if (pressed) {
            int page = std::max(1, (int) (data->icon_list.view_h / (row_h * browser_window(root)->dpi)) - 1);
            if (sym == XKB_KEY_Escape) {
                if (!data->query.empty()) {
                    reset_text_editor(c, "");
                    set_query(root, "");
                } else {
                    windowing::close_window(browser_window(root));
                }
                return;
            } else if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
                if ((mods & Modifier::MOD_CTRL) && !searching(data)) {
                    select_category(data, data->category + (sym == XKB_KEY_Up ? -1 : 1));
                    scroll_to_row(data->category_list, data->category, category_row_h * browser_window(root)->dpi);
                } else {
                    move_selection(root, sym == XKB_KEY_Up ? -1 : 1);
                }
                redraw(root);
                return;
            } else if (sym == XKB_KEY_Page_Up || sym == XKB_KEY_Page_Down) {
                move_selection(root, sym == XKB_KEY_Page_Up ? -page : page);
                redraw(root);
                return;
            } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                if (auto icon = row_icon(data, data->icon_list.selected))
                    copy_text(root, icon->name);
                redraw(root);
                return;
            }
        }
        edit_key(root, c, key, pressed, sym, mods, is_text, text);
    };
    browser(root)->field = field;
}

static void make_category_list(Container *body) {
    auto list = body->child(FILL_SPACE, FILL_SPACE);
    list->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto data = browser(root);
        c->exists = !searching(data);
        c->wanted_bounds.w = std::round(200 * browser_window(root)->dpi);
    };
    list->when_paint = [](Container *root, Container *c) {
        auto data = browser(root);
        auto dpi = browser_window(root)->dpi;
        auto cr = browser_window(root)->drawing_context;
        paint_list(root, c, data->category_list, data->shown_categories.size(), category_row_h * dpi, [&](int i, Bounds rb) {
            int category = data->shown_categories[i];
            auto count = std::to_string(data->by_category[category].size());
            auto cb = text(cr, 0, 0, count, 10 * dpi, {0, 0, 0, .45}, false, false);
            double count_x = rb.x + rb.w - 10 * dpi - cb.w;
            text(cr, count_x, rb.y + (rb.h - cb.h) * .5, count, 10 * dpi, {0, 0, 0, .45});
            auto label = categories[category].label;
            auto lb = text(cr, 0, 0, label, 11 * dpi, {0, 0, 0, .9}, i == data->category, false);
            text(cr, rb.x + 10 * dpi, rb.y + (rb.h - lb.h) * .5, label, 11 * dpi, {0, 0, 0, .9}, i == data->category, true,
                 count_x - rb.x - 18 * dpi);
        });
    };
    setup_list(list, &BrowserData::category_list, [](BrowserData *data) { return (int) data->shown_categories.size(); },
               [](double dpi) { return category_row_h * dpi; },
               [](Container *root, int i, bool) { select_category(browser(root), i); });
}

static void make_icon_list(Container *body) {
    auto list = body->child(FILL_SPACE, FILL_SPACE);
    list->when_paint = [](Container *root, Container *c) {
        auto data = browser(root);
        auto window = browser_window(root);
        auto dpi = window->dpi;
        auto cr = window->drawing_context;
        int icon_size = std::round(row_icon_size * dpi);
        paint_list(root, c, data->icon_list, row_count(data), row_h * dpi, [&](int i, Bounds rb) {
            auto icon = row_icon(data, i);
            if (!icon)
                return;
            double icon_x = std::round(rb.x + 8 * dpi);
            double icon_y = std::round(rb.y + rb.h * .5 - icon_size * .5);
            if (auto surface = icon_surface(data, icon->path, icon_size)) {
                cr->set_source_surface(surface, icon_x, icon_y);
                cr->paint_source();
            }
            std::string theme = icon->theme.empty() ? "unthemed" : icon->theme;
            auto tb = text(cr, 0, 0, theme, 10 * dpi, {0, 0, 0, .5}, false, false);
            double theme_w = std::min(tb.w, rb.w * .4);
            double theme_x = rb.x + rb.w - 10 * dpi - theme_w;
            text(cr, theme_x, rb.y + (rb.h - tb.h) * .5, theme, 10 * dpi, {0, 0, 0, .5}, false, true, theme_w + 1);
            double name_x = icon_x + icon_size + 10 * dpi;
            auto nb = text(cr, 0, 0, icon->name, 11 * dpi, {0, 0, 0, .9}, false, false);
            text(cr, name_x, rb.y + (rb.h - nb.h) * .5, icon->name, 11 * dpi, {0, 0, 0, .9}, false, true,
                 std::max(1.0, theme_x - 12 * dpi - name_x));
        });

        // Says why the list is empty.
        std::string message;
        if (!data->index)
            message = "Loading icons…";
        else if (searching(data) && data->results.empty())
            message = "No icons match “" + data->query + "”";
        else if (data->index->variants.empty())
            message = "No icon themes found";
        if (!message.empty()) {
            auto b = c->real_bounds;
            auto mb = text(cr, 0, 0, message, 11 * dpi, {0, 0, 0, .5}, false, false);
            text(cr, b.x + (b.w - mb.w) * .5, b.y + (b.h - mb.h) * .5, message, 11 * dpi, {0, 0, 0, .5}, false, true, b.w - 20 * dpi);
        }
    };
    setup_list(list, &BrowserData::icon_list, [](BrowserData *data) { return row_count(data); },
               [](double dpi) { return row_h * dpi; },
               [](Container *root, int i, bool double_click) {
                   auto data = browser(root);
                   data->icon_list.selected = i;
                   if (double_click)
                       if (auto icon = row_icon(data, i))
                           copy_text(root, icon->name);
               });
}

// ---- Add icon ----

/*
 * The icons "Add icon" installed, one path per line in ~/.config/mylar/added_icons.txt.
 * Only these get a "Remove icon" button: other programs install icons in the same folder.
 */
static std::mutex added_icons_mutex;

static std::filesystem::path added_icons_path() {
    const char *home = getenv("HOME");
    return std::filesystem::path(home ? home : "") / ".config/mylar/added_icons.txt";
}

// Read once, then kept in step with the file by added_icons_write_locked.
static std::vector<std::string> added_icons;
static bool added_icons_loaded = false;

static std::vector<std::string> added_icons_read_locked() {
    if (!added_icons_loaded) {
        added_icons_loaded = true;
        std::ifstream in(added_icons_path());
        std::string line;
        while (std::getline(in, line))
            if (!line.empty())
                added_icons.push_back(line);
    }
    return added_icons;
}

static void added_icons_write_locked(const std::vector<std::string> &paths) {
    added_icons = paths;
    auto path = added_icons_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    for (auto &p : paths)
        out << p << "\n";
}

static void added_icons_add(const std::string &icon_path) {
    std::lock_guard<std::mutex> lock(added_icons_mutex);
    auto paths = added_icons_read_locked();
    if (std::find(paths.begin(), paths.end(), icon_path) == paths.end())
        paths.push_back(icon_path);
    added_icons_write_locked(paths);
}

static void added_icons_remove(const std::string &icon_path) {
    std::lock_guard<std::mutex> lock(added_icons_mutex);
    auto paths = added_icons_read_locked();
    std::erase(paths, icon_path);
    added_icons_write_locked(paths);
}

static bool added_icons_contains(const std::string &icon_path) {
    std::lock_guard<std::mutex> lock(added_icons_mutex);
    added_icons_read_locked();
    return std::find(added_icons.begin(), added_icons.end(), icon_path) != added_icons.end();
}

// GTK programs read the folder's icon-theme.cache, so keep it current when there is one.
static void update_gtk_icon_cache() {
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;
    auto hicolor = std::filesystem::path(home) / ".local/share/icons/hicolor";
    std::error_code ec;
    if (!std::filesystem::exists(hicolor / "icon-theme.cache", ec))
        return;
    std::string quoted = "'";
    for (char ch : hicolor.string())
        quoted += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    quoted += "'";
    std::string command = "command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -f -t " + quoted + " >/dev/null 2>&1";
    if (std::system(command.c_str()) != 0) {
        // Not fatal: GTK notices the stale cache from the folder times and scans instead.
    }
}

// Fixed sizes of the hicolor theme; a raster image goes in the largest one it fills.
static const int hicolor_sizes[] = {16, 22, 24, 32, 48, 64, 96, 128, 256, 512};

// A name no icon has yet, so the new one doesn't hide (or hide behind) another.
static std::string free_icon_name(std::string name, const std::filesystem::path &dir, const std::string &extension) {
    for (auto &ch : name)
        if (ch == ' ' || ch == '/')
            ch = '-';
    if (name.empty())
        name = "icon";
    std::string candidate = name;
    std::error_code ec;
    for (int i = 2; has_options(candidate) || std::filesystem::exists(dir / (candidate + extension), ec); i++)
        candidate = name + "-" + std::to_string(i);
    return candidate;
}

// Copies the image into ~/.local/share/icons/hicolor: an SVG as it is, anything else
// gdk-pixbuf reads as a square PNG of a fixed size. Returns the icon name, or "" with error set.
static std::string install_icon(const std::string &source, std::string &error) {
    namespace fs = std::filesystem;
    const char *home = getenv("HOME");
    if (!home || !*home) {
        error = "HOME is not set";
        return "";
    }
    fs::path hicolor = fs::path(home) / ".local/share/icons/hicolor";
    auto extension = to_lower(fs::path(source).extension().string());
    auto stem = fs::path(source).stem().string();
    std::error_code ec;

    if (extension == ".svg") {
        auto dir = hicolor / "scalable/apps";
        fs::create_directories(dir, ec);
        auto name = free_icon_name(stem, dir, ".svg");
        if (ec || !fs::copy_file(source, dir / (name + ".svg"), fs::copy_options::overwrite_existing, ec) || ec) {
            error = "Couldn't copy the icon: " + ec.message();
            return "";
        }
        added_icons_add((dir / (name + ".svg")).string());
        return name;
    }

    GError *gerror = nullptr;
    GdkPixbuf *image = gdk_pixbuf_new_from_file(source.c_str(), &gerror);
    if (!image) {
        error = gerror ? gerror->message : "Not an image";
        g_clear_error(&gerror);
        return "";
    }
    int w = gdk_pixbuf_get_width(image);
    int h = gdk_pixbuf_get_height(image);
    int longest = std::max(1, std::max(w, h));
    int size = hicolor_sizes[0];
    for (int s : hicolor_sizes)
        if (s <= longest)
            size = s;
    // Fit the image in the square, centered, keeping its aspect ratio.
    double scale = (double) size / longest;
    int sw = std::max(1, (int) std::round(w * scale));
    int sh = std::max(1, (int) std::round(h * scale));
    int x = (size - sw) / 2;
    int y = (size - sh) / 2;
    GdkPixbuf *square = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size, size);
    gdk_pixbuf_fill(square, 0x00000000);
    gdk_pixbuf_composite(image, square, x, y, sw, sh, x, y, scale, scale, GDK_INTERP_HYPER, 255);
    g_object_unref(image);

    auto dir = hicolor / (std::to_string(size) + "x" + std::to_string(size)) / "apps";
    fs::create_directories(dir, ec);
    auto name = free_icon_name(stem, dir, ".png");
    auto destination = (dir / (name + ".png")).string();
    bool saved = !ec && gdk_pixbuf_save(square, destination.c_str(), "png", &gerror, nullptr);
    g_object_unref(square);
    if (!saved) {
        error = "Couldn't save the icon: " + std::string(gerror ? gerror->message : ec.message());
        g_clear_error(&gerror);
        return "";
    }
    added_icons_add(destination);
    return name;
}

// Waits on the add-icon thread, then shows the new icon by searching for it.
static void watch_add_icon(Container *root) {
    auto data = browser(root);
    auto app = data->app;
    auto job = data->add_job;
    std::weak_ptr<bool> lifetime = root->lifetime;
    windowing::timer(app, 100, [app, root, job, lifetime](void *) {
        {
            std::lock_guard<std::mutex> lock(app->mutex);
            if (lifetime.expired())
                return;
            auto data = browser(root);
            if (data->add_job != job)
                return;
            if (!job->done) {
                watch_add_icon(root);
                return;
            }
            data->add_job = nullptr;
            if (!job->error.empty()) {
                show_notice(root, job->error, 5000, true);
            } else if (!job->removed.empty()) {
                if (job->index) {
                    data->job = nullptr;
                    index_ready(data, job->index);
                }
                show_notice(root, "Removed “" + job->removed + "” from the local icons", 5000);
            } else if (job->name.empty()) {
                data->notice_until = 0; // cancelled
            } else {
                if (job->index) {
                    // The index built when the window opened would no longer have the new icon.
                    data->job = nullptr;
                    index_ready(data, job->index);
                }
                if (data->field)
                    reset_text_editor(data->field, job->name);
                data->query = job->name;
                run_search(data);
                if (auto icon = row_icon(data, data->icon_list.selected))
                    show_notice(root, "Added “" + job->name + "” to " + icon->path, 5000);
                else
                    show_notice(root, "Added “" + job->name + "”", 5000);
            }
        }
        windowing::redraw(browser_window(root));
    }, nullptr);
}

// Picks an image with the same file chooser as the wallpaper setting, puts it in the
// local icons, regenerates the icon cache and shows the new icon.
static void add_icon(Container *root) {
    auto data = browser(root);
    if (data->add_job)
        return;
    // One GTK file chooser at a time.
    static std::atomic<bool> chooser_open = false;
    if (chooser_open.exchange(true))
        return;
    data->add_job = std::make_shared<AddIconJob>();
    show_notice(root, "Choose an image to add to the local icons…", 0);
    std::thread([job = data->add_job]() {
        auto path = choose_file();
        chooser_open = false;
        path.erase(std::remove_if(path.begin(), path.end(), [](unsigned char c) { return c == '\n' || c == '\r'; }), path.end());
        if (!path.empty()) {
            job->name = install_icon(path, job->error);
            if (!job->name.empty()) {
                update_gtk_icon_cache();
                icon_cache_generate();
                icon_cache_load();
                job->index = build_index();
                std::lock_guard<std::mutex> cache_lock(last_index_mutex);
                last_index = job->index;
            }
        }
        job->done = true;
    }).detach();
    watch_add_icon(root);
}

// Only icons "Add icon" installed can be removed here.
static bool is_local_icon(const BrowserIcon *icon) {
    return icon && added_icons_contains(icon->path);
}

static bool remove_armed(BrowserData *data) {
    auto icon = row_icon(data, data->icon_list.selected);
    return icon && icon->path == data->remove_armed_path && get_current_time_in_ms() < data->remove_armed_until;
}

// The first click asks for a second one; the second deletes the file and regenerates the icon cache.
static void remove_icon(Container *root) {
    auto data = browser(root);
    auto icon = row_icon(data, data->icon_list.selected);
    if (!is_local_icon(icon) || data->add_job)
        return;
    if (!remove_armed(data)) {
        data->remove_armed_path = icon->path;
        data->remove_armed_until = get_current_time_in_ms() + 3000;
        show_notice(root, "Click again to delete " + icon->path, 3000, true);
        return;
    }
    data->remove_armed_path.clear();
    data->remove_armed_until = 0;
    std::error_code ec;
    std::filesystem::remove(icon->path, ec);
    if (ec) {
        show_notice(root, "Couldn't remove " + icon->path + ": " + ec.message(), 5000, true);
        return;
    }
    added_icons_remove(icon->path);
    data->add_job = std::make_shared<AddIconJob>();
    data->add_job->removed = icon->name;
    show_notice(root, "Removing “" + icon->name + "”…", 0);
    std::thread([job = data->add_job]() {
        update_gtk_icon_cache();
        icon_cache_generate();
        icon_cache_load();
        job->index = build_index();
        {
            std::lock_guard<std::mutex> cache_lock(last_index_mutex);
            last_index = job->index;
        }
        job->done = true;
    }).detach();
    watch_add_icon(root);
}

static void footer_button(Container *parent, std::function<std::string(BrowserData *)> label_of,
                          std::function<bool(BrowserData *)> enabled, std::function<void(Container *)> on_click,
                          std::function<bool(BrowserData *)> visible = nullptr) {
    auto button = parent->child(FILL_SPACE, FILL_SPACE);
    button->pre_layout = [label_of, visible](Container *root, Container *c, const Bounds &) {
        auto window = browser_window(root);
        c->exists = !visible || visible(browser(root));
        auto label = label_of(browser(root));
        auto b = text(window->drawing_context, 0, 0, label, 11 * window->dpi, {0, 0, 0, 1}, false, false);
        c->wanted_bounds.w = std::round(b.w + 28 * window->dpi);
        c->wanted_bounds.h = FILL_SPACE;
    };
    button->when_paint = [label_of, enabled](Container *root, Container *c) {
        auto data = browser(root);
        auto label = label_of(data);
        auto window = browser_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        bool on = enabled(data);
        RGBA bg = !on ? RGBA(0, 0, 0, .04) : c->state.mouse_pressing ? RGBA(0, 0, 0, .16) : c->state.mouse_hovering ? RGBA(0, 0, 0, .1) : RGBA(0, 0, 0, .06);
        set_argb(cr, bg);
        rounded_rect(cr, 6 * dpi, b.x, b.y, b.w, b.h);
        cr->fill();
        RGBA color(0, 0, 0, on ? .85 : .35);
        auto tb = text(cr, 0, 0, label, 11 * dpi, color, false, false);
        text(cr, b.x + (b.w - tb.w) * .5, b.y + (b.h - tb.h) * .5, label, 11 * dpi, color);
    };
    button->when_clicked = [enabled, on_click](Container *root, Container *c) {
        if (enabled(browser(root)))
            on_click(root);
        focus_field(root);
        redraw(root);
    };
}

// The selected icon, larger, with its name, theme, category and file.
static void make_footer(Container *parent) {
    auto footer = parent->child(::hbox, FILL_SPACE, FILL_SPACE);
    footer->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = browser_window(root)->dpi;
        c->wanted_bounds.h = std::round(footer_h * dpi);
        c->spacing = 8 * dpi;
        // Centers the 30px tall buttons.
        double pad = std::round((footer_h - 30) * .5 * dpi);
        c->wanted_pad = Bounds(12 * dpi, pad, 12 * dpi, pad);
    };
    footer->when_paint = [](Container *root, Container *c) {
        auto data = browser(root);
        auto window = browser_window(root);
        auto cr = window->drawing_context;
        auto dpi = window->dpi;
        auto b = c->real_bounds;
        set_argb(cr, {1, 1, 1, 1});
        rounded_rect(cr, 8 * dpi, b.x, b.y, b.w, b.h);
        cr->fill();
        set_argb(cr, {0, 0, 0, .14});
        cr->set_line_width(std::max(1.0, (double) std::floor(dpi)));
        rounded_rect(cr, 8 * dpi, b.x + .5, b.y + .5, b.w - 1, b.h - 1);
        cr->stroke();

        // The buttons are the footer's children, at its right end.
        double buttons_x = b.x + b.w;
        for (auto child : c->children)
            if (child->exists && child->wanted_bounds.w > 0)
                buttons_x = std::min(buttons_x, child->real_bounds.x);
        double text_w = std::max(1.0, buttons_x - b.x - 12 * dpi);

        bool notice = notice_showing(data);
        RGBA notice_color = data->notice_error ? RGBA(.8, .15, .1, 1) : RGBA(.1, .45, .85, 1);
        auto icon = row_icon(data, data->icon_list.selected);
        if (!icon) {
            std::string hint = notice ? data->notice : "Select an icon to see its details. Double-click or press Enter to copy its name.";
            RGBA color = notice ? notice_color : RGBA(0, 0, 0, .5);
            auto hb = text(cr, 0, 0, hint, 10 * dpi, color, false, false);
            text(cr, b.x + 16 * dpi, b.y + (b.h - hb.h) * .5, hint, 10 * dpi, color, false, true, text_w - 16 * dpi);
            return;
        }
        int size = std::round(preview_icon_size * dpi);
        if (data->preview_path != icon->path || data->preview_size != size) {
            if (data->preview)
                cairo_surface_destroy(data->preview);
            data->preview = nullptr;
            data->preview_path = icon->path;
            data->preview_size = size;
            if (!icon->path.empty())
                load_icon_full_path(&data->preview, icon->path, size);
        }
        double x = b.x + 14 * dpi;
        if (data->preview) {
            cr->set_source_surface(data->preview, std::round(x), std::round(b.y + (b.h - size) * .5));
            cr->paint_source();
        }
        x += size + 14 * dpi;
        double w = std::max(1.0, buttons_x - x - 12 * dpi);

        auto nb = text(cr, 0, 0, icon->name, 12 * dpi, {0, 0, 0, .9}, true, false);
        std::string details = (icon->theme.empty() ? "unthemed" : icon->theme) + "  ·  " + context_label(icon->context);
        if (notice)
            details = data->notice;
        auto db = text(cr, 0, 0, details, 10 * dpi, {0, 0, 0, .55}, false, false);
        auto pb = text(cr, 0, 0, icon->path, 9 * dpi, {0, 0, 0, .45}, false, false);
        double gap = 2 * dpi;
        double y = b.y + (b.h - nb.h - db.h - pb.h - gap * 2) * .5;
        text(cr, x, y, icon->name, 12 * dpi, {0, 0, 0, .9}, true, true, w);
        y += nb.h + gap;
        text(cr, x, y, details, 10 * dpi, notice ? notice_color : RGBA(0, 0, 0, .55), false, true, w);
        y += db.h + gap;
        text(cr, x, y, icon->path, 9 * dpi, {0, 0, 0, .45}, false, true, w);
    };
    // Pushes the buttons to the right end.
    auto spacer = footer->child(FILL_SPACE, FILL_SPACE);
    spacer->pre_layout = [](Container *, Container *c, const Bounds &) { c->wanted_bounds.w = FILL_SPACE; };
    footer_button(footer, [](BrowserData *) { return std::string("Add icon…"); }, [](BrowserData *data) { return !data->add_job; }, add_icon);
    footer_button(footer, [](BrowserData *data) {
                      return std::string(remove_armed(data) ? "Click again to remove" : "Remove icon");
                  },
                  [](BrowserData *data) { return !data->add_job; }, remove_icon,
                  [](BrowserData *data) { return is_local_icon(row_icon(data, data->icon_list.selected)); });
    footer_button(footer, [](BrowserData *) { return std::string("Copy name"); }, [](BrowserData *data) { return row_icon(data, data->icon_list.selected) != nullptr; },
                  [](Container *root) {
                      auto data = browser(root);
                      if (auto icon = row_icon(data, data->icon_list.selected))
                          copy_text(root, icon->name);
                  });
    footer_button(footer, [](BrowserData *) { return std::string("Copy path"); }, [](BrowserData *data) { return row_icon(data, data->icon_list.selected) != nullptr; },
                  [](Container *root) {
                      auto data = browser(root);
                      if (auto icon = row_icon(data, data->icon_list.selected))
                          copy_text(root, icon->path);
                  });
}

static void fill_root(Container *root) {
    root->type = ::vbox;
    root->when_paint = [](Container *root, Container *c) {
        auto cr = browser_window(root)->drawing_context;
        cr->save();
        cr->set_operator(CAIRO_OPERATOR_CLEAR);
        cr->paint_source();
        cr->restore();
        set_rect(cr, c->real_bounds);
        set_argb(cr, RGBA(.941, .957, .976, 1));
        cr->fill();
    };
    root->when_key_event = [](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym, int mods, bool is_text,
                              std::string text) {
        auto data = browser(root);
        if (!pressed || !data->field || data->field->active)
            return;
        if (sym == XKB_KEY_Escape)
            windowing::close_window(browser_window(root));
        else
            focus_field(root);
    };
    auto content = root->child(::vbox, FILL_SPACE, FILL_SPACE);
    content->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = browser_window(root)->dpi;
        c->wanted_pad = Bounds(16 * dpi, 16 * dpi, 16 * dpi, 16 * dpi);
        c->spacing = 10 * dpi;
    };
    make_search_field(root, content);
    auto body = content->child(::hbox, FILL_SPACE, FILL_SPACE);
    body->pre_layout = [](Container *root, Container *c, const Bounds &) { c->spacing = 10 * browser_window(root)->dpi; };
    make_category_list(body);
    make_icon_list(body);
    make_footer(content);
    set_active(root, {browser(root)->field}, root, true, false);
}

static void start_icon_browser() {
    auto app = windowing::open_app();
    RawWindowSettings settings;
    settings.pos.w = 900;
    settings.pos.h = 640;
    settings.name = "Icon Browser";
    settings.app_id = "preferences-desktop-icons";
    auto mylar = open_mylar_window(app, WindowType::NORMAL, settings);
    auto data = new BrowserData;
    data->app = app;
    data->window = mylar;
    mylar->root->user_data = data;

    {
        std::lock_guard<std::mutex> lock(app->mutex);
        fill_root(mylar->root);
        data->job = std::make_shared<IndexJob>();
        std::shared_ptr<BrowserIndex> cached;
        {
            std::lock_guard<std::mutex> cache_lock(last_index_mutex);
            cached = last_index;
        }
        if (cached && !cached->variants.empty())
            index_ready(data, cached);
        std::thread([job = data->job]() {
            job->result = build_index();
            {
                std::lock_guard<std::mutex> cache_lock(last_index_mutex);
                last_index = job->result;
            }
            job->done = true;
        }).detach();
        watch_index(data);
    }

    windowing::main_loop(app);

    mylar->root->user_data = nullptr;
    delete data;
    cleanup_cached_pango_fonts();
    drawing::cleanup_thread_resources();
}

void icon_browser::open() {
    std::thread(start_icon_browser).detach();
}
