#include "dock/dock_extra.h"

#include "dock/dock_internal.h"

#include "dock/dock_battery.h"
#include "dock/dock_bluetooth.h"
#include "dock/dock_volume.h"
#include "settings.h"
#include "systray.h"

#include <cstdlib>
#include <linux/input-event-codes.h>
#include <set>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// hyprsunset outlives the plugin (it keeps running across "hyprctl plugin
// reload"), so read whether it is running instead of assuming it is off.
static bool hyprsunset_running() {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/proc", ec)) {
        const auto name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit))
            continue;
        std::ifstream comm(entry.path() / "comm");
        std::string cmd;
        if (std::getline(comm, cmd) && cmd == "hyprsunset")
            return true;
    }
    return false;
}

bool nightlight_on = hyprsunset_running();

// The dock's copy of set->nightlight_temperature, so the slider updates
// immediately while the setting itself is only written on the main thread.
static int nightlight_kelvin = -1;

int nightlight_temperature() {
    if (nightlight_kelvin < 0)
        nightlight_kelvin = std::clamp(set->nightlight_temperature, nightlight_min_kelvin, nightlight_max_kelvin);
    return nightlight_kelvin;
}

// Sends one command to hyprsunset's IPC socket (what "hyprctl hyprsunset"
// does), so dragging the slider doesn't fork a process per step.
static void hyprsunset_request(const std::string &request) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !signature)
        return;
    std::string path = std::string(runtime) + "/hypr/" + signature + "/.hyprsunset.sock";
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
        return;
    strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    timeval timeout = {0, 200 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (sockaddr *) &addr, sizeof(addr)) == 0 &&
        send(fd, request.data(), request.size(), MSG_NOSIGNAL) == (ssize_t) request.size()) {
        // Wait for the reply so hyprsunset doesn't write into a closed socket.
        char reply[256];
        (void) !recv(fd, reply, sizeof(reply), 0);
    }
    close(fd);
}

void set_nightlight_temperature(int kelvin) {
    kelvin = std::clamp((int) std::round(kelvin / 100.0) * 100, nightlight_min_kelvin, nightlight_max_kelvin);
    if (kelvin == nightlight_temperature())
        return;
    nightlight_kelvin = kelvin;
    if (nightlight_on)
        hyprsunset_request("temperature " + std::to_string(kelvin));
    main_thread([kelvin]() { set->nightlight_temperature = kelvin; });
    // Saving rewrites the whole settings file, so wait until the slider settles.
    debounce_main("nightlight_temperature", 300, []() { settings::load_save_settings(true, set); });
}

void make_nightlight_slider(Container *parent, MylarWindow *(*get_window)(Dock *)) {
    make_self_sizing_slider(parent,
        [](Container *) { return std::string("\uE708"); },
        [](Container *) { return std::to_string(nightlight_temperature()) + "K"; },
        [get_window](Container *c, float amount) {
            Container *root = c;
            while (root->parent) root = root->parent;
            amount = std::clamp(amount, 0.0f, 1.0f);
            set_nightlight_temperature(nightlight_max_kelvin - amount * (nightlight_max_kelvin - nightlight_min_kelvin));
            windowing::redraw(get_window((Dock *) root->user_data)->raw_window);
        },
        [](Container *) {
            return (float) (nightlight_max_kelvin - nightlight_temperature()) / (nightlight_max_kelvin - nightlight_min_kelvin);
        },
        get_window);
}

void toggle_nightlight() {
    // Never wait on hyprsunset from a plugin thread: a thread parked in
    // system() until hyprsunset exits returns into unmapped code if the plugin
    // was unloaded in the meantime, crashing the compositor on the next toggle.
    if (nightlight_on) {
        launch_command("killall hyprsunset");
    } else {
        launch_command("exec hyprsunset -t " + std::to_string(nightlight_temperature()));
    }
    nightlight_on = !nightlight_on;
}

// State handed to the short-lived timer that reopens a popup after the extra
// menu window has closed.
struct Reformed {
    MylarWindow *window = nullptr;
    Dock *dock = nullptr;
    float dpi = 1.0;
};

static MylarWindow *extra_window(Dock *dock) {
    return dock->extra;
}

static float extra_dpi(Container *root) {
    return extra_window((Dock *) root->user_data)->raw_window->dpi;
}

static drawing::Context *extra_cr(Container *root) {
    return extra_window((Dock *) root->user_data)->raw_window->drawing_context;
}

static void extra_redraw(Container *root) {
    windowing::redraw(extra_window((Dock *) root->user_data)->raw_window);
}

static void close_extra(Dock *dock) {
    auto window = extra_window(dock);
    if (!window)
        return;
    windowing::close_window(window->raw_window);
    if (dock->window->popup_window == window)
        dock->window->popup_window = nullptr;
}

static void open_bluetooth_popup(Dock *dock) {
    close_extra(dock);

    auto mylar = dock->window;
    auto dpi = mylar->raw_window->dpi;

    auto reformed = new Reformed;
    reformed->dpi = dpi;
    reformed->window = mylar;
    reformed->dock = dock;

    windowing::timer(dock->app, 40, [](void *data) {
        auto r = std::unique_ptr<Reformed>(static_cast<Reformed *>(data));
        auto mylar = r->window;
        auto dock = r->dock;
        std::lock_guard<std::mutex> lock(dock->app->mutex);
        if (finished || !bluetooth_available.load() || mylar->popup_window || !windowing::has_window(mylar->raw_window))
            return;
        auto dpi = r->dpi;
        auto c = container_by_name("extra", dock->window->root);

        RawWindowSettings settings = make_icon_anchored_popup_settings(c, dpi, 376, 448);

        dock->bluetooth = open_mylar_popup(mylar, settings);
        if (!dock->bluetooth)
            return;
        dock->bluetooth->root->user_data = dock;
        dock->bluetooth->root->wanted_bounds.w = FILL_SPACE;
        dock->bluetooth->root->wanted_bounds.h = FILL_SPACE;
        fill_bluetooth_container(dock);
        windowing::redraw(dock->bluetooth->raw_window);
    }, reformed);
}

/*************************************************
 *
 * Pages
 *
 * The popup shows one page at a time. Going into a page (or back out) slides
 * and fades between them the same way the Settings window does: the page being
 * left is kept, already laid out, and painted into a layer that slides away
 * while the new page slides in from the other side.
 *
 *************************************************/

struct ProjectionTarget {
    std::string name;
    int mode = MONITOR_PROJECTION_EXTEND;
};

struct ExtraMenu;

// Menus that still exist, so replies from the main thread can tell whether
// theirs is gone. Docks run on their own threads, hence the mutex.
static std::mutex extra_menus_mutex;
static std::set<ExtraMenu *> extra_menus_alive;

struct ExtraMenu : UserData {
    Dock *dock = nullptr;
    Container *pages = nullptr;    // Holds the current page as its only child
    std::string page = "main";
    bool closed = false;
    bool via_keyboard = false;

    // Slide between pages
    Container *outgoing = nullptr; // Holder of the old page's containers, or null
    int dir = 0;                   // 1 into a page, -1 back out, 0 idle
    long start = 0;                // Set on the first frame, so slow builds don't eat the animation
    float eased = 1;
    bool layer_open = false;

    // Project page
    std::string ours;              // The monitor this dock is on
    std::vector<ProjectionTarget> targets;
    bool targets_ready = false;
    int focus_row = -1;            // Keyboard selection on the Project page

    ExtraMenu() {
        std::lock_guard<std::mutex> lock(extra_menus_mutex);
        extra_menus_alive.insert(this);
    }
    ~ExtraMenu() override {
        {
            std::lock_guard<std::mutex> lock(extra_menus_mutex);
            extra_menus_alive.erase(this);
        }
        delete outgoing;
    }
};

static bool extra_menu_alive(ExtraMenu *menu) {
    std::lock_guard<std::mutex> lock(extra_menus_mutex);
    return extra_menus_alive.contains(menu) && !menu->closed;
}

static constexpr float extra_slide_ms = 260;
static constexpr float extra_slide_distance = 56;
static constexpr float extra_pad = 12;

struct ExtraPage {
    std::string id;
    int depth;                       // 0 for the main page, 1 for pages opened from it
    void (*fill)(Container *page, ExtraMenu *menu);
};

static void fill_main_page(Container *page, ExtraMenu *menu);
static void fill_project_page(Container *page, ExtraMenu *menu);

// Every page of the popup. A new page only needs an entry here and a tile
// (or anything else) that calls extra_show_page with its id.
static const std::vector<ExtraPage> extra_pages = {
    {"main", 0, fill_main_page},
    {"project", 1, fill_project_page},
};

static const ExtraPage *extra_page(const std::string &id) {
    for (auto &p : extra_pages)
        if (p.id == id)
            return &p;
    return nullptr;
}

static void extra_slide_finish(ExtraMenu *menu) {
    delete menu->outgoing;
    menu->outgoing = nullptr;
    menu->dir = 0;
    menu->start = 0;
}

static void extra_build_page(ExtraMenu *menu) {
    auto page = menu->pages->child(::vbox, FILL_SPACE, FILL_SPACE);
    if (auto p = extra_page(menu->page))
        p->fill(page, menu);
}

// Rebuilds the current page in place, without a slide.
static void extra_refill_page(ExtraMenu *menu) {
    for (auto child : menu->pages->children)
        delete child;
    menu->pages->children.clear();
    extra_build_page(menu);
}

static void extra_show_page(ExtraMenu *menu, const std::string &id) {
    auto from = extra_page(menu->page);
    auto to = extra_page(id);
    if (!to || id == menu->page)
        return;
    extra_slide_finish(menu);
    auto holder = new Container;
    holder->real_bounds = menu->pages->real_bounds;
    holder->children = std::move(menu->pages->children);
    menu->pages->children.clear();
    for (auto child : holder->children)
        child->parent = holder;
    menu->outgoing = holder;
    menu->dir = from ? (to->depth >= from->depth ? 1 : -1) : 1;
    menu->page = id;
    if (id == "project" && menu->targets_ready && menu->focus_row < 0 && menu->via_keyboard && !menu->targets.empty())
        menu->focus_row = 0;
    extra_build_page(menu);
    windowing::redraw(extra_window(menu->dock)->raw_window);
}

static void extra_pages_paint_before(Container *root, Container *c) {
    auto menu = (ExtraMenu *) c->user_data;
    if (menu->dir == 0)
        return;
    auto window = extra_window(menu->dock)->raw_window;
    auto cr = window->drawing_context;
    long now = get_current_time_in_ms();
    if (menu->start == 0)
        menu->start = now;
    float t = std::clamp((float) (now - menu->start) / extra_slide_ms, 0.0f, 1.0f);
    if (t >= 1) {
        extra_slide_finish(menu);
        return;
    }
    float e = 1 - (1 - t) * (1 - t) * (1 - t); // ease out
    menu->eased = e;
    double distance = extra_slide_distance * window->dpi * menu->dir;

    cr->save();
    clip_to_popup_background(cr, root->real_bounds, window->dpi);
    if (menu->outgoing) {
        cr->push_group();
        cr->save();
        cr->translate(-distance * e, 0);
        paint_outline(root, menu->outgoing);
        cr->restore();
        cr->pop_group_to_source();
        // Gone a bit before the new page arrives, so the two don't muddle.
        cr->paint_source(std::max(0.0f, 1 - e * 1.4f));
    }
    cr->push_group();
    cr->save();
    cr->translate(distance * (1 - e), 0);
    menu->layer_open = true;
    windowing::redraw(window);
}

static void extra_pages_paint_after(Container *root, Container *c) {
    auto menu = (ExtraMenu *) c->user_data;
    if (!menu->layer_open)
        return;
    menu->layer_open = false;
    auto cr = extra_cr(root);
    cr->restore();
    cr->pop_group_to_source();
    cr->paint_source(menu->eased);
    cr->restore();
}

/*************************************************
 *
 * Building blocks
 *
 *************************************************/

// Lays its children out left to right in equal columns, wrapping into as many
// rows as needed. Children that don't exist are skipped, so tiles can come and
// go (Bluetooth) without leaving holes.
static constexpr float tile_min_w = 100;
static constexpr float tile_h = 72;
static constexpr float tile_gap = 8;

static int flow_columns(double w, double min_w, double gap) {
    return std::max(1, (int) std::floor((w + gap) / (min_w + gap)));
}

static Container *make_flow_grid(Container *parent) {
    auto grid = parent->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    grid->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = extra_dpi(root);
        double gap = tile_gap * dpi;
        double th = tile_h * dpi;
        std::vector<Container *> shown;
        for (auto child : c->children)
            if (child->exists)
                shown.push_back(child);
        int cols = flow_columns(b.w, tile_min_w * dpi, gap);
        int rows = (int) std::ceil((double) shown.size() / cols);
        double w = (b.w - gap * (cols - 1)) / cols;
        c->wanted_bounds.h = rows > 0 ? rows * th + (rows - 1) * gap : 0;
        for (int i = 0; i < (int) shown.size(); i++) {
            int col = i % cols;
            int row = i / cols;
            ::layout(root, shown[i], Bounds(std::round(b.x + col * (w + gap)), std::round(b.y + row * (th + gap)), std::round(w), std::round(th)));
        }
    };
    return grid;
}

// One button in the main page's grid.
struct ExtraTile {
    std::string name;                           // Container name, so others can find it
    std::string icon;
    std::string label;
    std::function<bool()> visible;              // Null: always shown
    std::function<bool()> active;               // Null: never highlighted
    bool opens_page = false;                    // Draws a chevron
    std::function<void(Container *root, ExtraMenu *menu)> on_click;
};

static void paint_hover(drawing::Context *cr, Container *c, float dpi, float radius) {
    if (!c->state.mouse_hovering && !c->state.mouse_pressing)
        return;
    set_argb(cr, {0, 0, 0, c->state.mouse_pressing ? .14 : .07});
    drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h, radius * dpi, 1.0);
    cr->fill();
}

static void add_tile(Container *grid, ExtraMenu *menu, ExtraTile tile) {
    auto c = grid->child(FILL_SPACE, FILL_SPACE);
    c->name = tile.name;
    c->exists = !tile.visible || tile.visible();
    auto shared = std::make_shared<ExtraTile>(std::move(tile));
    c->when_clicked = [shared, menu](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        if (shared->on_click)
            shared->on_click(root, menu);
    };
    c->when_paint = [shared](Container *root, Container *c) {
        auto cr = extra_cr(root);
        auto dpi = extra_dpi(root);
        auto b = c->real_bounds;
        bool on = shared->active && shared->active();
        RGBA ink = on ? RGBA(1, 1, 1, 1) : RGBA(0, 0, 0, .85);

        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
        set_argb(cr, on ? accent : RGBA(0, 0, 0, .045));
        cr->fill();
        paint_hover(cr, c, dpi, 8);

        auto ib = draw_text(cr, 0, 0, shared->icon, 18 * dpi, false, icon_font, -1, -1, ink);
        auto lb = draw_text(cr, 0, 0, shared->label, 10 * dpi, false, set->font, -1, -1, ink);
        double gap = 6 * dpi;
        double top = b.y + (b.h - ib.h - gap - lb.h) * .5;
        draw_text(cr, center_x(c, ib.w), top, shared->icon, 18 * dpi, true, icon_font, -1, -1, ink);
        draw_text(cr, center_x(c, lb.w), top + ib.h + gap, shared->label, 10 * dpi, true, set->font, -1, -1, ink);

        if (shared->opens_page) {
            std::string chevron = "";
            auto cb = draw_text(cr, 0, 0, chevron, 9 * dpi, false, icon_font, -1, -1, ink);
            draw_text(cr, b.x + b.w - cb.w - 8 * dpi, b.y + 8 * dpi, chevron, 9 * dpi, true, icon_font, -1, -1, ink);
        }
    };
}

// Fixed height line of text.
static Container *add_text(Container *parent, std::string text, int size, RGBA color, float h, bool bold = false) {
    auto c = parent->child(FILL_SPACE, FILL_SPACE);
    c->pre_layout = [h](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = h * extra_dpi(root);
    };
    c->when_paint = [text, size, color, bold](Container *root, Container *c) {
        auto cr = extra_cr(root);
        auto dpi = extra_dpi(root);
        auto tb = draw_text(cr, 0, 0, text, size * dpi, false, set->font, -1, -1, color, bold);
        draw_text(cr, c->real_bounds.x, center_y(c, tb.h), text, size * dpi, true, set->font, -1, -1, color, bold);
    };
    return c;
}

static void extra_page_padding(Container *page) {
    page->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = extra_dpi(root);
        c->wanted_pad = Bounds(extra_pad * dpi, extra_pad * dpi, extra_pad * dpi, extra_pad * dpi);
        c->spacing = 10 * dpi;
    };
}

// Back button and title at the top of a sub page.
static void add_page_header(Container *page, ExtraMenu *menu, std::string title, std::string back_to) {
    auto header = page->child(::hbox, FILL_SPACE, FILL_SPACE);
    header->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = extra_dpi(root);
        c->wanted_bounds.h = 30 * dpi;
        c->spacing = 6 * dpi;
    };
    auto back = header->child(FILL_SPACE, FILL_SPACE);
    back->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 30 * extra_dpi(root);
    };
    back->when_paint = [](Container *root, Container *c) {
        auto cr = extra_cr(root);
        auto dpi = extra_dpi(root);
        paint_hover(cr, c, dpi, 6);
        std::string glyph = "";
        auto gb = draw_text(cr, 0, 0, glyph, 11 * dpi, false, icon_font, -1, -1, {0, 0, 0, .85});
        draw_text(cr, center_x(c, gb.w), center_y(c, gb.h), glyph, 11 * dpi, true, icon_font, -1, -1, {0, 0, 0, .85});
    };
    back->when_clicked = [menu, back_to](Container *root, Container *c) {
        extra_show_page(menu, back_to);
    };
    add_text(header, title, 12, {0, 0, 0, .9}, 30, true);
}

/*************************************************
 *
 * Main page
 *
 *************************************************/

static std::vector<ExtraTile> extra_tiles() {
    return {
        {"bluetooth", "", "Bluetooth", []() { return bluetooth_available.load(); }, nullptr, false,
         [](Container *, ExtraMenu *menu) { open_bluetooth_popup(menu->dock); }},
        // On laptops night light lives in the battery menu, next to brightness.
        {"night_light", "", "Night light", []() { return !has_internal_battery(); }, []() { return nightlight_on; }, false,
         [](Container *root, ExtraMenu *) {
             toggle_nightlight();
             extra_redraw(root);
         }},
        {"keyboard", "", "Keyboard", nullptr, nullptr, false,
         [](Container *, ExtraMenu *menu) {
             close_extra(menu->dock);
             main_thread([]() { settings::start("Keyboard"); });
         }},
        {"project", "", "Project", nullptr, nullptr, true,
         [](Container *, ExtraMenu *menu) { extra_show_page(menu, "project"); }},
        {"screenshot", "", "Screenshot", nullptr, nullptr, false,
         [](Container *, ExtraMenu *menu) {
             close_extra(menu->dock);
             windowing::timer(menu->dock->app, 100, [](void *) {
                 system("hyprctl dispatch \"hl.plugin.mylar.screenshot_tool()\"");
             }, nullptr);
         }},
        {"kill", "", "Kill window", nullptr, nullptr, false,
         [](Container *, ExtraMenu *menu) {
             close_extra(menu->dock);
             system("hyprctl kill");
         }},
    };
}

static void make_tray_area(Container *parent);

static constexpr float tray_h = 60;

static void fill_main_page(Container *page, ExtraMenu *menu) {
    extra_page_padding(page);
    auto grid = make_flow_grid(page);
    for (auto &tile : extra_tiles())
        add_tile(grid, menu, tile);
    // Laptops have night light in the battery menu instead.
    if (!has_internal_battery()) {
        auto gap = page->child(FILL_SPACE, 8);
        gap->pre_layout = [](Container *root, Container *c, const Bounds &) {
            c->wanted_bounds.h = 8 * extra_dpi(root);
        };
        make_nightlight_slider(page, extra_window);
    }
    make_tray_area(page);
}

int extra_popup_height() {
    int shown = 0;
    for (auto &tile : extra_tiles())
        if (!tile.visible || tile.visible())
            shown++;
    int cols = flow_columns(extra_popup_w - extra_pad * 2, tile_min_w, tile_gap);
    int rows = std::max(1, (int) std::ceil((double) shown / cols));
    float main_h = extra_pad * 2 + rows * tile_h + (rows - 1) * tile_gap + 10 + tray_h;
    if (!has_internal_battery())
        main_h += 8 + 32; // Night light temperature slider
    // Leaves the Project page room for a few monitors.
    return (int) std::ceil(std::max(main_h, 236.0f));
}

/*************************************************
 *
 * Project page: how the other monitors show this one
 *
 *************************************************/

static const char *projection_mode_names[] = {"Extend", "Mirror", "Off"};
static constexpr int projection_mode_count = 3;

// Asks the main thread for the other monitors and how each is used.
static void extra_fetch_targets(ExtraMenu *menu) {
    auto dock = menu->dock;
    std::string ours = menu->ours;
    main_thread([dock, menu, ours]() {
        std::vector<ProjectionTarget> targets;
        for (auto &m : hypriso->all_monitors())
            if (m.from != ours)
                targets.push_back({m.from, monitor_projection_mode(ours, m.from)});
        windowing::run_on_app(dock->app, [dock, menu, targets]() {
            std::lock_guard<std::mutex> lock(dock->app->mutex);
            if (!extra_menu_alive(menu))
                return;
            menu->targets = targets;
            menu->targets_ready = true;
            if (menu->focus_row >= (int) targets.size())
                menu->focus_row = (int) targets.size() - 1;
            if (menu->via_keyboard && menu->focus_row < 0 && !targets.empty())
                menu->focus_row = 0;
            // During a slide this only swaps the incoming page; the slide keeps going.
            if (menu->page == "project")
                extra_refill_page(menu);
            windowing::redraw(extra_window(dock)->raw_window);
        });
    });
}

static void extra_set_mode(ExtraMenu *menu, int row, int mode) {
    if (row < 0 || row >= (int) menu->targets.size())
        return;
    mode = std::clamp(mode, 0, projection_mode_count - 1);
    auto &target = menu->targets[row];
    if (target.mode == mode)
        return;
    target.mode = mode;
    // Mirroring onto a monitor that mirrors another, or turning one off, can
    // change what the others do, so ask again once the rules are written.
    main_thread([ours = menu->ours, name = target.name, mode]() {
        monitor_set_projection_mode(ours, name, mode);
    });
    extra_fetch_targets(menu);
    windowing::redraw(extra_window(menu->dock)->raw_window);
}

static void add_projection_row(Container *page, ExtraMenu *menu, int row) {
    auto line = page->child(::hbox, FILL_SPACE, FILL_SPACE);
    line->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = extra_dpi(root);
        c->wanted_bounds.h = 40 * dpi;
        c->wanted_pad = Bounds(0, 6 * dpi, 6 * dpi, 6 * dpi);
    };
    line->when_paint = [menu, row](Container *root, Container *c) {
        auto cr = extra_cr(root);
        auto dpi = extra_dpi(root);
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 8 * dpi, 1.0);
        set_argb(cr, {0, 0, 0, .045});
        cr->fill();
        if (menu->focus_row == row) {
            set_argb(cr, accent);
            drawRoundedRect(cr, b.x + dpi, b.y + dpi, b.w - 2 * dpi, b.h - 2 * dpi, 7 * dpi, std::round(2 * dpi));
            cr->stroke();
        }
    };

    auto name = line->child(FILL_SPACE, FILL_SPACE);
    name->when_paint = [menu, row](Container *root, Container *c) {
        if (row >= (int) menu->targets.size())
            return;
        auto cr = extra_cr(root);
        auto dpi = extra_dpi(root);
        auto &target = menu->targets[row];
        RGBA ink = {0, 0, 0, target.mode == MONITOR_PROJECTION_OFF ? .45 : .85};
        std::string glyph = "";
        auto gb = draw_text(cr, 0, 0, glyph, 13 * dpi, false, icon_font, -1, -1, ink);
        double x = c->real_bounds.x + 10 * dpi;
        draw_text(cr, x, center_y(c, gb.h), glyph, 13 * dpi, true, icon_font, -1, -1, ink);
        x += gb.w + 8 * dpi;
        cr->save();
        cr->rectangle(x, c->real_bounds.y, std::max(0.0, c->real_bounds.x + c->real_bounds.w - x - 4 * dpi), c->real_bounds.h);
        cr->clip_path();
        auto tb = draw_text(cr, 0, 0, target.name, 11 * dpi, false, set->font, -1, -1, ink);
        draw_text(cr, x, center_y(c, tb.h), target.name, 11 * dpi, true, set->font, -1, -1, ink);
        cr->restore();
    };

    // Extend | Mirror | Off
    auto group = line->child(::hbox, FILL_SPACE, FILL_SPACE);
    group->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 180 * extra_dpi(root);
    };
    group->when_paint = [](Container *root, Container *c) {
        auto cr = extra_cr(root);
        auto dpi = extra_dpi(root);
        auto b = c->real_bounds;
        set_argb(cr, {0, 0, 0, .07});
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 6 * dpi, 1.0);
        cr->fill();
    };
    for (int mode = 0; mode < projection_mode_count; mode++) {
        auto seg = group->child(FILL_SPACE, FILL_SPACE);
        seg->when_paint = [menu, row, mode](Container *root, Container *c) {
            if (row >= (int) menu->targets.size())
                return;
            auto cr = extra_cr(root);
            auto dpi = extra_dpi(root);
            auto b = c->real_bounds;
            bool selected = menu->targets[row].mode == mode;
            if (selected) {
                set_argb(cr, accent);
                drawRoundedRect(cr, b.x + 2 * dpi, b.y + 2 * dpi, b.w - 4 * dpi, b.h - 4 * dpi, 5 * dpi, 1.0);
                cr->fill();
            } else if (c->state.mouse_hovering || c->state.mouse_pressing) {
                set_argb(cr, {0, 0, 0, c->state.mouse_pressing ? .12 : .06});
                drawRoundedRect(cr, b.x + 2 * dpi, b.y + 2 * dpi, b.w - 4 * dpi, b.h - 4 * dpi, 5 * dpi, 1.0);
                cr->fill();
            }
            RGBA ink = selected ? RGBA(1, 1, 1, 1) : RGBA(0, 0, 0, .8);
            std::string text = projection_mode_names[mode];
            auto tb = draw_text(cr, 0, 0, text, 10 * dpi, false, set->font, -1, -1, ink);
            draw_text(cr, center_x(c, tb.w), center_y(c, tb.h), text, 10 * dpi, true, set->font, -1, -1, ink);
        };
        seg->when_clicked = [menu, row, mode](Container *root, Container *c) {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            extra_set_mode(menu, row, mode);
        };
    }
}

static void fill_project_page(Container *page, ExtraMenu *menu) {
    extra_page_padding(page);
    add_page_header(page, menu, "Project", "main");
    if (!menu->targets_ready) {
        add_text(page, "Looking for displays…", 10, {0, 0, 0, .5}, 20);
        return;
    }
    if (menu->targets.empty()) {
        add_text(page, "No other displays are connected", 10, {0, 0, 0, .5}, 20);
        return;
    }
    add_text(page, "How other displays show this screen", 10, {0, 0, 0, .55}, 16);
    for (int row = 0; row < (int) menu->targets.size(); row++)
        add_projection_row(page, menu, row);
}

static void extra_key(Container *root, ExtraMenu *menu, xkb_keysym_t sym) {
    if (sym == XKB_KEY_Escape) {
        if (menu->page != "main" && !menu->via_keyboard)
            extra_show_page(menu, "main");
        else
            close_extra(menu->dock);
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        close_extra(menu->dock);
        return;
    }
    if (menu->page != "project")
        return;
    if (sym == XKB_KEY_BackSpace) {
        extra_show_page(menu, "main");
        return;
    }
    int count = (int) menu->targets.size();
    if (count == 0)
        return;
    int &row = menu->focus_row;
    if (sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab) {
        row = row <= 0 ? count - 1 : row - 1;
    } else if (sym == XKB_KEY_Down || sym == XKB_KEY_Tab) {
        row = row < 0 ? 0 : (row + 1) % count;
    } else if (sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_p || sym == XKB_KEY_P || sym == XKB_KEY_space) {
        if (row < 0)
            row = 0;
        int mode = menu->targets[row].mode;
        if (sym == XKB_KEY_Left)
            mode = std::max(0, mode - 1);
        else if (sym == XKB_KEY_Right)
            mode = std::min(projection_mode_count - 1, mode + 1);
        else
            mode = (mode + 1) % projection_mode_count; // P again cycles, like Windows
        extra_set_mode(menu, row, mode);
    } else {
        return;
    }
    extra_redraw(root);
}

/*************************************************
 *
 * System tray (grey area under the big buttons)
 *
 *************************************************/

struct TrayIconCache {
    unsigned long revision = 0;
    int size = 0;
    cairo_surface_t *surface = nullptr;
    bool attempted = false;
};

struct TrayAreaData : UserData {
    unsigned long revision = (unsigned long) -1;
    std::vector<TrayItemView> items;
    std::map<std::string, TrayIconCache> icons;

    ~TrayAreaData() override {
        for (auto &[key, cache] : icons)
            if (cache.surface)
                cairo_surface_destroy(cache.surface);
    }
};

struct TrayLayout {
    double cell = 0;
    double icon = 0;
    double x = 0;
    double y = 0;
    int per_row = 1;
};

static TrayLayout tray_layout(const Bounds &b, size_t count, double dpi) {
    TrayLayout l;
    double pad = 8 * dpi;
    l.cell = 34 * dpi;
    for (int attempt = 0; attempt < 4; attempt++) {
        l.per_row = std::max(1, (int) std::floor((b.w - pad * 2) / l.cell));
        int rows = std::max(1, (int) std::ceil((double) count / l.per_row));
        if (rows * l.cell <= b.h - pad * 2 + .5 || l.cell <= 20 * dpi)
            break;
        // Shrink the cells until every icon fits.
        l.cell = std::max(20 * dpi, (b.h - pad * 2) / rows);
    }
    l.icon = std::round(l.cell * .65);
    int rows = std::max(1, (int) std::ceil((double) count / l.per_row));
    l.x = b.x + pad;
    l.y = b.y + std::max(pad, (b.h - rows * l.cell) * .5);
    return l;
}

static int tray_item_at(Container *root, Container *c, TrayAreaData *data, double dpi) {
    auto l = tray_layout(c->real_bounds, data->items.size(), dpi);
    int col = (int) std::floor((root->mouse_current_x - l.x) / l.cell);
    int row = (int) std::floor((root->mouse_current_y - l.y) / l.cell);
    if (col < 0 || col >= l.per_row || row < 0)
        return -1;
    int index = row * l.per_row + col;
    return index < (int) data->items.size() ? index : -1;
}

static std::string tray_icon_file(const TrayItemView &item, int size) {
    namespace fs = std::filesystem;
    const auto &name = item.icon_name;
    if (name.empty())
        return "";
    std::error_code ec;
    if (name[0] == '/')
        return fs::exists(name, ec) ? name : "";
    if (!item.icon_theme_path.empty()) {
        for (auto ext : {".png", ".svg", ".xpm"}) {
            auto path = item.icon_theme_path + "/" + name + ext;
            if (fs::exists(path, ec))
                return path;
        }
        // Apps often ship a small hicolor-style tree in IconThemePath.
        std::string best;
        fs::recursive_directory_iterator it(item.icon_theme_path, fs::directory_options::skip_permission_denied, ec), end;
        for (int visited = 0; !ec && it != end && visited < 2000; it.increment(ec), visited++) {
            if (!it->is_regular_file(ec))
                continue;
            auto path = it->path();
            if (path.stem() != name)
                continue;
            auto ext = path.extension().string();
            if (ext != ".png" && ext != ".svg" && ext != ".xpm")
                continue;
            best = path.string();
            if (ext == ".svg")
                break;
        }
        if (!best.empty())
            return best;
    }
    if (!icons_loaded)
        return "";
    return one_shot_icon(size, {name, to_lower(name)});
}

static cairo_surface_t *tray_icon_surface(TrayAreaData *data, const TrayItemView &item, int size) {
    auto &cache = data->icons[item.key];
    if (cache.attempted && cache.revision == item.revision && cache.size == size)
        return cache.surface;
    if (cache.surface)
        cairo_surface_destroy(cache.surface);
    cache = {};
    cache.revision = item.revision;
    cache.size = size;
    cache.attempted = true;
    auto path = tray_icon_file(item, size);
    if (!path.empty())
        load_icon_full_path(&cache.surface, path, size);
    if (!cache.surface && item.pixels.w > 0 && item.pixels.h > 0) {
        auto pixels = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, item.pixels.w, item.pixels.h);
        cairo_surface_flush(pixels);
        auto stride = cairo_image_surface_get_stride(pixels);
        auto dst = cairo_image_surface_get_data(pixels);
        for (int y = 0; y < item.pixels.h; y++)
            memcpy(dst + (size_t) y * stride, item.pixels.argb.data() + (size_t) y * item.pixels.w, (size_t) item.pixels.w * 4);
        cairo_surface_mark_dirty(pixels);
        // Scale to the cell size once instead of on every paint.
        cache.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        // Offscreen icon preparation (once per icon), not window painting.
        auto scale_cr = cairo_create(cache.surface);
        double scale = (double) size / std::max(item.pixels.w, item.pixels.h);
        cairo_translate(scale_cr, (size - item.pixels.w * scale) * .5, (size - item.pixels.h * scale) * .5);
        cairo_scale(scale_cr, scale, scale);
        cairo_set_source_surface(scale_cr, pixels, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(scale_cr), CAIRO_FILTER_GOOD);
        cairo_paint(scale_cr);
        cairo_destroy(scale_cr);
        cairo_surface_destroy(pixels);
    }
    return cache.surface;
}

static void open_tray_menu(Dock *dock, MylarWindow *parent, std::string key, std::vector<TrayMenuItem> items, Bounds anchor_rect);

static void tray_clicked(Container *root, Container *c, int button) {
    auto dock = (Dock *) root->user_data;
    auto data = (TrayAreaData *) c->user_data;
    auto extra = extra_window(dock);
    auto dpi = extra->raw_window->dpi;
    int index = tray_item_at(root, c, data, dpi);
    if (index == -1)
        return;
    auto item = data->items[index];
    bool show_menu = !item.xembed && item.has_menu && (button == 3 || (button == 1 && item.item_is_menu));

    if (show_menu) {
        auto l = tray_layout(c->real_bounds, data->items.size(), dpi);
        Bounds anchor_rect(
            l.x + (index % l.per_row) * l.cell,
            l.y + (index / l.per_row) * l.cell,
            l.cell,
            l.cell);
        main_thread([dock, extra, key = item.key, anchor_rect]() {
            systray::fetch_menu(key, [dock, extra, key, anchor_rect](std::vector<TrayMenuItem> menu) {
                if (menu.empty()) {
                    // No usable dbusmenu: let the app show its own.
                    if (!finished && !dock->closing) {
                        windowing::run_on_app(dock->app, [dock, extra]() {
                            std::lock_guard<std::mutex> lock(dock->app->mutex);
                            if (!finished && !dock->closing && dock->extra == extra && windowing::has_window(extra->raw_window))
                                close_extra(dock);
                        });
                    }
                    systray::click(key, 3);
                    return;
                }
                open_tray_menu(dock, extra, key, std::move(menu), anchor_rect);
            });
        });
    } else {
        close_extra(dock);
        main_thread([key = item.key, button]() {
            systray::click(key, button);
        });
    }
}

static void make_tray_area(Container *parent) {
    auto tray = parent->child(FILL_SPACE, FILL_SPACE);
    tray->name = "systray";
    tray->user_data = new TrayAreaData;
    tray->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto s = extra_window(dock)->raw_window->dpi;
        c->wanted_bounds = Bounds(0, 0, FILL_SPACE, tray_h * s);
    };
    tray->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = extra_window(dock);
        auto cr = window->raw_window->drawing_context;
        auto dpi = window->raw_window->dpi;
        auto data = (TrayAreaData *) c->user_data;
        auto b = c->real_bounds;

        set_argb(cr, {0, 0, 0, .08});
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        cr->fill();

        if (systray::revision() != data->revision) {
            data->items = systray::snapshot(&data->revision);
            // Forget icons of items that are gone.
            for (auto it = data->icons.begin(); it != data->icons.end();) {
                bool alive = std::any_of(data->items.begin(), data->items.end(), [&](const TrayItemView &item) { return item.key == it->first; });
                if (!alive) {
                    if (it->second.surface)
                        cairo_surface_destroy(it->second.surface);
                    it = data->icons.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (data->items.empty()) {
            auto tb = draw_text(cr, 0, 0, "No tray icons", 10 * dpi, false, set->font, -1, -1, {0, 0, 0, .35});
            draw_text(cr, center_x(c, tb.w), center_y(c, tb.h), "No tray icons", 10 * dpi, true, set->font, -1, -1, {0, 0, 0, .35});
            return;
        }

        auto l = tray_layout(b, data->items.size(), dpi);
        int hovered = c->state.mouse_hovering ? tray_item_at(root, c, data, dpi) : -1;
        cr->save();
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        cr->clip_path();
        for (int i = 0; i < (int) data->items.size(); i++) {
            auto &item = data->items[i];
            double x = l.x + (i % l.per_row) * l.cell;
            double y = l.y + (i / l.per_row) * l.cell;
            if (i == hovered) {
                set_argb(cr, c->state.mouse_pressing ? RGBA(0, 0, 0, .14) : RGBA(0, 0, 0, .08));
                drawRoundedRect(cr, x + 2 * dpi, y + 2 * dpi, l.cell - 4 * dpi, l.cell - 4 * dpi, 6 * dpi, 1.0);
                cr->fill();
            }
            if (item.needs_attention) {
                set_argb(cr, {.95, .45, .1, .25});
                drawRoundedRect(cr, x + 2 * dpi, y + 2 * dpi, l.cell - 4 * dpi, l.cell - 4 * dpi, 6 * dpi, 1.0);
                cr->fill();
            }
            int size = (int) l.icon;
            double ix = std::round(x + (l.cell - size) * .5);
            double iy = std::round(y + (l.cell - size) * .5);
            if (auto surface = tray_icon_surface(data, item, size)) {
                double sw = cairo_image_surface_get_width(surface);
                double sh = cairo_image_surface_get_height(surface);
                cr->save();
                cr->translate(ix, iy);
                if (sw > 0 && sh > 0 && (sw != size || sh != size))
                    cr->scale(size / sw, size / sh);
                cr->set_source_surface(surface, 0, 0);
                cr->paint_source();
                cr->restore();
            } else {
                // No icon: a circle with the first letter of the title.
                set_argb(cr, {0, 0, 0, .2});
                cr->arc(ix + size * .5, iy + size * .5, size * .5, 0, 2 * M_PI);
                cr->fill();
                std::string letter = item.title.empty() ? "?" : item.title.substr(0, 1);
                auto tb = draw_text(cr, 0, 0, letter, size * .5, false, set->font, -1, -1, {0, 0, 0, .8});
                draw_text(cr, ix + size * .5 - tb.w * .5, iy + size * .5 - tb.h * .5, letter, size * .5, true, set->font, -1, -1, {0, 0, 0, .8});
            }
        }
        cr->restore();
    };
    tray->when_mouse_motion = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        windowing::redraw(extra_window(dock)->raw_window);
    };
    tray->when_mouse_leaves_container = tray->when_mouse_motion;
    tray->when_clicked = [](Container *root, Container *c) {
        int button = c->state.mouse_button_pressed == BTN_RIGHT ? 3 : c->state.mouse_button_pressed == BTN_MIDDLE ? 2 : 1;
        tray_clicked(root, c, button);
    };
    tray->when_fine_scrolled = [](Container *root, Container *c, double scroll_x, double scroll_y, bool) {
        auto dock = (Dock *) root->user_data;
        auto data = (TrayAreaData *) c->user_data;
        int index = tray_item_at(root, c, data, extra_window(dock)->raw_window->dpi);
        if (index == -1)
            return;
        bool horizontal = std::abs(scroll_x) > std::abs(scroll_y);
        double amount = horizontal ? scroll_x : scroll_y;
        if (amount == 0)
            return;
        int delta = amount > 0 ? 1 : -1;
        main_thread([key = data->items[index].key, delta, horizontal]() {
            systray::scroll(key, delta, horizontal);
        });
    };
}

/*************************************************
 *
 * Tray item menu (com.canonical.dbusmenu)
 *
 *************************************************/

struct TrayMenuData : UserData {
    std::string key;
    std::vector<TrayMenuItem> items;
    std::vector<int> path; // indices of the open submenus
};

static constexpr int tray_menu_row_h = 30;
static constexpr int tray_menu_separator_h = 9;
static constexpr int tray_menu_w = 280;

static const std::vector<TrayMenuItem> &tray_menu_level(TrayMenuData *data) {
    const std::vector<TrayMenuItem> *level = &data->items;
    for (int index : data->path) {
        if (index < 0 || index >= (int) level->size())
            break;
        level = &(*level)[index].children;
    }
    return *level;
}

static int tray_menu_height(const std::vector<TrayMenuItem> &items, bool back_row) {
    int h = back_row ? tray_menu_row_h + tray_menu_separator_h : 0;
    int deepest = 0;
    for (auto &item : items) {
        if (!item.visible)
            continue;
        h += item.separator ? tray_menu_separator_h : tray_menu_row_h;
        if (!item.children.empty())
            deepest = std::max(deepest, tray_menu_height(item.children, true));
    }
    return std::max(h, deepest);
}

// Rows of the current level: -1 is the "Back" row, -2 a separator, otherwise an index into the level.
static std::vector<std::pair<int, Bounds>> tray_menu_rows(TrayMenuData *data, Bounds area, double dpi) {
    std::vector<std::pair<int, Bounds>> rows;
    double y = area.y;
    if (!data->path.empty()) {
        rows.push_back({-1, Bounds(area.x, y, area.w, tray_menu_row_h * dpi)});
        y += tray_menu_row_h * dpi;
        rows.push_back({-2, Bounds(area.x, y, area.w, tray_menu_separator_h * dpi)});
        y += tray_menu_separator_h * dpi;
    }
    auto &level = tray_menu_level(data);
    for (int i = 0; i < (int) level.size(); i++) {
        if (!level[i].visible)
            continue;
        double h = (level[i].separator ? tray_menu_separator_h : tray_menu_row_h) * dpi;
        rows.push_back({level[i].separator ? -2 : i, Bounds(area.x, y, area.w, h)});
        y += h;
    }
    return rows;
}

static void open_tray_menu(Dock *dock, MylarWindow *parent, std::string key, std::vector<TrayMenuItem> items, Bounds anchor_rect) {
    struct Request {
        Dock *dock;
        MylarWindow *parent;
        std::string key;
        std::vector<TrayMenuItem> items;
        Bounds anchor_rect;
    };
    auto request = std::make_shared<Request>(Request{dock, parent, std::move(key), std::move(items), anchor_rect});
    // Called on the main thread: hop to the dock's thread, where its windows live.
    windowing::run_on_app(dock->app, [request]() {
        auto dock = request->dock;
        std::lock_guard<std::mutex> lock(dock->app->mutex);
        auto extra = request->parent;
        if (finished || dock->closing || dock->extra != extra || extra->popup_window ||
            !windowing::has_window(extra->raw_window))
            return;
        auto dpi = extra->raw_window->dpi;
        int h = std::min(640, tray_menu_height(request->items, false) + 12);
        RawWindowSettings settings;
        settings.pos.w = tray_menu_w;
        settings.pos.h = h;
        settings.name = "Popup";
        settings.popup.use_explicit_anchor_rect = true;
        settings.popup.anchor_rect_x = (int) std::round(request->anchor_rect.x / dpi);
        settings.popup.anchor_rect_y = (int) std::round(request->anchor_rect.y / dpi);
        settings.popup.anchor_rect_w = std::max(1, (int) std::round(request->anchor_rect.w / dpi));
        settings.popup.anchor_rect_h = std::max(1, (int) std::round(request->anchor_rect.h / dpi));
        settings.popup.anchor = RawWindowSettings::PopupAnchor::TOP;
        settings.popup.gravity = RawWindowSettings::PopupGravity::TOP;
        settings.popup.use_offset = true;
        settings.popup.offset_y = -6;
        settings.popup.constraint_adjustment =
            RawWindowSettings::POPUP_CONSTRAINT_SLIDE_X |
            RawWindowSettings::POPUP_CONSTRAINT_SLIDE_Y |
            RawWindowSettings::POPUP_CONSTRAINT_FLIP_X |
            RawWindowSettings::POPUP_CONSTRAINT_FLIP_Y;
        auto menu = open_mylar_popup(extra, settings);
        if (!menu)
            return;
        dock->tray_menu = menu;
        auto root = menu->root;
        root->user_data = dock;
        root->wanted_bounds.w = FILL_SPACE;
        root->wanted_bounds.h = FILL_SPACE;
        root->on_closed = [menu](Container *root) {
            auto dock = (Dock *) root->user_data;
            if (dock->tray_menu == menu)
                dock->tray_menu = nullptr;
        };
        root->when_paint = [](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = dock->tray_menu->raw_window;
            paint_popup_background(window->drawing_context, c->real_bounds, window->dpi);
        };

        auto list = root->child(FILL_SPACE, FILL_SPACE);
        auto data = new TrayMenuData;
        data->key = request->key;
        data->items = std::move(request->items);
        list->user_data = data;
        auto area = [](Container *root, Container *c) {
            auto dpi = ((Dock *) root->user_data)->tray_menu->raw_window->dpi;
            auto b = c->real_bounds;
            return Bounds(b.x + 6 * dpi, b.y + 6 * dpi, b.w - 12 * dpi, b.h - 12 * dpi);
        };
        list->when_paint = [area](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = dock->tray_menu->raw_window;
            auto cr = window->drawing_context;
            auto dpi = window->dpi;
            auto data = (TrayMenuData *) c->user_data;
            auto &level = tray_menu_level(data);
            cr->save();
            clip_to_popup_background(cr, c->real_bounds, dpi);
            for (auto &[index, rb] : tray_menu_rows(data, area(root, c), dpi)) {
                if (index == -2) {
                    set_argb(cr, {0, 0, 0, .1});
                    cr->rectangle(rb.x + 8 * dpi, std::round(rb.y + rb.h * .5), rb.w - 16 * dpi, std::max(1.0, (double) std::floor(dpi)));
                    cr->fill();
                    continue;
                }
                bool enabled = index == -1 || level[index].enabled;
                bool hovered = c->state.mouse_hovering && bounds_contains(rb, root->mouse_current_x, root->mouse_current_y);
                if (hovered && enabled) {
                    set_argb(cr, c->state.mouse_pressing ? RGBA(0, 0, 0, .14) : RGBA(0, 0, 0, .07));
                    drawRoundedRect(cr, rb.x, rb.y, rb.w, rb.h, 6 * dpi, 1.0);
                    cr->fill();
                }
                RGBA ink = enabled ? RGBA(0, 0, 0, .9) : RGBA(0, 0, 0, .35);
                std::string label = index == -1 ? "‹  Back" : level[index].label;
                auto tb = draw_text(cr, 0, 0, label, 11 * dpi, false, set->font, -1, -1, ink);
                cr->save();
                cr->rectangle(rb.x, rb.y, rb.w - 26 * dpi, rb.h);
                cr->clip_path();
                draw_text(cr, rb.x + 30 * dpi, rb.y + rb.h * .5 - tb.h * .5, label, 11 * dpi, true, set->font, -1, -1, ink);
                cr->restore();
                if (index >= 0) {
                    auto &item = level[index];
                    if (item.toggle_state == 1 && !item.toggle_type.empty()) {
                        std::string mark = item.toggle_type == "radio" ? "●" : "✓";
                        auto mb = draw_text(cr, 0, 0, mark, 11 * dpi, false, set->font, -1, -1, ink);
                        draw_text(cr, rb.x + 15 * dpi - mb.w * .5, rb.y + rb.h * .5 - mb.h * .5, mark, 11 * dpi, true, set->font, -1, -1, ink);
                    }
                    if (!item.children.empty()) {
                        auto ab = draw_text(cr, 0, 0, "›", 14 * dpi, false, set->font, -1, -1, ink);
                        draw_text(cr, rb.x + rb.w - 12 * dpi - ab.w, rb.y + rb.h * .5 - ab.h * .5, "›", 14 * dpi, true, set->font, -1, -1, ink);
                    }
                }
            }
            cr->restore();
        };
        list->when_mouse_motion = [](Container *root, Container *c) {
            windowing::redraw(((Dock *) root->user_data)->tray_menu->raw_window);
        };
        list->when_mouse_leaves_container = list->when_mouse_motion;
        list->when_clicked = [area](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = dock->tray_menu->raw_window;
            auto data = (TrayMenuData *) c->user_data;
            auto &level = tray_menu_level(data);
            for (auto &[index, rb] : tray_menu_rows(data, area(root, c), window->dpi)) {
                if (!bounds_contains(rb, root->mouse_current_x, root->mouse_current_y))
                    continue;
                if (index == -1) {
                    data->path.pop_back();
                } else if (index >= 0 && level[index].enabled) {
                    if (!level[index].children.empty()) {
                        data->path.push_back(index);
                    } else {
                        int id = level[index].id;
                        main_thread([key = data->key, id]() {
                            systray::menu_clicked(key, id);
                        });
                        windowing::close_window(window);
                        if (auto extra = extra_window(dock); extra && extra->popup_window == dock->tray_menu)
                            extra->popup_window = nullptr;
                        return;
                    }
                }
                windowing::redraw(window);
                return;
            }
        };
        windowing::redraw(dock->tray_menu->raw_window);
    });
}

void fill_extra_container(Container *root, std::string page, bool via_keyboard) {
    auto dock = (Dock *) root->user_data;
    root->type = ::vbox;
    root->wanted_bounds.w = FILL_SPACE;
    root->wanted_bounds.h = FILL_SPACE;
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        paint_popup_background(extra_cr(root), c->real_bounds, extra_window(dock)->raw_window->dpi);
    };

    auto menu = new ExtraMenu;
    menu->dock = dock;
    menu->ours = dock->creation_settings.monitor_name;
    menu->via_keyboard = via_keyboard;
    menu->page = extra_page(page) ? page : "main";

    auto pages = root->child(::fullycustom, FILL_SPACE, FILL_SPACE);
    pages->user_data = menu;
    menu->pages = pages;
    pages->pre_layout = [](Container *root, Container *c, const Bounds &) {
        for (auto child : c->children) {
            if (child->pre_layout)
                child->pre_layout(root, child, c->real_bounds);
            ::layout(root, child, c->real_bounds);
        }
    };
    pages->when_paint = extra_pages_paint_before;
    pages->after_paint = extra_pages_paint_after;

    root->on_closed = [menu](Container *root) {
        menu->closed = true;
        auto dock = (Dock *) root->user_data;
        if (dock && dock->extra && dock->extra->root == root)
            dock->extra = nullptr;
    };
    root->when_key_event = [menu](Container *root, Container *, int, bool pressed, xkb_keysym_t sym, int, bool, std::string) {
        if (pressed)
            extra_key(root, menu, sym);
    };

    extra_build_page(menu);
    extra_fetch_targets(menu);
}

void open_extra_popup(Dock *dock, std::string page, bool via_keyboard) {
    auto mylar = dock->window;
    auto anchor = container_by_name("extra", mylar->root);
    if (!anchor || mylar->popup_window)
        return;
    auto dpi = mylar->raw_window->dpi;

    RawWindowSettings settings = make_icon_anchored_popup_settings(anchor, dpi, extra_popup_w, extra_popup_height());
    // Above the button, extending left from its right edge.
    settings.popup.anchor = RawWindowSettings::PopupAnchor::TOP_RIGHT;
    settings.popup.gravity = RawWindowSettings::PopupGravity::TOP_LEFT;
    settings.keyboard_focus_on_click = via_keyboard;

    auto extra = open_mylar_popup(mylar, settings);
    if (!extra)
        return;
    dock->extra = extra;
    extra->root->user_data = dock;
    fill_extra_container(extra->root, page, via_keyboard);

    if (via_keyboard) {
        later(30, [dock, extra](Timer *) {
            if (dock->extra == extra && windowing::has_window(extra->raw_window))
                windowing::request_keyboard_focus(extra->raw_window);
        });
    }
    windowing::redraw(extra->raw_window);
}
