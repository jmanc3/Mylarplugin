#include "dock/dock_wifi.h"

#include "dock/dock_internal.h"

#include "simple_dbus.h"
#include "text_editor.h"

// Ported from winbar's wifi_menu.cpp. All NetworkManager / wpa_supplicant calls
// block, so they run on a worker thread that publishes value snapshots. The
// Wayland dock thread only reads those snapshots and posts commands back.

static std::mutex wifi_mutex;
static std::condition_variable wifi_wakeup;
static std::thread wifi_thread;
static bool wifi_thread_running = false;
static int wifi_menu_count = 0;
static std::vector<WifiCommand> wifi_commands;
static std::atomic<std::shared_ptr<const WifiMenuView>> wifi_latest_view;

void wifi_command(WifiCommand command) {
    {
        std::lock_guard<std::mutex> lock(wifi_mutex);
        wifi_commands.push_back(std::move(command));
    }
    wifi_wakeup.notify_all();
}

const WifiDevice *wifi_device(const WifiStatus &status) {
    for (const auto &device : status.devices)
        if (device.path == status.device_path) return &device;
    return nullptr;
}

const WifiAccessPoint *wifi_access_point(const WifiStatus &status, const std::string &ssid) {
    for (const auto &ap : status.access_points)
        if (ap.ssid == ssid) return &ap;
    return nullptr;
}

static void wifi_worker() {
    auto bus = wifi_backend_open();
    std::string device_path;
    unsigned long revision = 0;
    // Pending connection, polled until NetworkManager / wpa_supplicant settles.
    std::string pending_active, pending_created, pending_ssid;
    bool pending_secured = false;
    auto pending_started = std::chrono::steady_clock::now();
    // Scan progress: NetworkManager bumps LastScan when results arrive.
    bool scanning = false;
    long long scan_last = -1;
    auto scan_started = std::chrono::steady_clock::now();
    // Captive portals (cafés, hotels): NetworkManager reports Connectivity == PORTAL.
    auto portal_recheck_until = std::chrono::steady_clock::now();
    auto portal_last_check = std::chrono::steady_clock::now();
    std::string message;
    bool message_error = false;
    auto message_until = std::chrono::steady_clock::now();
    auto feedback = [&](std::string text, bool error) {
        message = std::move(text);
        message_error = error;
        message_until = std::chrono::steady_clock::now() + std::chrono::seconds(9);
    };

    while (!finished) {
        std::vector<WifiCommand> commands;
        {
            std::unique_lock<std::mutex> lock(wifi_mutex);
            // Nothing to show and nothing in flight: sleep until a menu opens.
            wifi_wakeup.wait(lock, [&]() {
                return finished || wifi_menu_count > 0 || !wifi_commands.empty() || !pending_active.empty();
            });
            if (finished) break;
            commands.swap(wifi_commands);
        }

        auto status = wifi_backend_read(bus, device_path);
        device_path = status.device_path;
        std::string action_ssid, action;
        for (const auto &command : commands) {
            std::string error;
            if (command.type == "scan" || command.type == "auto-scan") {
                auto device = wifi_device(status);
                if (!device || !status.wireless_enabled) continue;
                if (wifi_backend_scan(bus, device_path, error)) {
                    scanning = true;
                    scan_last = device->last_scan;
                    scan_started = std::chrono::steady_clock::now();
                } else if (command.type == "scan") {
                    // NetworkManager refuses scans requested too soon after the last one.
                    feedback(error.empty() ? "Couldn't scan for networks." : "Couldn't scan: " + error, true);
                }
            } else if (command.type == "toggle") {
                bool target = !status.wireless_enabled;
                if (!target && !pending_active.empty()) {
                    wifi_backend_finish(bus, pending_active, false);
                    pending_active.clear();
                }
                if (wifi_backend_set_enabled(bus, device_path, target, error))
                    feedback(target ? "Wi-Fi turned on" : "Wi-Fi turned off", false);
                else
                    feedback("Couldn't change Wi-Fi: " + error, true);
            } else if (command.type == "device") {
                for (size_t i = 0; i < status.devices.size(); ++i) {
                    if (status.devices[i].path != device_path) continue;
                    device_path = status.devices[(i + 1) % status.devices.size()].path;
                    break;
                }
                scanning = false;
            } else if (command.type == "notice") {
                feedback(command.text, true);
            } else if (command.type == "auto-check") {
                // Re-probe on open so a finished portal sign-in is noticed promptly.
                if (status.connectivity == 2 || status.connectivity == 3) wifi_backend_check_connectivity(bus);
            } else if (command.type == "sign-in") {
                // Any plain-http page gets redirected to the portal; NetworkManager's
                // own probe URI is the one it uses to detect the portal.
                auto uri = status.portal_uri.starts_with("http://") ? status.portal_uri : std::string("http://neverssl.com");
                main_thread([uri]() {
                    auto quoted = g_shell_quote(uri.c_str());
                    launch_command(std::string("xdg-open ") + quoted);
                    g_free(quoted);
                });
                portal_recheck_until = std::chrono::steady_clock::now() + std::chrono::minutes(3);
            } else if (auto ap = wifi_access_point(status, command.ssid)) {
                if (command.type == "connect") {
                    if (!pending_active.empty()) {
                        wifi_backend_finish(bus, pending_active, false);
                        pending_active.clear();
                    }
                    std::string active, created;
                    if (wifi_backend_connect(bus, device_path, *ap, command.text, active, created, error)) {
                        pending_active = active;
                        pending_created = created;
                        pending_ssid = ap->ssid;
                        pending_secured = ap->secured;
                        pending_started = std::chrono::steady_clock::now();
                        message.clear();
                    } else {
                        feedback("Couldn't connect to " + ap->ssid + ": " + error, true);
                    }
                } else if (command.type == "disconnect") {
                    action_ssid = ap->ssid;
                    action = "Disconnecting…";
                    if (wifi_backend_disconnect(bus, device_path, error))
                        feedback("Disconnected from " + ap->ssid, false);
                    else
                        feedback("Couldn't disconnect: " + error, true);
                } else if (command.type == "forget") {
                    if (pending_ssid == ap->ssid && !pending_active.empty()) {
                        wifi_backend_finish(bus, pending_active, false);
                        pending_active.clear();
                    }
                    if (wifi_backend_forget(bus, ap->saved_connections, error))
                        feedback("Forgot " + ap->ssid, false);
                    else
                        feedback("Couldn't forget " + ap->ssid + ": " + error, true);
                }
            }
        }
        if (!commands.empty()) {
            status = wifi_backend_read(bus, device_path);
            device_path = status.device_path;
        }

        if (!pending_active.empty()) {
            auto state = wifi_backend_active_state(bus, pending_active);
            auto elapsed = std::chrono::steady_clock::now() - pending_started;
            if (state == 2) {
                wifi_backend_finish(bus, pending_active, true);
                pending_active.clear();
                // Open café/hotel networks usually need a sign-in page before internet works.
                auto connectivity = wifi_backend_check_connectivity(bus);
                if (connectivity == 2) {
                    feedback("Connected to " + pending_ssid + ". Sign in to use the internet.", false);
                    portal_recheck_until = std::chrono::steady_clock::now() + std::chrono::minutes(3);
                } else {
                    feedback(connectivity == 3 || connectivity == 1 ?
                        "Connected to " + pending_ssid + ", but there's no internet access." :
                        "Connected to " + pending_ssid, connectivity == 3 || connectivity == 1);
                }
                status = wifi_backend_read(bus, device_path);
            } else if (state == 4 || (state == 0 && elapsed > std::chrono::milliseconds(1500)) ||
                       elapsed > std::chrono::seconds(45)) {
                auto device = wifi_device(status);
                auto reason = device ? device->state_reason : 0;
                // NMDeviceStateReason: no secrets, supplicant disconnect / config / failure / timeout.
                // wpa_supplicant temp-disables a network (state 4) after wrong-key/auth failures.
                bool credentials = status.backend == "wpa_supplicant" ? state == 4 :
                    reason == 7 || reason == 8 || reason == 9 || reason == 10 || reason == 11;
                wifi_backend_finish(bus, pending_active, false);
                feedback(pending_secured && credentials ?
                    "Couldn't connect to " + pending_ssid + ". Check the password and try again." :
                    "Couldn't connect to " + pending_ssid + ".", true);
                // Don't keep a profile holding a password that was just rejected.
                if (!pending_created.empty()) {
                    std::string ignored;
                    wifi_backend_forget(bus, {pending_created}, ignored);
                }
                pending_active.clear();
                status = wifi_backend_read(bus, device_path);
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (status.connectivity == 2 && now < portal_recheck_until &&
            now - portal_last_check > std::chrono::seconds(5)) {
            portal_last_check = now;
            if (wifi_backend_check_connectivity(bus) == 4) {
                feedback("Signed in. You're connected to the internet.", false);
                portal_recheck_until = now;
            }
            status = wifi_backend_read(bus, device_path);
        }

        if (scanning) {
            auto device = wifi_device(status);
            auto elapsed = std::chrono::steady_clock::now() - scan_started;
            bool reported = device && device->last_scan != -1 && device->last_scan != scan_last;
            // wpa_supplicant has no LastScan; its scans finish within a few seconds.
            auto limit = status.backend == "wpa_supplicant" ? std::chrono::seconds(4) : std::chrono::seconds(10);
            if (!device || reported || elapsed > limit) scanning = false;
        }
        if (!message.empty() && std::chrono::steady_clock::now() > message_until) message.clear();

        WifiMenuView view;
        view.revision = ++revision;
        view.status = std::move(status);
        view.scanning = scanning;
        view.connecting_ssid = pending_active.empty() ? "" : pending_ssid;
        view.busy = !view.connecting_ssid.empty();
        view.action_ssid = action_ssid;
        view.action = action;
        view.message = message;
        view.error = message_error;
        wifi_latest_view.store(std::make_shared<const WifiMenuView>(std::move(view)));

        std::unique_lock<std::mutex> lock(wifi_mutex);
        wifi_wakeup.wait_for(lock, scanning || !pending_active.empty() ? std::chrono::milliseconds(500) :
            std::chrono::seconds(3), []() { return finished || !wifi_commands.empty(); });
    }
    wifi_backend_close(bus);
    std::lock_guard<std::mutex> lock(wifi_mutex);
    wifi_thread_running = false;
}

void wifi_menu_opened() {
    {
        std::lock_guard<std::mutex> lock(wifi_mutex);
        ++wifi_menu_count;
        // Same as winbar: opening the menu asks for fresh results.
        wifi_commands.push_back({"auto-scan", "", ""});
        wifi_commands.push_back({"auto-check", "", ""});
        if (!wifi_thread_running && !finished) {
            if (wifi_thread.joinable()) wifi_thread.join();
            wifi_thread_running = true;
            wifi_thread = std::thread(wifi_worker);
        }
    }
    wifi_wakeup.notify_all();
}

void wifi_menu_closed() {
    std::lock_guard<std::mutex> lock(wifi_mutex);
    if (wifi_menu_count > 0) --wifi_menu_count;
}

std::shared_ptr<const WifiMenuView> wifi_latest() {
    return wifi_latest_view.load();
}

void wifi_stop() {
    // dock::stop and the last dock thread may both get here; only one joins.
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(wifi_mutex);
        wifi_menu_count = 0;
        wifi_commands.clear();
        worker = std::move(wifi_thread);
    }
    wifi_wakeup.notify_all();
    if (worker.joinable()) worker.join();
}

/*************************************************
 *
 * Dock button (winbar's tray network icon)
 *
 *************************************************/

// bit 0: link up, bit 1: wired. Shared by every dock; written by wifi_watch_dock.
static std::atomic<int> network_icon_bits = -1;

static int network_icon_compute() {
    auto link = default_network_link();
    return (link.up ? 1 : 0) | (link.up && link.wired ? 2 : 0);
}

SNetworkIconState network_icon_state() {
    int bits = network_icon_bits.load();
    if (bits < 0) network_icon_bits = bits = network_icon_compute();
    return {(bits & 1) != 0, (bits & 2) != 0};
}

static void wifi_watch_dock_tick(Dock *dock, int drawn) {
    windowing::timer(dock->app, 3000, [dock, drawn](void *) {
        if (finished) return;
        int bits = network_icon_compute();
        network_icon_bits = bits;
        if (bits != drawn) {
            std::lock_guard<std::mutex> lock(dock->app->mutex);
            windowing::redraw(dock->window->raw_window);
        }
        wifi_watch_dock_tick(dock, bits);
    }, nullptr);
}

void wifi_watch_dock(Dock *dock) {
    // winbar re-checked its interface state every few seconds; do the same and only
    // repaint the dock when the icon actually changes.
    wifi_watch_dock_tick(dock, network_icon_bits.load());
}

/*************************************************
 *
 * Menu (dock thread)
 *
 *************************************************/

struct WifiMenuData : UserData {
    WifiMenuView view;
    unsigned long revision = 0;
    bool animating = false;
    std::string selected;
    bool password_mode = false;
    bool show_password = false;
    bool confirming_forget = false;
    // Mirror of the selected row's text editor, kept current by its on_change.
    std::string password;
};

struct WifiRow : UserData {
    WifiAccessPoint ap;
    bool hovered = false;
    double expansion = 0;
    double expansion_from = 0;
    double expansion_target = 0;
    long animation_start = 0;
    Container *field = nullptr;
    Container *input = nullptr;
    Container *buttons = nullptr;
    Container *secondary = nullptr;
};

static WifiMenuData *wifi_menu_data(Container *root) {
    return static_cast<WifiMenuData *>(container_by_name("wifi_body", root)->user_data);
}

static RawWindow *wifi_window(Container *root) {
    return static_cast<Dock *>(root->user_data)->wifi->raw_window;
}

static void wifi_redraw(Container *root) {
    windowing::redraw(wifi_window(root));
}

static Container *wifi_row(Container *parent, double height) {
    auto row = parent->child(FILL_SPACE, height);
    row->pre_layout = [height](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = height * wifi_window(root)->dpi;
    };
    return row;
}

static void wifi_draw_text(Container *root, Bounds b, const std::string &text, double size = 10,
                           RGBA color = {.14, .18, .24, 1}, bool centered = false, bool bold = false) {
    auto window = wifi_window(root);
    auto font = get_cached_pango_font(window->cr, set->font, size * window->dpi,
        bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_text(font, text.c_str(), -1);
    pango_layout_set_width(font, std::max(1.0, b.w) * PANGO_SCALE);
    pango_layout_set_height(font, std::max(1.0, b.h) * PANGO_SCALE);
    pango_layout_set_wrap(font, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(font, centered ? PANGO_ALIGN_CENTER : PANGO_ALIGN_LEFT);
    int width, height;
    pango_layout_get_pixel_size(font, &width, &height);
    cairo_save(window->cr);
    cairo_rectangle(window->cr, b.x, b.y, b.w, b.h);
    cairo_clip(window->cr);
    set_argb(window->cr, color);
    cairo_move_to(window->cr, b.x, b.y + std::max(0.0, (b.h - height) * .5));
    pango_cairo_show_layout(window->cr, font);
    cairo_restore(window->cr);
}

static void wifi_paint_button(Container *root, Container *c, const std::string &text, bool enabled) {
    auto window = wifi_window(root);
    auto b = c->real_bounds;
    if (b.h <= 0) return;
    set_argb(window->cr, !enabled ? RGBA(.95, .96, .97, 1) : c->state.mouse_pressing ?
        RGBA(.72, .83, .95, 1) : c->state.mouse_hovering ? RGBA(.83, .90, .98, 1) : RGBA(.92, .94, .96, 1));
    drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 4 * window->dpi, 1);
    cairo_fill(window->cr);
    b.x += 6 * window->dpi;
    b.w -= 12 * window->dpi;
    wifi_draw_text(root, b, text, 10, enabled ? RGBA(.14, .18, .24, 1) : RGBA(.53, .57, .62, 1), true);
}

// Signal strength as in the Windows network list (and winbar): the full Wi-Fi glyph
// drawn faintly, the bars for the current level on top, and the overlay lock glyph,
// which is designed to sit on the same origin as the Wi-Fi glyph.
static void wifi_paint_signal(cairo_t *cr, double x, double y, double size, int strength, bool secured,
                              double dpi, RGBA background) {
    const char *level = strength >= 75 ? "\uE701" : strength >= 50 ? "\uE874" : strength >= 25 ? "\uE873" : "\uE872";
    const RGBA color = {.14, .18, .24, 1};
    draw_text(cr, x, y, "\uE701", size, true, icon_font, -1, -1, RGBA(.14, .18, .24, .22));
    draw_text(cr, x, y, level, size, true, icon_font, -1, -1, color);
    if (!secured) return;
    // The lock overlaps the arcs; outline it in the row colour so it reads as a badge.
    auto layout = get_cached_pango_font(cr, icon_font, size, PANGO_WEIGHT_NORMAL, false);
    pango_layout_set_width(layout, -1);
    pango_layout_set_height(layout, -1);
    pango_layout_set_text(layout, "\uE889", -1);
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, x, y);
    pango_cairo_layout_path(cr, layout);
    set_argb(cr, background);
    cairo_set_line_width(cr, 3 * dpi);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke_preserve(cr);
    set_argb(cr, color);
    cairo_fill(cr);
    cairo_restore(cr);
}

std::string wifi_band(unsigned frequency) {
    if (frequency >= 5925) return "6 GHz";
    if (frequency >= 4900) return "5 GHz";
    if (frequency >= 2400) return "2.4 GHz";
    return "";
}

bool wifi_valid_password(const WifiAccessPoint &ap, const std::string &password, std::string &problem) {
    if (ap.key_mgmt == "wpa-psk") {
        bool hex = password.size() == 64 && std::all_of(password.begin(), password.end(),
            [](unsigned char c) { return std::isxdigit(c) != 0; });
        if (!hex && (password.size() < 8 || password.size() > 63)) {
            problem = "A WPA password must be 8–63 characters.";
            return false;
        }
    } else if (ap.key_mgmt == "none") {
        // NetworkManager also accepts a WEP passphrase of any length.
        if (password.empty()) {
            problem = "Enter the WEP key.";
            return false;
        }
    } else if (password.empty()) {
        problem = "Enter the network security key.";
        return false;
    }
    return true;
}

static void wifi_reset_selection(Container *root) {
    auto data = wifi_menu_data(root);
    data->password_mode = false;
    data->show_password = false;
    data->confirming_forget = false;
    data->password.clear();
    // Don't leave typed passwords (or their undo history) sitting in hidden editors.
    auto networks = static_cast<ScrollContainer *>(container_by_name("wifi_networks", root));
    for (auto row : networks->content->children)
        if (auto input = static_cast<WifiRow *>(row->user_data)->input)
            reset_text_editor(input, "");
}

static void wifi_submit_password(Container *root, WifiRow *row) {
    auto data = wifi_menu_data(root);
    std::string problem;
    if (!wifi_valid_password(row->ap, data->password, problem)) {
        wifi_command({"notice", "", problem});
        return;
    }
    wifi_command({"connect", row->ap.ssid, data->password});
    wifi_reset_selection(root);
    wifi_redraw(root);
}

static void wifi_primary_clicked(Container *root, WifiRow *row) {
    auto data = wifi_menu_data(root);
    if (data->password_mode) {
        wifi_submit_password(root, row);
    } else if (row->ap.active) {
        wifi_command({"disconnect", row->ap.ssid, ""});
    } else if (!row->ap.saved_connections.empty() || !row->ap.secured) {
        wifi_command({"connect", row->ap.ssid, ""});
    } else if (row->ap.enterprise) {
        wifi_command({"notice", "", "Enterprise (802.1X) networks need to be set up in your network settings first."});
    } else {
        data->password_mode = true;
        data->password.clear();
        reset_text_editor(row->input, "");
        // Focus the field right away, like the applications search box.
        set_active(root, {row->input}, root, true, false);
    }
    data->confirming_forget = false;
    wifi_redraw(root);
}

static std::string wifi_primary_label(WifiMenuData *data, WifiRow *row, bool *enabled) {
    const auto &view = data->view;
    *enabled = !view.busy && view.action_ssid.empty();
    if (view.connecting_ssid == row->ap.ssid) return "Connecting…";
    if (view.action_ssid == row->ap.ssid) return view.action;
    if (data->password_mode) {
        *enabled = *enabled && !data->password.empty();
        return "Connect";
    }
    return row->ap.active ? "Disconnect" : "Connect";
}

// The menu data outlives every row: both are owned by the same popup root.
static Container *wifi_network_row(Container *parent, WifiMenuData *menu) {
    auto row = parent->child(vbox, FILL_SPACE, 62);
    auto data = new WifiRow;
    row->user_data = data;
    row->receive_events_even_if_obstructed = true;
    row->when_mouse_enters_container = [data](Container *root, Container *) {
        data->hovered = true;
        wifi_redraw(root);
    };
    row->when_mouse_leaves_container = [data](Container *root, Container *) {
        data->hovered = false;
        wifi_redraw(root);
    };
    row->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        auto window = wifi_window(root);
        auto menu = wifi_menu_data(root);
        const bool selected = menu->selected == data->ap.ssid;
        const auto now = get_current_time_in_ms();
        double elapsed = std::clamp((now - data->animation_start) / 180.0, 0.0, 1.0);
        data->expansion = data->expansion_from + (data->expansion_target - data->expansion_from) *
            (1 - std::pow(1 - elapsed, 3));
        double target = selected ? 1 : 0;
        if (target != data->expansion_target) {
            data->expansion_from = data->expansion;
            data->expansion_target = target;
            data->animation_start = now;
        }
        const bool password = selected && menu->password_mode;
        data->field->exists = password && data->expansion > .001;
        data->buttons->exists = data->expansion > .001;
        data->secondary->exists = password || !data->ap.saved_connections.empty();
        c->wanted_bounds.h = (62 + ((password ? 44 : 0) + 42) * data->expansion) * window->dpi;
    };
    row->when_paint = [data](Container *root, Container *c) {
        auto window = wifi_window(root);
        auto b = c->real_bounds;
        cairo_save(window->cr);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 6 * window->dpi, 1);
        cairo_clip(window->cr);
        set_argb(window->cr, data->expansion > .01 ? RGBA(.88, .93, .99, 1) :
            data->hovered ? RGBA(.92, .94, .97, 1) : RGBA(.95, .96, .97, 1));
        cairo_paint(window->cr);
        if (std::abs(data->expansion - data->expansion_target) > .001)
            wifi_menu_data(root)->animating = true;
    };
    row->after_paint = [](Container *root, Container *) { cairo_restore(wifi_window(root)->cr); };

    auto header = wifi_row(row, 62);
    header->when_paint = [data](Container *root, Container *c) {
        auto window = wifi_window(root);
        auto dpi = window->dpi;
        auto menu = wifi_menu_data(root);
        auto b = c->real_bounds;
        RGBA background = data->expansion > .01 ? RGBA(.88, .93, .99, 1) :
            data->hovered ? RGBA(.92, .94, .97, 1) : RGBA(.95, .96, .97, 1);
        wifi_paint_signal(window->cr, b.x + 14 * dpi, b.y + 18 * dpi, 20 * dpi, data->ap.strength, data->ap.secured,
            dpi, background);
        std::string security = data->ap.security == "OWE" ? "Open (encrypted)" :
            data->ap.security.empty() ? "Open" : data->ap.security == "WEP" ? "WEP (insecure)" : data->ap.security;
        std::string subtitle;
        if (data->ap.active) {
            const auto &status = menu->view.status;
            auto device = wifi_device(status);
            subtitle = !(device && device->state == 100) ? "Connecting…" :
                status.connectivity == 2 ? "Connected, sign-in required" :
                status.connectivity == 1 || status.connectivity == 3 ? "Connected, no internet" : "Connected";
            subtitle += " · " + security;
        } else if (menu->view.connecting_ssid == data->ap.ssid) {
            subtitle = "Connecting…";
        } else {
            subtitle = (data->ap.saved_connections.empty() ? "" : "Saved · ") + security;
        }
        if (auto band = wifi_band(data->ap.frequency); !band.empty()) subtitle += " · " + band;
        // winbar always showed the BSSID; keep it for hovered or opened rows.
        if ((data->hovered || menu->selected == data->ap.ssid) && !data->ap.bssid.empty())
            subtitle += " · " + data->ap.bssid;
        wifi_draw_text(root, Bounds(b.x + 54 * dpi, b.y + 9 * dpi, b.w - 64 * dpi, 22 * dpi), data->ap.ssid);
        wifi_draw_text(root, Bounds(b.x + 54 * dpi, b.y + 31 * dpi, b.w - 64 * dpi, 22 * dpi),
            subtitle, 9, {.43, .48, .55, 1});
    };
    header->when_clicked = [data](Container *root, Container *) {
        auto menu = wifi_menu_data(root);
        menu->selected = menu->selected == data->ap.ssid ? "" : data->ap.ssid;
        wifi_reset_selection(root);
        wifi_redraw(root);
    };

    data->field = row->child(hbox, FILL_SPACE, 0);
    data->field->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        double dpi = wifi_window(root)->dpi;
        c->wanted_bounds.h = 44 * data->expansion * dpi;
        c->wanted_pad = Bounds(12 * dpi, 2 * dpi, 12 * dpi, 8 * dpi);
        c->spacing = 6 * dpi;
    };
    auto input = data->field->child(FILL_SPACE, FILL_SPACE);
    data->input = input;
    STextEditorOptions options;
    options.multiline = false;
    options.password = true;
    options.max_length = 64; // WPA allows 63 characters or 64 hex digits.
    options.font_size = 11;
    options.window = [](Container *root) { return wifi_window(root); };
    options.viewport = [](Container *root, Container *c) {
        auto dpi = wifi_window(root)->dpi;
        auto bounds = c->real_bounds;
        bounds.x += 8 * dpi;
        bounds.w = std::max(0.0, bounds.w - 16 * dpi);
        return bounds;
    };
    // Fires for typing and for asynchronous clipboard pastes alike.
    options.on_change = [menu](std::string text) { menu->password = std::move(text); };
    setup_text_editor(input, "", std::move(options));
    input->pre_layout = [](Container *root, Container *c, const Bounds &) {
        // "Show" turns masking off on the shared editor.
        static_cast<STextEditorData *>(c->user_data)->options.password = !wifi_menu_data(root)->show_password;
    };
    input->when_paint = [](Container *root, Container *c) {
        auto window = wifi_window(root);
        auto b = c->real_bounds;
        if (b.h <= 0) return;
        set_argb(window->cr, {1, 1, 1, 1});
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 4 * window->dpi, 1);
        cairo_fill_preserve(window->cr);
        set_argb(window->cr, c->active ? accent : RGBA(.8, .8, .8, 1));
        cairo_set_line_width(window->cr, window->dpi);
        cairo_stroke(window->cr);
        if (static_cast<STextEditorData *>(c->user_data)->text.empty()) {
            auto hint = b;
            hint.x += 8 * window->dpi;
            hint.w -= 16 * window->dpi;
            wifi_draw_text(root, hint, "Enter the network security key", 10, {.53, .57, .62, 1});
        }
        paint_text_editor(root, c, RGBA(.14, .18, .24, 1), RGBA(.72, .83, .95, 1));
    };
    auto edit_key = std::move(input->when_key_event);
    input->when_key_event = [edit_key, data](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym,
                                             int mods, bool is_text, std::string text) {
        auto menu = wifi_menu_data(root);
        // Key events reach every container; only the open password row reacts.
        if (!menu->password_mode || menu->selected != data->ap.ssid) return;
        if (pressed && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
            wifi_submit_password(root, data);
            return;
        }
        if (pressed && sym == XKB_KEY_Escape) {
            wifi_reset_selection(root);
            wifi_redraw(root);
            return;
        }
        edit_key(root, c, key, pressed, sym, mods, is_text, text);
        wifi_redraw(root);
    };
    auto reveal = data->field->child(56, FILL_SPACE);
    reveal->pre_layout = [](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.w = 56 * wifi_window(root)->dpi;
    };
    reveal->when_paint = [](Container *root, Container *c) {
        wifi_paint_button(root, c, wifi_menu_data(root)->show_password ? "Hide" : "Show", true);
    };
    reveal->when_clicked = [data](Container *root, Container *) {
        auto menu = wifi_menu_data(root);
        menu->show_password = !menu->show_password;
        // Clicking the button took focus from the field; hand it back.
        set_active(root, {data->input}, root, true, false);
        wifi_redraw(root);
    };

    data->buttons = row->child(hbox, FILL_SPACE, 0);
    data->buttons->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        double dpi = wifi_window(root)->dpi;
        c->wanted_bounds.h = 42 * data->expansion * dpi;
        c->wanted_pad = Bounds(12 * dpi, 0, 12 * dpi, 8 * dpi);
        c->spacing = 8 * dpi;
    };
    auto primary = data->buttons->child(FILL_SPACE, FILL_SPACE);
    primary->when_paint = [data](Container *root, Container *c) {
        bool enabled = false;
        auto label = wifi_primary_label(wifi_menu_data(root), data, &enabled);
        wifi_paint_button(root, c, label, enabled);
    };
    primary->when_clicked = [data](Container *root, Container *) {
        bool enabled = false;
        wifi_primary_label(wifi_menu_data(root), data, &enabled);
        if (enabled) wifi_primary_clicked(root, data);
    };
    data->secondary = data->buttons->child(FILL_SPACE, FILL_SPACE);
    data->secondary->when_paint = [](Container *root, Container *c) {
        auto menu = wifi_menu_data(root);
        if (menu->password_mode) {
            wifi_paint_button(root, c, "Cancel", true);
            return;
        }
        wifi_paint_button(root, c, menu->confirming_forget ? "Are you sure?" : "Forget",
            !menu->view.busy && menu->view.action_ssid.empty());
    };
    data->secondary->when_clicked = [data](Container *root, Container *) {
        auto menu = wifi_menu_data(root);
        if (menu->password_mode) {
            wifi_reset_selection(root);
        } else if (!menu->view.busy && menu->view.action_ssid.empty()) {
            if (!menu->confirming_forget) {
                menu->confirming_forget = true;
            } else {
                menu->confirming_forget = false;
                wifi_command({"forget", data->ap.ssid, ""});
            }
        }
        wifi_redraw(root);
    };
    return row;
}

// Wi-Fi state only; used for the empty network list.
std::string wifi_state_line(const WifiMenuView &view) {
    const auto &status = view.status;
    if (!view.revision) return "";
    if (!status.service_running) return "Neither NetworkManager nor wpa_supplicant is running";
    if (status.devices.empty()) return "No Wi-Fi adapter found";
    if (!status.hardware_enabled) return "Wi-Fi is disabled by a hardware switch";
    if (!status.wireless_enabled) return "Wi-Fi is off";
    if (!view.connecting_ssid.empty()) return "Connecting to " + view.connecting_ssid + "…";
    for (const auto &ap : status.access_points) {
        if (!ap.active) continue;
        if (status.connectivity == 2) return "Sign in to " + ap.ssid + " to use the internet";
        return "Connected to " + ap.ssid;
    }
    return "Not connected";
}

// Header line: like winbar's tray icon, report a wired default route first.
std::string wifi_status_line(const WifiMenuView &view) {
    auto wifi = wifi_state_line(view);
    const auto &link = view.status.link;
    if (!view.revision || !link.up || !link.wired) return wifi;
    std::string wired = "Ethernet connected (" + link.interface + ")";
    const auto &status = view.status;
    // A missing Wi-Fi service or adapter isn't worth mentioning while Ethernet works.
    if (!status.service_running || status.devices.empty() || wifi == "Not connected") return wired;
    return wired + " · " + wifi;
}

static void wifi_build(Dock *dock) {
    auto root = dock->wifi->root;
    root->type = vbox;
    auto body = root->child(vbox, FILL_SPACE, FILL_SPACE);
    body->name = "wifi_body";
    body->user_data = new WifiMenuData;
    body->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double pad = wifi_window(root)->dpi;
        c->wanted_pad = Bounds(pad, pad, pad, pad);
    };
    auto progress = wifi_row(body, 8);
    progress->when_paint = [](Container *root, Container *c) {
        const auto &view = wifi_menu_data(root)->view;
        if (!view.scanning && !view.busy) return;
        // winbar's scanning animation: five dots easing across the top edge.
        auto window = wifi_window(root);
        double phase = (get_current_time_in_ms() % 2000) / 2000.0;
        // Dots slide in from the edges; keep them inside the rounded corners.
        cairo_save(window->cr);
        clip_to_popup_background(window->cr, root->real_bounds, window->dpi);
        for (int i = 0; i < 5; ++i) {
            double step = (phase - i * .08) / .6;
            if (step < 0 || step > 1) continue;
            double x = .5 - .5 * std::cos(step * M_PI);
            set_argb(window->cr, accent);
            cairo_arc(window->cr, c->real_bounds.x + x * c->real_bounds.w,
                c->real_bounds.y + 4 * window->dpi, 2 * window->dpi, 0, 2 * M_PI);
            cairo_fill(window->cr);
        }
        cairo_restore(window->cr);
    };
    auto title = body->child(hbox, FILL_SPACE, 52);
    title->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = wifi_window(root)->dpi;
        c->wanted_bounds.h = 52 * dpi;
        // Same 16px side inset as the network cards below, so edges line up.
        c->wanted_pad = Bounds(16 * dpi, 0, 16 * dpi, 0);
        c->spacing = 12 * dpi;
    };
    auto heading = title->child(FILL_SPACE, FILL_SPACE);
    heading->when_paint = [](Container *root, Container *c) {
        auto dpi = wifi_window(root)->dpi;
        const auto &view = wifi_menu_data(root)->view;
        auto b = c->real_bounds;
        wifi_draw_text(root, Bounds(b.x, b.y + 4 * dpi, b.w, 24 * dpi), "Wi-Fi", 12, {.14, .18, .24, 1}, false, true);
        wifi_draw_text(root, Bounds(b.x, b.y + 26 * dpi, b.w, 22 * dpi), wifi_status_line(view), 9, {.43, .48, .55, 1});
    };
    // Adapter pill. winbar chose the card from a "Preferred network card" combobox;
    // with several adapters, clicking the pill cycles through them.
    auto adapter = title->child(0, FILL_SPACE);
    auto adapter_label = [](const WifiMenuView &view) {
        std::string label;
        if (auto device = wifi_device(view.status)) label = device->interface;
        if (view.status.devices.size() > 1) label += "  ▾";
        return label;
    };
    adapter->pre_layout = [adapter_label](Container *root, Container *c, const Bounds &) {
        auto window = wifi_window(root);
        auto label = adapter_label(wifi_menu_data(root)->view);
        c->exists = !label.empty();
        auto font = get_cached_pango_font(window->cr, set->font, 9 * window->dpi, PANGO_WEIGHT_NORMAL, false);
        pango_layout_set_width(font, -1);
        pango_layout_set_height(font, -1);
        pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_NONE);
        pango_layout_set_text(font, label.c_str(), -1);
        int width = 0, height = 0;
        pango_layout_get_pixel_size(font, &width, &height);
        c->wanted_bounds.w = std::min<double>(width + 20 * window->dpi, 160 * window->dpi);
    };
    adapter->when_paint = [adapter_label](Container *root, Container *c) {
        auto window = wifi_window(root);
        auto dpi = window->dpi;
        const auto &view = wifi_menu_data(root)->view;
        const bool switchable = view.status.devices.size() > 1;
        const double h = 22 * dpi;
        Bounds pill(c->real_bounds.x, c->real_bounds.y + (c->real_bounds.h - h) * .5, c->real_bounds.w, h);
        set_argb(window->cr, switchable && c->state.mouse_pressing ? RGBA(.78, .86, .96, 1) :
            switchable && c->state.mouse_hovering ? RGBA(.86, .91, .98, 1) : RGBA(.93, .95, .97, 1));
        drawRoundedRect(window->cr, pill.x, pill.y, pill.w, pill.h, h * .5, 1);
        cairo_fill(window->cr);
        pill.x += 10 * dpi;
        pill.w -= 20 * dpi;
        wifi_draw_text(root, pill, adapter_label(view), 9, {.33, .38, .45, 1}, true);
    };
    adapter->when_clicked = [](Container *root, Container *) {
        if (wifi_menu_data(root)->view.status.devices.size() > 1) wifi_command({"device", "", ""});
    };

    // Captive portal banner, shown while NetworkManager reports Connectivity == PORTAL.
    auto portal = body->child(FILL_SPACE, 0);
    portal->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = wifi_window(root)->dpi;
        c->exists = wifi_menu_data(root)->view.status.connectivity == 2;
        c->wanted_bounds.h = 46 * dpi;
        c->wanted_pad = Bounds(16 * dpi, 0, 16 * dpi, 8 * dpi);
    };
    auto sign_in = portal->child(FILL_SPACE, FILL_SPACE);
    sign_in->when_paint = [](Container *root, Container *c) {
        auto window = wifi_window(root);
        auto b = c->real_bounds;
        set_argb(window->cr, c->state.mouse_pressing ? RGBA(.72, .83, .95, 1) :
            c->state.mouse_hovering ? RGBA(.80, .88, .98, 1) : RGBA(.86, .92, .99, 1));
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 4 * window->dpi, 1);
        cairo_fill(window->cr);
        b.x += 10 * window->dpi;
        b.w -= 20 * window->dpi;
        wifi_draw_text(root, b, "This network needs you to sign in · Open sign-in page", 10, {.10, .36, .66, 1});
    };
    sign_in->when_clicked = [](Container *, Container *) { wifi_command({"sign-in", "", ""}); };

    ScrollPaneSettings settings(dock->wifi->raw_window->dpi);
    settings.bottom_show_amount = SNever;
    auto networks = make_newscrollpane_as_child(body, settings, [](Container *root) {
        auto window = wifi_window(root);
        DrawContext context;
        context.cr = window->cr;
        context.dpi = window->dpi;
        context.on_needs_frame = [window]() { windowing::redraw(window); };
        return context;
    });
    networks->name = "wifi_networks";
    auto scroll_layout = networks->pre_layout;
    networks->pre_layout = [scroll_layout](Container *root, Container *c, const Bounds &bounds) {
        if (scroll_layout) scroll_layout(root, c, bounds);
        double dpi = wifi_window(root)->dpi;
        auto content = static_cast<ScrollContainer *>(c)->content;
        content->wanted_pad = Bounds(16 * dpi, 4 * dpi, 16 * dpi, 16 * dpi);
        content->spacing = 8 * dpi;
    };
    networks->when_paint = [](Container *root, Container *c) {
        auto window = wifi_window(root);
        cairo_save(window->cr);
        cairo_rectangle(window->cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_clip(window->cr);
        if (!static_cast<ScrollContainer *>(c)->content->children.empty()) return;
        const auto &view = wifi_menu_data(root)->view;
        std::string message = wifi_state_line(view);
        if (view.status.service_running && !view.status.devices.empty() && view.status.wireless_enabled)
            message = view.scanning || view.revision <= 1 ? "Searching for networks…" : "No networks found";
        auto b = c->real_bounds;
        b.x += 30 * window->dpi;
        b.w -= 60 * window->dpi;
        wifi_draw_text(root, b, message, 10, {.43, .48, .55, 1}, true);
    };
    networks->after_paint = [](Container *root, Container *) { cairo_restore(wifi_window(root)->cr); };

    auto feedback = body->child(FILL_SPACE, 0);
    feedback->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto window = wifi_window(root);
        const auto &view = wifi_menu_data(root)->view;
        c->exists = !view.message.empty();
        auto font = get_cached_pango_font(window->cr, set->font, 10 * window->dpi, PANGO_WEIGHT_NORMAL, false);
        pango_layout_set_text(font, view.message.c_str(), -1);
        pango_layout_set_width(font, std::max(1.0, root->real_bounds.w - 24 * window->dpi) * PANGO_SCALE);
        pango_layout_set_height(font, -1);
        pango_layout_set_wrap(font, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_ellipsize(font, PANGO_ELLIPSIZE_NONE);
        int width, height;
        pango_layout_get_pixel_size(font, &width, &height);
        c->wanted_bounds.h = std::min(height + 16.0 * window->dpi, 90.0 * window->dpi);
    };
    feedback->when_paint = [](Container *root, Container *c) {
        auto window = wifi_window(root);
        const auto &view = wifi_menu_data(root)->view;
        set_argb(window->cr, view.error ? RGBA(.99, .90, .89, 1) : RGBA(.89, .95, .91, 1));
        cairo_rectangle(window->cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_fill(window->cr);
        auto b = c->real_bounds;
        b.shrink(8 * window->dpi);
        wifi_draw_text(root, b, view.message, 10, view.error ? RGBA(.68, .19, .15, 1) : RGBA(.19, .43, .28, 1));
    };

    auto footer = wifi_row(body, 50);
    footer->type = hbox;
    footer->pre_layout = [](Container *root, Container *c, const Bounds &) {
        double dpi = wifi_window(root)->dpi;
        c->wanted_bounds.h = 50 * dpi;
        c->wanted_pad = Bounds(8 * dpi, 8 * dpi, 8 * dpi, 8 * dpi);
        c->spacing = 8 * dpi;
    };
    for (bool toggle : {true, false}) {
        auto button = wifi_row(footer, 34);
        auto enabled = [toggle](Container *root) {
            const auto &view = wifi_menu_data(root)->view;
            if (!view.status.service_running || view.status.devices.empty()) return false;
            return toggle ? view.status.hardware_enabled : view.status.wireless_enabled && !view.scanning;
        };
        button->when_paint = [toggle, enabled](Container *root, Container *c) {
            const auto &view = wifi_menu_data(root)->view;
            std::string label = view.scanning ? "Scanning…" : "Refresh";
            if (toggle) label = view.status.wireless_enabled ? "Turn off Wi-Fi" : "Turn on Wi-Fi";
            wifi_paint_button(root, c, label, enabled(root));
        };
        button->when_clicked = [toggle, enabled](Container *root, Container *) {
            if (!enabled(root)) return;
            wifi_command({toggle ? "toggle" : "scan", "", ""});
        };
    }
}

static void wifi_apply(Dock *dock, const WifiMenuView &view) {
    auto root = dock->wifi->root;
    auto data = wifi_menu_data(root);
    data->view = view;
    if (!data->selected.empty() && !wifi_access_point(view.status, data->selected)) {
        data->selected.clear();
        wifi_reset_selection(root);
    }
    auto networks = static_cast<ScrollContainer *>(container_by_name("wifi_networks", root));
    auto content = networks->content;
    std::unordered_set<std::string> wanted;
    for (const auto &ap : view.status.access_points) wanted.insert(ap.ssid);
    std::unordered_map<std::string, Container *> rows;
    std::erase_if(content->children, [&wanted, &rows](Container *row) {
        if (wanted.contains(row->name)) {
            rows.emplace(row->name, row);
            return false;
        }
        delete row;
        return true;
    });
    // Rows survive refreshes (keyed by SSID) so selection, typing and animations persist.
    std::vector<Container *> ordered;
    for (const auto &ap : view.status.access_points) {
        auto found = rows.find(ap.ssid);
        Container *row = nullptr;
        if (found != rows.end()) {
            row = found->second;
        } else {
            row = wifi_network_row(content, data);
            content->children.pop_back(); // Re-inserted below in sorted order.
            row->name = ap.ssid;
        }
        static_cast<WifiRow *>(row->user_data)->ap = ap;
        ordered.push_back(row);
    }
    content->children = std::move(ordered);
}

static void wifi_watch(Dock *dock, MylarWindow *window) {
    windowing::timer(dock->app, 16, [dock, window](void *) {
        if (finished) return;
        RawWindow *popup_to_redraw = nullptr;
        {
            std::lock_guard<std::mutex> lock(dock->app->mutex);
            if (dock->wifi != window) return; // Menu closed; stop polling.
            auto menu = wifi_menu_data(window->root);
            bool redraw = false;
            auto view = wifi_latest_view.load();
            if (view && view->revision != menu->revision) {
                wifi_apply(dock, *view);
                menu->revision = view->revision;
                redraw = true;
            }
            redraw |= menu->animating || menu->view.scanning || menu->view.busy;
            menu->animating = false;
            if (redraw) popup_to_redraw = window->raw_window;
        }
        // Runs on the owning Wayland thread, so the popup can't close before painting.
        if (popup_to_redraw) windowing::redraw_now(popup_to_redraw);
        wifi_watch(dock, window);
    }, nullptr);
}

void fill_wifi_container(Dock *dock) {
    auto window = dock->wifi;
    window->root->skip_delete = true; // Root borrows the dock's UserData.
    window->root->wanted_bounds.w = FILL_SPACE;
    window->root->wanted_bounds.h = FILL_SPACE;
    window->root->when_paint = [](Container *root, Container *c) {
        auto dock = static_cast<Dock *>(root->user_data);
        auto window = dock->wifi->raw_window;
        paint_popup_background(window->cr, c->real_bounds, window->dpi);
    };
    // Mylar's raw close handler doesn't destroy roots; hook the real close so the
    // menu is freed and the worker can go back to sleep.
    auto on_close = window->raw_window->on_close;
    window->raw_window->on_close = [dock, window, on_close](RawWindow *raw) {
        if (on_close) on_close(raw);
        {
            std::lock_guard<std::mutex> lock(dock->app->mutex);
            if (dock->wifi == window) dock->wifi = nullptr;
            if (dock->window->popup_window == window) dock->window->popup_window = nullptr;
            window->root->on_closed = nullptr;
            delete window->root;
            delete window;
        }
        wifi_menu_closed();
    };
    wifi_build(dock);
    // Show the last snapshot immediately; the worker refreshes it right away.
    if (auto previous = wifi_latest_view.load()) {
        wifi_apply(dock, *previous);
        wifi_menu_data(window->root)->revision = previous->revision;
    }
    wifi_menu_opened();
    wifi_watch(dock, window);
}
