// D-Bus state belongs to the compositor main thread. The dock threads only see
// value snapshots captured by their controls and post actions back to that thread.
struct BluetoothMenuView {
    unsigned long revision = 0;
    std::vector<Adapter> adapters;
    std::vector<Device> devices;
    bool running = true;
    bool agent_ready = true;
    bool busy = false;
    bool scanning = false;
    bool scan_pending = false;
    bool scan_stopping = false;
    bool showing_paired = true;
    std::map<std::string, std::string> actions;
    bool error = false;
    std::string message;
    std::string pairing_path;
    std::string request_type;
    std::string request_device;
    std::string pin;
    unsigned long request_id = 0;
};

// The compositor publishes values only; the Wayland dock thread owns all UI work.
static std::atomic<std::shared_ptr<const BluetoothMenuView>> bluetooth_latest_view;
static unsigned long bluetooth_view_revision = 0;
static std::atomic<bool> bluetooth_available = false;
static int bluetooth_menu_count = 0;
static bool bluetooth_showing_paired = true;
static bool bluetooth_scan_stopping = false;
static unsigned long bluetooth_message_serial = 0;
static bool bluetooth_message_error = false;
static std::string bluetooth_message;
static std::map<std::string, std::string> bluetooth_actions;
static std::unordered_set<std::string> bluetooth_scans;
static std::unordered_set<std::string> bluetooth_scan_pending;
static std::unique_ptr<BluetoothRequest> bluetooth_request;
static unsigned long bluetooth_request_id = 0;

static void bluetooth_refresh();
static void bluetooth_stop_scanning();

static void bluetooth_feedback(std::string message, bool error) {
    bluetooth_message = std::move(message);
    bluetooth_message_error = error;
    auto serial = ++bluetooth_message_serial;
    later(9000, [serial](Timer *) {
        if (serial != bluetooth_message_serial) return;
        bluetooth_message.clear();
        bluetooth_refresh();
    });
}
static void bluetooth_rebuild(Dock *dock, const BluetoothMenuView &view);

static Device *bluetooth_device(const std::string &path) {
    for (auto interface : bluetooth_interfaces)
        if (interface->type == BluetoothInterfaceType::Device && interface->object_path == path)
            return static_cast<Device *>(interface);
    return nullptr;
}

static Adapter *bluetooth_adapter(const std::string &path) {
    for (auto interface : bluetooth_interfaces)
        if (interface->type == BluetoothInterfaceType::Adapter && interface->object_path == path)
            return static_cast<Adapter *>(interface);
    return nullptr;
}

static std::string bluetooth_name(const BluetoothInterface &device) {
    return !device.alias.empty() ? device.alias : !device.name.empty() ? device.name : device.mac_address;
}

static void bluetooth_reply(BluetoothRequest *request, const char *error = nullptr, const std::string &input = "") {
    if (!request || !request->message || !dbus_connection_get_is_connected(request->connection)) return;
    auto reply = error ? dbus_message_new_error(request->message, error, "Bluetooth request declined") :
        dbus_message_new_method_return(request->message);
    if (!reply) return;
    if (!error && request->type == "RequestPinCode") {
        const char *pin = input.c_str();
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
    } else if (!error && request->type == "RequestPasskey") {
        dbus_uint32_t value = std::stoul(input);
        dbus_message_append_args(reply, DBUS_TYPE_UINT32, &value, DBUS_TYPE_INVALID);
    }
    dbus_connection_send(request->connection, reply, nullptr);
    dbus_message_unref(reply);
}

static void bluetooth_clear_request(bool cancel) {
    if (cancel && bluetooth_request && bluetooth_request->type != "DisplayPinCode" &&
        bluetooth_request->type != "DisplayPasskey")
        bluetooth_reply(bluetooth_request.get(), "org.bluez.Error.Canceled");
    bluetooth_request.reset();
    ++bluetooth_request_id;
}

static void bluetooth_receive_request(BluetoothRequest *request) {
    if (request->type == "Cancelled") {
        bluetooth_reply(request);
        bluetooth_clear_request(true);
        delete request;
    } else if (!bluetooth_menu_count) {
        bluetooth_reply(request, "org.bluez.Error.Rejected");
        delete request;
    } else {
        bluetooth_clear_request(true);
        bluetooth_request.reset(request);
        // Display methods acknowledge immediately; the displayed code stays until
        // BlueZ cancels it or pairing completes. Never reply to these twice.
        if (request->type == "DisplayPinCode" || request->type == "DisplayPasskey")
            bluetooth_reply(request);
    }
    bluetooth_refresh();
}

static void bluetooth_action_result(BluetoothCallbackInfo *info) {
    auto found = bluetooth_actions.find(info->object_path);
    if (found == bluetooth_actions.end()) return;
    bool cancelling = found->second == "Cancel Pair";
    if (found->second != info->command && !(cancelling && info->command == "Pair")) return;
    if (info->command == "Cancel Pair") {
        if (!info->succeeded) {
            found->second = "Pair";
            bluetooth_feedback("Cancel failed: " + info->message, true);
            bluetooth_refresh();
        }
        // The original Pair call determines when the operation is finished.
        return;
    }
    bluetooth_actions.erase(found);
    const std::map<std::string, std::string> success = {
        {"Pair", "Device successfully paired"}, {"Connect", "Device successfully connected"},
        {"Disconnect", "Device successfully disconnected"}, {"Unpair", "Device successfully forgotten"},
        {"Cancel Pair", "Pairing cancelled"}, {"Power On", "Bluetooth enabled"}, {"Power Off", "Bluetooth disabled"}
    };
    auto message = success.find(info->command);
    bluetooth_feedback(info->succeeded ? (message != success.end() ? message->second : info->command + " completed") :
        cancelling ? "Pairing cancelled" : info->command + " failed: " + info->message, !info->succeeded && !cancelling);
    if (info->command == "Pair" || info->command == "Cancel Pair") bluetooth_clear_request(true);
    if (info->succeeded && (info->command == "Pair" || info->command == "Power Off")) {
        bluetooth_showing_paired = true;
        bluetooth_stop_scanning();
    }
    // Device/property signals already carry the result; do not enumerate the
    // whole BlueZ object tree again after every connect/forget operation.
    bluetooth_refresh();
}

static void bluetooth_scan_result(BluetoothCallbackInfo *info) {
    bluetooth_scan_pending.erase(info->object_path);
    if (info->succeeded && info->command == "Scan On") {
        bluetooth_scans.insert(info->object_path);
        if (bluetooth_menu_count) bluetooth_showing_paired = false;
        if (!bluetooth_menu_count) {
            if (auto adapter = bluetooth_adapter(info->object_path)) {
                bluetooth_scan_pending.insert(info->object_path);
                adapter->scan_off(bluetooth_scan_result);
            }
        }
    } else if (info->command == "Scan Off") {
        bluetooth_scans.erase(info->object_path);
    }
    if (!info->succeeded && bluetooth_menu_count)
        bluetooth_feedback("Discovery failed: " + info->message, true);
    if (bluetooth_scans.empty() && bluetooth_scan_pending.empty()) bluetooth_showing_paired = true;
    bluetooth_refresh();
}

static void bluetooth_stop_scanning() {
    bluetooth_scan_stopping = true;
    auto paths = bluetooth_scans;
    bluetooth_scans.clear();
    for (const auto &path : paths)
        if (auto adapter = bluetooth_adapter(path)) {
            bluetooth_scan_pending.insert(path);
            adapter->scan_off(bluetooth_scan_result);
        }
}

static void bluetooth_command(const std::string &path, const std::string &command) {
    main_thread([path, command]() {
        if (!bluetooth_menu_count || !bluetooth_running) return;
        if (command == "Scan") {
            if (!bluetooth_scan_pending.empty() || !bluetooth_actions.empty()) return;
            bluetooth_message.clear();
            if (!bluetooth_scans.empty()) {
                bluetooth_stop_scanning();
            } else {
                bluetooth_scan_stopping = false;
                for (auto interface : bluetooth_interfaces) {
                    if (interface->type != BluetoothInterfaceType::Adapter) continue;
                    auto adapter = static_cast<Adapter *>(interface);
                    if (!adapter->powered) continue;
                    bluetooth_scan_pending.insert(adapter->object_path);
                    adapter->scan_on(bluetooth_scan_result);
                }
            }
        } else if (command == "Cancel Pair") {
            auto action = bluetooth_actions.find(path);
            if (action == bluetooth_actions.end() || action->second != "Pair") return;
            bluetooth_clear_request(true);
            bluetooth_actions[path] = "Cancel Pair";
            if (auto device = bluetooth_device(path)) device->cancel_pair(bluetooth_action_result);
        } else {
            if (!bluetooth_actions.empty()) return;
            bluetooth_message_error = false;
            bluetooth_message.clear();
            if (command == "Power On" || command == "Power Off") {
                if (command == "Power Off") bluetooth_stop_scanning();
                for (auto interface : bluetooth_interfaces) {
                    if (interface->type != BluetoothInterfaceType::Adapter) continue;
                    auto adapter = static_cast<Adapter *>(interface);
                    if (adapter->powered == (command == "Power On")) continue;
                    bluetooth_actions[adapter->object_path] = command;
                    if (command == "Power On") adapter->power_on(bluetooth_action_result);
                    else adapter->power_off(bluetooth_action_result);
                }
            } else if (auto device = bluetooth_device(path)) {
                bluetooth_actions[path] = command;
                if (command == "Pair") device->pair(bluetooth_action_result);
                else if (command == "Connect") device->connect(bluetooth_action_result);
                else if (command == "Disconnect") device->disconnect(bluetooth_action_result);
                else if (command == "Unpair") device->unpair(bluetooth_action_result);
            }
        }
        bluetooth_refresh();
    });
}

static void bluetooth_submit(unsigned long id, bool accept, std::string input) {
    main_thread([id, accept, input]() {
        if (!bluetooth_request || id != bluetooth_request_id) return;
        auto type = bluetooth_request->type;
        if (accept && (type == "RequestPinCode" || type == "RequestPasskey")) {
            bool valid = !input.empty() && input.size() <= (type == "RequestPinCode" ? 16 : 6);
            if (type == "RequestPasskey")
                valid &= std::all_of(input.begin(), input.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
            if (!valid) {
                bluetooth_feedback(type == "RequestPinCode" ? "Enter a PIN of 1–16 characters." : "Enter 1–6 digits.", true);
                bluetooth_refresh();
                return;
            }
        }
        bluetooth_reply(bluetooth_request.get(), accept ? nullptr : "org.bluez.Error.Rejected", input);
        bluetooth_clear_request(false);
        bluetooth_refresh();
    });
}

static void bluetooth_refresh() {
    // Rate-limit discovery bursts. Never wait for rendering or create UI nodes
    // on the compositor thread.
    static bool queued = false;
    if (queued) return;
    queued = true;
    later(50, [](Timer *) {
        queued = false;
        if (finished || !bluetooth_menu_count) return;
        if (!bluetooth_running) {
            bluetooth_clear_request(true);
            bluetooth_scans.clear();
            bluetooth_scan_pending.clear();
            bluetooth_actions.clear();
        }
        std::erase_if(bluetooth_scans, [](const std::string &path) {
            auto adapter = bluetooth_adapter(path);
            return !adapter || !adapter->powered;
        });
        BluetoothMenuView view;
        view.revision = ++bluetooth_view_revision;
        view.running = bluetooth_running;
        view.agent_ready = dbus_bluetooth_agent_ready();
        for (const auto &[path, command] : bluetooth_actions)
            if (command == "Pair" || command == "Cancel Pair") view.pairing_path = path;
        view.busy = !bluetooth_actions.empty();
        view.scanning = !bluetooth_scans.empty() || !bluetooth_scan_pending.empty();
        view.scan_pending = !bluetooth_scan_pending.empty();
        view.scan_stopping = bluetooth_scan_stopping;
        view.showing_paired = bluetooth_showing_paired;
        view.actions = bluetooth_actions;
        view.message = bluetooth_message;
        view.error = bluetooth_message_error;
        if (view.running) {
            for (auto interface : bluetooth_interfaces) {
                if (interface->type == BluetoothInterfaceType::Adapter)
                    view.adapters.push_back(*static_cast<Adapter *>(interface));
                else if (interface->type == BluetoothInterfaceType::Device)
                    view.devices.push_back(*static_cast<Device *>(interface));
            }
        }
        std::sort(view.devices.begin(), view.devices.end(), [](const Device &a, const Device &b) {
            if (a.connected != b.connected) return a.connected;
            if (a.paired != b.paired) return a.paired;
            return bluetooth_name(a) < bluetooth_name(b);
        });
        if (bluetooth_request) {
            view.request_type = bluetooth_request->type;
            view.pin = bluetooth_request->pin;
            view.request_device = bluetooth_request->object_path;
            if (auto device = bluetooth_device(view.request_device)) view.request_device = bluetooth_name(*device);
            view.request_id = bluetooth_request_id;
        }
        bluetooth_latest_view.store(std::make_shared<const BluetoothMenuView>(std::move(view)));
    });
}

void dock::change_in_bluetooth() {
    bluetooth_available = bluetooth_running;
    if (!bluetooth_running) {
        bluetooth_clear_request(true);
        bluetooth_scans.clear();
        bluetooth_scan_pending.clear();
        bluetooth_actions.clear();
        bluetooth_showing_paired = true;
    }
    bluetooth_refresh();
}

struct BluetoothMenuData : UserData {
    BluetoothMenuView view;
    bool animating = false;
    std::map<std::pair<std::string, int>, std::shared_ptr<cairo_surface_t>> icons;
};

struct BluetoothDeviceRow : UserData {
    Device device{""};
    std::string action;
    bool enabled = false;
    bool pairing_allowed = false;
    bool busy = false;
    bool hovered = false;
    bool confirming_forget = false;
    double expansion = 0;
    double expansion_from = 0;
    double expansion_target = 0;
    long animation_start = 0;
    std::shared_ptr<cairo_surface_t> icon;
    std::string icon_name;
    double icon_dpi = 0;
};

static BluetoothMenuData *bluetooth_menu_data(Container *root) {
    return static_cast<BluetoothMenuData *>(container_by_name("bluetooth_body", root)->user_data);
}

static RawWindow *bluetooth_window(Container *root) {
    return static_cast<Dock *>(root->user_data)->bluetooth->raw_window;
}

static Container *bluetooth_row(Container *parent, double height) {
    auto row = parent->child(FILL_SPACE, height);
    row->pre_layout = [height](Container *root, Container *c, const Bounds &) {
        c->wanted_bounds.h = height * bluetooth_window(root)->dpi;
    };
    return row;
}

static void bluetooth_draw_text(Container *root, Bounds b, const std::string &text, double size = 10,
                                RGBA color = {.14, .18, .24, 1}, bool centered = false, bool bold = false) {
    auto window = bluetooth_window(root);
    auto font = get_cached_pango_font(window->cr, mylar_font, size * window->dpi,
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

static Container *bluetooth_label(Container *parent, std::string text, double height = 28,
                                   bool centered = false, double size = 10) {
    auto row = bluetooth_row(parent, height);
    row->when_paint = [text, centered, size](Container *root, Container *c) {
        auto b = c->real_bounds;
        b.x += 12 * bluetooth_window(root)->dpi;
        b.w -= 24 * bluetooth_window(root)->dpi;
        bluetooth_draw_text(root, b, text, size, {.14, .18, .24, 1}, centered);
    };
    return row;
}

static void bluetooth_paint_button(Container *root, Container *c, const std::string &text, bool enabled) {
    auto window = bluetooth_window(root);
    auto b = c->real_bounds;
    if (b.h <= 0) return;
    set_argb(window->cr, !enabled ? RGBA(.95, .96, .97, 1) : c->state.mouse_pressing ?
        RGBA(.72, .83, .95, 1) : c->state.mouse_hovering ? RGBA(.83, .90, .98, 1) : RGBA(.92, .94, .96, 1));
    drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 4 * window->dpi, 1);
    cairo_fill(window->cr);
    b.x += 6 * window->dpi;
    b.w -= 12 * window->dpi;
    bluetooth_draw_text(root, b, text, 10, enabled ? RGBA(.14, .18, .24, 1) : RGBA(.53, .57, .62, 1), true);
}

static Container *bluetooth_button(Container *parent, std::string text, bool enabled,
                                   std::function<void(Container *)> action) {
    auto row = bluetooth_row(parent, 34);
    row->when_paint = [text, enabled](Container *root, Container *c) { bluetooth_paint_button(root, c, text, enabled); };
    row->when_clicked = [enabled, action](Container *root, Container *) { if (enabled) action(root); };
    return row;
}

static std::string bluetooth_action_label(const std::string &action) {
    if (action == "Connect") return "Connecting…";
    if (action == "Disconnect") return "Disconnecting…";
    if (action == "Unpair") return "Forgetting…";
    if (action == "Cancel Pair") return "Cancelling…";
    return action;
}

static Container *bluetooth_device_row(Container *parent) {
    auto row = parent->child(vbox, FILL_SPACE, 62);
    auto data = new BluetoothDeviceRow;
    row->user_data = data;
    row->receive_events_even_if_obstructed = true;
    row->when_mouse_enters_container = [data](Container *root, Container *) {
        data->hovered = true;
        windowing::redraw(bluetooth_window(root));
    };
    row->when_mouse_leaves_container = [data](Container *root, Container *) {
        data->hovered = false;
        data->confirming_forget = false;
        windowing::redraw(bluetooth_window(root));
    };
    row->pre_layout = [data](Container *root, Container *c, const Bounds &) {
        auto window = bluetooth_window(root);
        const auto now = get_current_time_in_ms();
        double elapsed = std::clamp((now - data->animation_start) / 180.0, 0.0, 1.0);
        data->expansion = data->expansion_from + (data->expansion_target - data->expansion_from) *
            (1 - std::pow(1 - elapsed, 3));
        double target = data->hovered || !data->action.empty() ? 1 : 0;
        if (target != data->expansion_target) {
            data->expansion_from = data->expansion;
            data->expansion_target = target;
            data->animation_start = now;
        }
        c->wanted_bounds.h = (62 + (data->device.paired ? 68 : 34) * data->expansion) * window->dpi;
    };
    row->when_paint = [data](Container *root, Container *c) {
        auto window = bluetooth_window(root);
        auto b = c->real_bounds;
        cairo_save(window->cr);
        drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 6 * window->dpi, 1);
        cairo_clip(window->cr);
        set_argb(window->cr, data->expansion > .01 ? RGBA(.88, .93, .99, 1) : RGBA(.95, .96, .97, 1));
        cairo_paint(window->cr);
        if (std::abs(data->expansion - data->expansion_target) > .001)
            bluetooth_menu_data(root)->animating = true;
    };
    row->after_paint = [](Container *root, Container *) { cairo_restore(bluetooth_window(root)->cr); };
    auto header = bluetooth_row(row, 62);
    header->when_paint = [data](Container *root, Container *c) {
        auto window = bluetooth_window(root);
        auto dpi = window->dpi;
        if (icons_loaded && (data->icon_name != data->device.icon || data->icon_dpi != dpi)) {
            data->icon_name = data->device.icon;
            data->icon_dpi = dpi;
            auto &cache = bluetooth_menu_data(root)->icons;
            auto [entry, inserted] = cache.try_emplace({data->device.icon, int(24 * dpi)});
            if (inserted) {
                cairo_surface_t *surface = nullptr;
                auto path = one_shot_icon(24 * dpi, {data->device.icon, "bluetooth"});
                if (!path.empty()) load_icon_full_path(&surface, path, 24 * dpi);
                if (surface) entry->second = std::shared_ptr<cairo_surface_t>(surface, cairo_surface_destroy);
            }
            data->icon = entry->second;
        }
        auto b = c->real_bounds;
        if (data->icon) {
            cairo_set_source_surface(window->cr, data->icon.get(), b.x + 10 * dpi, b.y + 19 * dpi);
            cairo_paint(window->cr);
        } else {
            draw_text(window->cr, b.x + 10 * dpi, b.y + 19 * dpi, "\uE702", 20 * dpi, true,
                "Segoe Fluent Icons", -1, -1, accent);
        }
        std::string subtitle = data->device.connected ? "Connected" : "Paired";
        if (!data->device.paired) subtitle = data->device.mac_address;
        else if (data->device.connected && !data->device.percentage.empty())
            subtitle += ", Battery " + data->device.percentage + "%";
        if (data->device.paired && data->hovered) subtitle += " · " + data->device.mac_address;
        bluetooth_draw_text(root, Bounds(b.x + 54 * dpi, b.y + 9 * dpi, b.w - 64 * dpi, 22 * dpi),
            bluetooth_name(data->device));
        bluetooth_draw_text(root, Bounds(b.x + 54 * dpi, b.y + 31 * dpi, b.w - 64 * dpi, 22 * dpi),
            subtitle, 9, {.43, .48, .55, 1});
    };
    for (bool forget : {false, true}) {
        auto action = row->child(FILL_SPACE, 0);
        action->pre_layout = [data, forget](Container *root, Container *c, const Bounds &) {
            c->exists = (!forget || data->device.paired) && data->expansion > .001;
            c->wanted_bounds.h = 34 * data->expansion * bluetooth_window(root)->dpi;
        };
        auto enabled = [data, forget]() {
            if (forget) return data->device.paired && !data->busy;
            if (data->action == "Pair") return true;
            return data->enabled && !data->busy && (data->device.paired || data->pairing_allowed);
        };
        action->when_paint = [data, forget, enabled](Container *root, Container *c) {
            std::string label;
            if (forget) label = data->action == "Unpair" ? "Forgetting…" : data->confirming_forget ? "Are you sure?" : "Forget";
            else if (data->action == "Pair") label = "Cancel";
            else if (!data->action.empty() && data->action != "Unpair") label = bluetooth_action_label(data->action);
            else label = data->device.paired ? data->device.connected ? "Disconnect" : "Connect" : "Pair";
            bluetooth_paint_button(root, c, label, enabled());
        };
        action->when_clicked = [data, forget, enabled](Container *root, Container *) {
            if (!enabled()) return;
            if (forget && !data->confirming_forget) {
                data->confirming_forget = true;
                windowing::redraw(bluetooth_window(root));
                return;
            }
            std::string command = forget ? "Unpair" : data->action == "Pair" ? "Cancel Pair" :
                data->device.paired ? data->device.connected ? "Disconnect" : "Connect" : "Pair";
            data->confirming_forget = false;
            bluetooth_command(data->device.object_path, command);
        };
    }
    return row;
}

static void bluetooth_rebuild(Dock *dock, const BluetoothMenuView &view) {
    auto root = dock->bluetooth->root;
    auto body = container_by_name("bluetooth_body", root);
    if (!body) {
        root->type = vbox;
        body = root->child(vbox, FILL_SPACE, FILL_SPACE);
        body->pre_layout = [](Container *root, Container *c, const Bounds &) {
            double pad = bluetooth_window(root)->dpi;
            c->wanted_pad = Bounds(pad, pad, pad, pad);
        };
        body->name = "bluetooth_body";
        body->user_data = new BluetoothMenuData;
        auto progress = bluetooth_row(body, 8);
        progress->when_paint = [](Container *root, Container *c) {
            const auto &view = bluetooth_menu_data(root)->view;
            if (!view.scanning && !view.busy) return;
            auto window = bluetooth_window(root);
            double phase = (get_current_time_in_ms() % 2000) / 2000.0;
            for (int i = 0; i < 5; ++i) {
                double step = (phase - i * .08) / .6;
                if (step < 0 || step > 1) continue;
                double x = .5 - .5 * std::cos(step * M_PI);
                set_argb(window->cr, accent);
                cairo_arc(window->cr, c->real_bounds.x + x * c->real_bounds.w,
                    c->real_bounds.y + 4 * window->dpi, 2 * window->dpi, 0, 2 * M_PI);
                cairo_fill(window->cr);
            }
        };
        ScrollPaneSettings settings(dock->bluetooth->raw_window->dpi);
        settings.bottom_show_amount = SNever;
        auto devices = make_newscrollpane_as_child(body, settings, [](Container *root) {
            auto window = bluetooth_window(root);
            DrawContext context;
            context.cr = window->cr;
            context.dpi = window->dpi;
            context.on_needs_frame = [window]() { windowing::redraw(window); };
            return context;
        });
        devices->name = "bluetooth_devices";
        auto scroll_layout = devices->pre_layout;
        devices->pre_layout = [scroll_layout](Container *root, Container *c, const Bounds &bounds) {
            if (scroll_layout) scroll_layout(root, c, bounds);
            double dpi = bluetooth_window(root)->dpi;
            auto content = static_cast<ScrollContainer *>(c)->content;
            content->wanted_pad = Bounds(16 * dpi, 8 * dpi, 16 * dpi, 16 * dpi);
            content->spacing = 8 * dpi;
        };
        devices->when_paint = [](Container *root, Container *c) {
            auto window = bluetooth_window(root);
            cairo_save(window->cr);
            cairo_rectangle(window->cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
            cairo_clip(window->cr);
            if (!static_cast<ScrollContainer *>(c)->content->children.empty()) return;
            const auto &view = bluetooth_menu_data(root)->view;
            bool powered = std::any_of(view.adapters.begin(), view.adapters.end(), [](const Adapter &a) { return a.powered; });
            std::string message = !view.running ? "Bluetooth service is unavailable" : view.adapters.empty() ?
                "No Bluetooth adapter found" : !powered ? "Bluetooth is disabled" :
                view.showing_paired ? "No paired devices" : "Searching for devices…";
            if (!view.running)
                message = "";
            auto b = c->real_bounds;
            b.x += 30 * window->dpi;
            b.w -= 60 * window->dpi;
            bluetooth_draw_text(root, b, message, 10, {.43, .48, .55, 1}, true);
        };
        devices->after_paint = [](Container *root, Container *) { cairo_restore(bluetooth_window(root)->cr); };
        auto feedback = body->child(FILL_SPACE, 0);
        feedback->name = "bluetooth_feedback";
        feedback->pre_layout = [](Container *root, Container *c, const Bounds &) {
            auto window = bluetooth_window(root);
            const auto &view = bluetooth_menu_data(root)->view;
            c->exists = !view.message.empty();
            auto font = get_cached_pango_font(window->cr, mylar_font, 10 * window->dpi, PANGO_WEIGHT_NORMAL, false);
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
            auto window = bluetooth_window(root);
            const auto &view = bluetooth_menu_data(root)->view;
            set_argb(window->cr, view.error ? RGBA(.99, .90, .89, 1) : RGBA(.89, .95, .91, 1));
            cairo_rectangle(window->cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
            cairo_fill(window->cr);
            auto b = c->real_bounds;
            b.shrink(8 * window->dpi);
            bluetooth_draw_text(root, b, view.message, 10, view.error ? RGBA(.68, .19, .15, 1) : RGBA(.19, .43, .28, 1));
        };
        auto prompt = body->child(vbox, FILL_SPACE, 0);
        prompt->name = "bluetooth_input_bar";
        prompt->pre_layout = [](Container *root, Container *c, const Bounds &bounds) {
            for (auto child : c->children)
                if (child->pre_layout) child->pre_layout(root, child, bounds);
            c->wanted_bounds.h = reserved_height(c);
        };
        prompt->when_paint = [](Container *root, Container *c) {
            auto window = bluetooth_window(root);
            set_argb(window->cr, {.89, .94, .99, 1});
            cairo_rectangle(window->cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
            cairo_fill(window->cr);
        };
        auto footer = bluetooth_row(body, 50);
        footer->name = "bluetooth_footer";
        footer->type = hbox;
        footer->pre_layout = [](Container *root, Container *c, const Bounds &) {
            double dpi = bluetooth_window(root)->dpi;
            c->wanted_bounds.h = 50 * dpi;
            c->wanted_pad = Bounds(8 * dpi, 8 * dpi, 8 * dpi, 8 * dpi);
            c->spacing = 8 * dpi;
        };
        for (bool power : {false, true}) {
            auto button = bluetooth_row(footer, 34);
            auto enabled = [power](Container *root) {
                const auto &view = bluetooth_menu_data(root)->view;
                bool powered = std::any_of(view.adapters.begin(), view.adapters.end(), [](const Adapter &a) { return a.powered; });
                return power ? !view.adapters.empty() && !view.busy && !view.scanning :
                    powered && !view.busy && !view.scan_pending;
            };
            button->when_paint = [power, enabled](Container *root, Container *c) {
                const auto &view = bluetooth_menu_data(root)->view;
                bool powered = std::any_of(view.adapters.begin(), view.adapters.end(), [](const Adapter &a) { return a.powered; });
                std::string label = view.scan_pending ? (view.scan_stopping ? "Cancelling…" : "Starting Scan…") :
                    view.showing_paired ? "Add Device" : "Cancel";
                if (power) {
                    label = powered ? "Disable Bluetooth" : "Enable Bluetooth";
                    for (const auto &[path, action] : view.actions) {
                        if (action == "Power On") label = "Enabling…";
                        if (action == "Power Off") label = "Disabling…";
                    }
                }
                bluetooth_paint_button(root, c, label, enabled(root));
            };
            button->when_clicked = [power, enabled](Container *root, Container *) {
                if (!enabled(root)) return;
                const auto &view = bluetooth_menu_data(root)->view;
                bool powered = std::any_of(view.adapters.begin(), view.adapters.end(), [](const Adapter &a) { return a.powered; });
                bluetooth_command("", power ? powered ? "Power Off" : "Power On" : "Scan");
            };
        }
    }
    auto data = static_cast<BluetoothMenuData *>(body->user_data);
    bool mode_changed = data->view.showing_paired != view.showing_paired;
    data->view = view;
    auto devices = static_cast<ScrollContainer *>(container_by_name("bluetooth_devices", root));
    if (mode_changed) devices->scroll_v_real = devices->scroll_v_visual = 0;
    std::vector<Device> wanted;
    for (const auto &device : view.devices) {
        bool powered = std::any_of(view.adapters.begin(), view.adapters.end(), [&device](const Adapter &adapter) {
            return adapter.object_path == device.adapter && adapter.powered;
        });
        if (powered && (device.paired == view.showing_paired || view.pairing_path == device.object_path)) wanted.push_back(device);
    }
    std::unordered_set<std::string> wanted_paths;
    for (const auto &device : wanted) wanted_paths.insert(device.object_path);
    std::unordered_map<std::string, Container *> rows;
    std::erase_if(devices->content->children, [&wanted_paths, &rows](Container *row) {
        if (wanted_paths.contains(row->name)) {
            rows.emplace(row->name, row);
            return false;
        }
        delete row;
        return true;
    });
    // Retain rows during property updates so hovering, confirmation and animations
    // survive RSSI/battery signals and asynchronous method replies.
    for (const auto &device : wanted) {
        auto found = rows.find(device.object_path);
        auto row = found != rows.end() ? found->second : nullptr;
        if (!row) {
            row = bluetooth_device_row(devices->content);
            row->name = device.object_path;
        }
        auto option = static_cast<BluetoothDeviceRow *>(row->user_data);
        option->device = device;
        auto action = view.actions.find(device.object_path);
        option->action = action == view.actions.end() ? "" : action->second;
        option->enabled = true;
        option->busy = view.busy;
        option->pairing_allowed = view.agent_ready;
    }
    auto prompt = container_by_name("bluetooth_input_bar", root);
    auto prompt_key = std::to_string(view.request_id) + view.request_type;
    prompt->exists = !view.request_type.empty();
    if (dock->bluetooth_prompt == prompt_key) return;
    dock->bluetooth_prompt = prompt_key;
    dock->bluetooth_input.clear();
    for (auto child : prompt->children) delete child;
    prompt->children.clear();
    if (!prompt->exists) return;
    const bool input = view.request_type == "RequestPinCode" || view.request_type == "RequestPasskey";
    const bool display = view.request_type == "DisplayPinCode" || view.request_type == "DisplayPasskey";
    bluetooth_label(prompt, view.request_device, 28);
    std::string title = input ? (view.request_type == "RequestPinCode" ? "Enter PIN:" : "Enter Passkey:") :
        display ? "Type this code on the device, then Enter:" : view.request_type == "RequestConfirmation" ?
        "Does Passkey match?" : view.request_type == "AuthorizeService" ? "Allow this device to connect?" : "Allow this device to pair?";
    bluetooth_label(prompt, title, 28);
    if (!view.pin.empty()) bluetooth_label(prompt, view.pin, 37, true, 16);
    if (input) {
        auto field = bluetooth_row(prompt, 46);
        field->when_paint = [](Container *root, Container *c) {
            auto dock = static_cast<Dock *>(root->user_data);
            auto window = bluetooth_window(root);
            auto b = c->real_bounds;
            b.x += 13 * window->dpi;
            b.w -= 26 * window->dpi;
            b.y += 5 * window->dpi;
            b.h -= 10 * window->dpi;
            set_argb(window->cr, {1, 1, 1, 1});
            drawRoundedRect(window->cr, b.x, b.y, b.w, b.h, 4 * window->dpi, 1);
            cairo_fill_preserve(window->cr);
            set_argb(window->cr, accent);
            cairo_set_line_width(window->cr, window->dpi);
            cairo_stroke(window->cr);
            b.shrink(6 * window->dpi);
            bluetooth_draw_text(root, b, dock->bluetooth_input.empty() ? "Type here…" : dock->bluetooth_input + "|", 11);
        };
        field->when_key_event = [id = view.request_id, numeric = view.request_type == "RequestPasskey"]
            (Container *root, Container *, int, bool pressed, xkb_keysym_t sym, int, bool is_text, std::string text) {
            if (!pressed) return;
            auto dock = static_cast<Dock *>(root->user_data);
            auto &value = dock->bluetooth_input;
            if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) bluetooth_submit(id, true, value);
            else if (sym == XKB_KEY_BackSpace && !value.empty()) {
                auto pos = value.size() - 1;
                while (pos && (static_cast<unsigned char>(value[pos]) & 0xc0) == 0x80) --pos;
                value.erase(pos);
            } else if (is_text && value.size() + text.size() <= (numeric ? 6 : 16) &&
                std::all_of(text.begin(), text.end(), [numeric](unsigned char c) {
                    return numeric ? c >= '0' && c <= '9' : c >= 32 && c != 127;
                })) value += text;
            windowing::redraw(bluetooth_window(root));
        };
    }
    if (!display) {
        bluetooth_button(prompt, input ? "Submit" : view.request_type == "RequestConfirmation" ? "Matches" : "Allow", true,
            [id = view.request_id](Container *root) { bluetooth_submit(id, true, static_cast<Dock *>(root->user_data)->bluetooth_input); });
        bluetooth_button(prompt, input ? "Cancel" : "Reject", true,
            [id = view.request_id](Container *) { bluetooth_submit(id, false, ""); });
    }
}

static void bluetooth_watch_dock(Dock *dock) {
    windowing::timer(dock->app, dock->bluetooth ? 16 : 100, [](void *data) {
        auto dock = static_cast<Dock *>(data);
        if (finished) return;
        RawWindow *popup_to_redraw = nullptr;
        {
            std::lock_guard<std::mutex> lock(dock->app->mutex);
            const bool available = bluetooth_available.load();
            auto update_icon = [available](MylarWindow *window) {
                if (!window || !windowing::has_window(window->raw_window)) return;
                if (auto item = container_by_name("bluetooth", window->root)) {
                    if (item->exists != available) {
                        item->exists = available;
                        windowing::redraw(window->raw_window);
                    }
                }
            };
            update_icon(dock->window);
            update_icon(dock->extra);
            if (dock->bluetooth) {
                bool redraw = false;
                auto view = bluetooth_latest_view.load();
                if (view && view->revision != dock->bluetooth_revision) {
                    bluetooth_rebuild(dock, *view);
                    dock->bluetooth_revision = view->revision;
                    redraw = true;
                }
                auto menu = bluetooth_menu_data(dock->bluetooth->root);
                redraw |= menu->animating || menu->view.scanning || menu->view.busy;
                menu->animating = false;
                if (redraw) popup_to_redraw = dock->bluetooth->raw_window;
            }
        }
        // on_render takes the app mutex itself. This timer runs on the owning
        // Wayland thread, so the popup cannot close between unlock and paint.
        if (popup_to_redraw) windowing::redraw_now(popup_to_redraw);
        bluetooth_watch_dock(dock);
    }, dock);
}

static void fill_bluetooth_container(Dock *dock) {
    auto window = dock->bluetooth;
    dock->bluetooth_prompt.clear();
    dock->bluetooth_input.clear();
    auto previous = bluetooth_latest_view.load();
    dock->bluetooth_revision = previous ? previous->revision : 0;
    window->root->skip_delete = true; // Root borrows the dock's UserData.
    window->root->wanted_bounds.w = FILL_SPACE;
    window->root->wanted_bounds.h = FILL_SPACE;
    window->root->when_paint = [](Container *root, Container *c) {
        auto dock = static_cast<Dock *>(root->user_data);
        auto window = dock->bluetooth->raw_window;
        paint_popup_background(window->cr, c->real_bounds, window->dpi);
    };
    // Mylar's raw close handler currently does not destroy roots, so hook the
    // actual window close for discovery/agent cleanup as well as popup ownership.
    auto on_close = window->raw_window->on_close;
    window->raw_window->on_close = [dock, window, on_close](RawWindow *raw) {
        if (on_close) on_close(raw);
        {
            std::lock_guard<std::mutex> lock(dock->app->mutex);
            if (dock->bluetooth == window) dock->bluetooth = nullptr;
            if (dock->window->popup_window == window) dock->window->popup_window = nullptr;
            window->root->on_closed = nullptr;
            delete window->root;
            delete window;
        }
        main_thread([]() {
            if (bluetooth_menu_count > 0) --bluetooth_menu_count;
            if (bluetooth_menu_count) return;
            bluetooth_clear_request(true);
            for (const auto &[path, command] : bluetooth_actions)
                if (command == "Pair")
                    if (auto device = bluetooth_device(path)) device->cancel_pair(nullptr);
            bluetooth_stop_scanning();
        });
    };
    BluetoothMenuView loading;
    bluetooth_rebuild(dock, loading);
    main_thread([]() {
        if (finished) return;
        if (!bluetooth_menu_count) bluetooth_showing_paired = true;
        ++bluetooth_menu_count;
        on_any_bluetooth_property_changed = bluetooth_refresh;
        on_bluetooth_request = bluetooth_receive_request;
        bluetooth_message.clear();
        ++bluetooth_message_serial;
        bluetooth_message_error = !dbus_connection_system;
        if (bluetooth_message_error) bluetooth_message = "System D-Bus is unavailable.";
        if (bluetooth_running) {
            register_agent_if_needed();
            if (!dbus_bluetooth_agent_ready()) {
                bluetooth_message_error = true;
                bluetooth_message = "Could not register the Bluetooth pairing agent.";
            }
            update_devices();
        }
        bluetooth_refresh();
    });
}
