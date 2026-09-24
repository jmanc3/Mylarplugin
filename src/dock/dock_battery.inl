struct BatterySample {
    std::time_t timestamp;
    double percentage;
};

struct BatteryView {
    BatteryStatus status;
    std::vector<BatterySample> history;
    std::time_t updated = 0;
    double percent_per_hour = 0;
    double remaining_seconds = 0;
    bool pending = false;
    bool history_enabled = true;
    std::string action_error;
    std::string history_error;
};

static std::mutex battery_mutex;
static std::condition_variable battery_wakeup;
static std::thread battery_thread;
static BatteryView battery_view;
static bool watching_battery = false;
static bool battery_refresh_requested = false;
// Positive enables, negative disables; 1 = saver, 2 = protector.
static int battery_action = 0;

static BatteryView battery_snapshot() {
    std::lock_guard<std::mutex> lock(battery_mutex);
    return battery_view;
}

static BatteryStatus battery_status_snapshot() {
    std::lock_guard<std::mutex> lock(battery_mutex);
    return battery_view.status;
}

static bool has_internal_battery() {
    std::error_code error;
    std::filesystem::directory_iterator entries("/sys/class/power_supply", error);
    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        const auto &entry = *entries;
        std::string type, scope, present;
        std::ifstream(entry.path() / "type") >> type;
        std::ifstream(entry.path() / "scope") >> scope;
        std::ifstream(entry.path() / "present") >> present;
        if (type == "Battery" && scope != "Device" && present != "0") return true;
    }
    return false;
}

static std::filesystem::path battery_history_path() {
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config/mylar/battery-history.tsv";
    return {};
}

static std::vector<BatterySample> load_battery_history(const std::filesystem::path &path) {
    std::vector<BatterySample> samples;
    std::ifstream file(path);
    file.imbue(std::locale::classic());
    const auto now = std::time(nullptr);
    std::string line;
    // Ignore malformed, duplicate, future and expired records independently.
    while (std::getline(file, line)) {
        std::istringstream record(line);
        record.imbue(std::locale::classic());
        BatterySample sample{};
        std::string extra;
        if (!(record >> sample.timestamp >> sample.percentage) || (record >> extra)) continue;
        if (sample.timestamp <= now - 86400 || sample.timestamp > now || sample.timestamp % 300 != 0 ||
            !std::isfinite(sample.percentage) || sample.percentage < 0 || sample.percentage > 100) continue;
        if (!samples.empty() && sample.timestamp <= samples.back().timestamp) continue;
        samples.push_back(sample);
    }
    return samples;
}

static bool save_battery_history(const std::filesystem::path &path, const std::vector<BatterySample> &samples) {
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::trunc);
    file.imbue(std::locale::classic());
    file << std::fixed << std::setprecision(2);
    for (const auto &sample : samples)
        file << sample.timestamp << '\t' << sample.percentage << '\n';
    file.close();
    if (!file) return false;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

static void request_battery_refresh() {
    {
        std::lock_guard<std::mutex> lock(battery_mutex);
        battery_refresh_requested = true;
    }
    battery_wakeup.notify_all();
}

static void toggle_battery_history() {
    main_thread([]() {
        {
            std::lock_guard<std::mutex> lock(battery_mutex);
            battery_view.history_enabled = !battery_view.history_enabled;
            set->battery_history_enabled = battery_view.history_enabled;
            if (!battery_view.history_enabled) battery_view.history_error.clear();
            battery_refresh_requested = true;
        }
        settings::load_save_settings(true, set);
        battery_wakeup.notify_all();
        dock::change_in_battery();
    });
}

static void request_battery_action(bool protector) {
    {
        std::lock_guard<std::mutex> lock(battery_mutex);
        auto &s = battery_view.status;
        if (battery_view.pending || !(protector ? s.protector_available : s.saver_available)) return;
        battery_action = (protector ? 2 : 1) * ((protector ? s.protector_enabled : s.battery_saver) ? -1 : 1);
        battery_view.pending = true;
        battery_view.action_error.clear();
    }
    battery_wakeup.notify_all();
    dock::change_in_battery();
}

static void watch_battery_level() {
    std::lock_guard<std::mutex> lock(battery_mutex);
    if (watching_battery) return;
    if (battery_thread.joinable()) battery_thread.join();
    battery_action = 0;
    battery_refresh_requested = false;
    battery_view = BatteryView{};
    battery_view.history_enabled = set->battery_history_enabled;
    watching_battery = true;
    battery_thread = std::thread([]() {
        const auto path = battery_history_path();
        BatteryView next;
        next.history = load_battery_history(path);
        // Session samples carry only known continuous discharge. Persisted levels
        // alone cannot tell us whether the laptop was charging between readings.
        std::vector<BatterySample> discharge;
        bool save_needed = true;
        while (!finished) {
            int action;
            {
                std::lock_guard<std::mutex> lock(battery_mutex);
                action = battery_action;
                battery_action = 0;
                battery_refresh_requested = false;
            }
            if (action) {
                next.action_error.clear();
                bool applied = std::abs(action) == 2 ? dbus_set_battery_protector(action > 0, next.action_error) :
                    dbus_set_battery_saver(action > 0, next.action_error);
                if (!applied && next.action_error.empty()) next.action_error = "The power setting could not be changed.";
            }
            next.status = dbus_read_battery();
            const auto now = std::time(nullptr);
            const auto bucket = now - now % 300;
            next.updated = now;
            {
                // Serialize recording with the toggle so no write starts after disabling it.
                std::lock_guard<std::mutex> lock(battery_mutex);
                next.history_enabled = battery_view.history_enabled;
                auto old_size = next.history.size();
                std::erase_if(next.history, [now](const auto &s) { return s.timestamp <= now - 86400 || s.timestamp > now; });
                save_needed |= old_size != next.history.size();
                if (next.history_enabled && next.status.valid && (next.history.empty() || next.history.back().timestamp < bucket)) {
                    next.history.push_back({bucket, next.status.percentage});
                    save_needed = true;
                }
                if (next.history_enabled && save_needed) {
                    bool saved = save_battery_history(path, next.history);
                    next.history_error = saved ? "" : "History could not be saved. Check your Mylar config-folder permissions.";
                    save_needed = !saved;
                }
                if (!next.history_enabled) next.history_error.clear();
            }
            next.percent_per_hour = 0;
            next.remaining_seconds = 0;
            auto &status = next.status;
            if (status.valid && status.state == 2) {
                if (!discharge.empty() && (now <= discharge.back().timestamp || now - discharge.back().timestamp > 120 ||
                    status.percentage > discharge.back().percentage + .2)) discharge.clear();
                std::erase_if(discharge, [now](const auto &s) { return s.timestamp < now - 1800; });
                discharge.push_back({now, status.percentage});
                // Average over 15–30 minutes to avoid extrapolating 1% quantisation noise.
                if (discharge.size() >= 2 && now - discharge.front().timestamp >= 900) {
                    double drop = discharge.front().percentage - status.percentage;
                    if (drop >= 1)
                        next.percent_per_hour = drop * 3600 / double(now - discharge.front().timestamp);
                }
                if (next.percent_per_hour <= 0 && std::isfinite(status.energy_rate) && status.energy_rate > .1 &&
                    std::isfinite(status.energy_full) && status.energy_full > 0)
                    next.percent_per_hour = 100 * status.energy_rate / status.energy_full;
                if (status.time_to_empty > 0 && status.time_to_empty <= 7 * 86400)
                    next.remaining_seconds = status.time_to_empty;
                else if (next.percent_per_hour > 0)
                    next.remaining_seconds = status.percentage / next.percent_per_hour * 3600;
                else if (std::isfinite(status.energy) && status.energy > 0 &&
                    std::isfinite(status.energy_rate) && status.energy_rate > .1)
                    next.remaining_seconds = status.energy / status.energy_rate * 3600;
                if (!std::isfinite(next.remaining_seconds) || next.remaining_seconds > 7 * 86400)
                    next.remaining_seconds = 0;
            } else {
                discharge.clear();
            }
            if (action && next.action_error.empty()) {
                bool actual = std::abs(action) == 2 ? status.protector_enabled : status.battery_saver;
                bool available = std::abs(action) == 2 ? status.protector_available : status.saver_available;
                if (!available || actual != (action > 0))
                    next.action_error = "The requested setting was not confirmed by the power service.";
            }
            {
                std::lock_guard<std::mutex> lock(battery_mutex);
                next.pending = battery_action != 0;
                next.history_enabled = battery_view.history_enabled;
                if (!next.history_enabled) next.history_error.clear();
                battery_view = next;
            }
            if (!finished) dock::change_in_battery();
            std::unique_lock<std::mutex> lock(battery_mutex);
            battery_wakeup.wait_for(lock, std::chrono::seconds(30), []() {
                return finished || battery_action != 0 || battery_refresh_requested;
            });
        }
        std::lock_guard<std::mutex> lock(battery_mutex);
        watching_battery = false;
    });
}

static std::string battery_duration(double seconds) {
    auto minutes = std::max(1, int(std::round(seconds / 60)));
    if (minutes < 60) return std::format("{} min", minutes);
    return std::format("{} hr {} min", minutes / 60, minutes % 60);
}

static std::string battery_clock(std::time_t timestamp, bool include_day = false) {
    std::tm local{};
    localtime_r(&timestamp, &local);
    char text[64];
    std::strftime(text, sizeof(text), include_day ? "%a %I:%M %p" : "%I:%M %p", &local);
    std::string result = text;
    if (!include_day && result[0] == '0') result.erase(0, 1);
    return result;
}

static void battery_text(cairo_t *cr, double x, double y, const std::string &text, double size,
                         double dpi, RGBA color, double width = -1, bool bold = false) {
    draw_text(cr, x, y, text, size * dpi, true, mylar_font, width, -1, color, bold);
}

static void paint_battery_graph(cairo_t *cr, const Bounds &b, double dpi, const BatteryView &view,
                                double mouse_x, bool hovered) {
    const RGBA muted = {.40, .46, .54, 1};
    const RGBA blue = {.08, .48, .88, 1};
    const auto now = std::time(nullptr);
    const bool forecast = view.status.valid && view.status.state == 2 && view.remaining_seconds > 0;
    const double history_seconds = 3 * 3600;
    // Keep one third of the plot available for a discharge projection.
    const double future_seconds = 5400;
    const double start = now - history_seconds;
    const double total = history_seconds + future_seconds;
    const Bounds plot(b.x + 32 * dpi, b.y + 8 * dpi, b.w - 44 * dpi, b.h - 40 * dpi);
    auto x = [&](double t) { return plot.x + (t - start) / total * plot.w; };
    auto y = [&](double percent) { return plot.y + (1 - percent / 100) * plot.h; };
    cairo_save(cr);
    set_argb(cr, {.08, .48, .88, .04});
    cairo_rectangle(cr, x(now), plot.y, plot.right() - x(now), plot.h);
    cairo_fill(cr);
    for (int percent : {0, 25, 50, 75, 100}) {
        set_argb(cr, {.89, .92, .95, 1});
        cairo_set_line_width(cr, dpi);
        cairo_move_to(cr, plot.x, y(percent));
        cairo_line_to(cr, plot.right(), y(percent));
        cairo_stroke(cr);
        battery_text(cr, b.x, y(percent) - 6 * dpi, std::to_string(percent), 8, dpi, muted);
    }
    set_argb(cr, {.68, .75, .83, 1});
    cairo_move_to(cr, x(now), plot.y);
    cairo_line_to(cr, x(now), plot.bottom());
    cairo_stroke(cr);
    std::vector<BatterySample> points;
    for (const auto &sample : view.history)
        if (sample.timestamp >= start && sample.timestamp <= now) points.push_back(sample);
    const auto recorded_points = points.size();
    if (view.status.valid && now - view.updated <= 90) points.push_back({view.updated, view.status.percentage});
    // Draw separate segments across missed samples (suspend, shutdown, missing service).
    for (size_t begin = 0; begin < points.size();) {
        size_t end = begin + 1;
        while (end < points.size() && points[end].timestamp - points[end - 1].timestamp <= 360) ++end;
        if (end - begin >= 2) {
            cairo_move_to(cr, x(points[begin].timestamp), y(0));
            for (size_t i = begin; i < end; ++i) cairo_line_to(cr, x(points[i].timestamp), y(points[i].percentage));
            cairo_line_to(cr, x(points[end - 1].timestamp), y(0));
            cairo_close_path(cr);
            auto gradient = cairo_pattern_create_linear(0, plot.y, 0, plot.bottom());
            cairo_pattern_add_color_stop_rgba(gradient, 0, .08, .48, .88, .24);
            cairo_pattern_add_color_stop_rgba(gradient, 1, .08, .48, .88, .02);
            cairo_set_source(cr, gradient);
            cairo_fill(cr);
            cairo_pattern_destroy(gradient);
            set_argb(cr, blue);
            cairo_set_line_width(cr, 2 * dpi);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_move_to(cr, x(points[begin].timestamp), y(points[begin].percentage));
            for (size_t i = begin + 1; i < end; ++i) cairo_line_to(cr, x(points[i].timestamp), y(points[i].percentage));
            cairo_stroke(cr);
        } else {
            set_argb(cr, blue);
            cairo_arc(cr, x(points[begin].timestamp), y(points[begin].percentage), 2.5 * dpi, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        begin = end;
    }
    if (forecast) {
        set_argb(cr, {.08, .48, .88, .65});
        double dash[] = {4 * dpi, 4 * dpi};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_line_width(cr, 2 * dpi);
        cairo_move_to(cr, x(now), y(view.status.percentage));
        const double duration = std::min(future_seconds, view.remaining_seconds);
        cairo_line_to(cr, x(now + duration), y(view.status.percentage * (1 - duration / view.remaining_seconds)));
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
    }
    if (view.status.valid) {
        set_argb(cr, blue);
        cairo_arc(cr, x(now), y(view.status.percentage), 3.5 * dpi, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    if (hovered) {
        const double hover_x = std::clamp(mouse_x, plot.x, plot.right());
        const double hover_time = start + (hover_x - plot.x) / plot.w * total;
        double hover_percent = 0;
        bool intersects = false;
        for (size_t i = 1; i < points.size(); ++i) {
            if (hover_time < points[i - 1].timestamp || hover_time > points[i].timestamp ||
                points[i].timestamp - points[i - 1].timestamp > 360) continue;
            const double fraction = (hover_time - points[i - 1].timestamp) /
                double(points[i].timestamp - points[i - 1].timestamp);
            hover_percent = points[i - 1].percentage + fraction *
                (points[i].percentage - points[i - 1].percentage);
            intersects = true;
            break;
        }
        if (forecast && hover_time >= now && hover_time <= now + std::min(future_seconds, view.remaining_seconds)) {
            hover_percent = view.status.percentage * (1 - (hover_time - now) / view.remaining_seconds);
            intersects = true;
        }
        if (intersects) {
            const double hover_y = y(hover_percent);
            set_argb(cr, {.25, .35, .46, .72});
            double dash[] = {3 * dpi, 3 * dpi};
            cairo_set_dash(cr, dash, 2, 0);
            cairo_set_line_width(cr, dpi);
            cairo_move_to(cr, hover_x, plot.y);
            cairo_line_to(cr, hover_x, plot.bottom());
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0);
            set_argb(cr, blue);
            cairo_arc(cr, hover_x, hover_y, 3.5 * dpi, 0, 2 * M_PI);
            cairo_fill(cr);
            const auto time_label = battery_clock(static_cast<std::time_t>(std::round(hover_time)));
            const auto charge_label = std::format("{:.1f}%", hover_percent);
            const double label_x = std::clamp(hover_x + 6 * dpi, plot.x, plot.right() - 68 * dpi);
            const double label_y = std::clamp(hover_y - 26 * dpi, plot.y, plot.bottom() - 28 * dpi);
            battery_text(cr, label_x, label_y, charge_label, 9, dpi, {.12, .17, .24, 1}, 65 * dpi, true);
            battery_text(cr, label_x, label_y + 14 * dpi, time_label, 8, dpi, muted, 68 * dpi);
        }
    }
    battery_text(cr, plot.x, plot.bottom() + 10 * dpi, battery_clock(now - 10800), 8, dpi, muted);
    battery_text(cr, x(now - 5400) - 24 * dpi, plot.bottom() + 10 * dpi, battery_clock(now - 5400), 8, dpi, muted);
    battery_text(cr, x(now) - 12 * dpi, plot.bottom() + 10 * dpi, "Now", 8, dpi, muted);
    battery_text(cr, plot.right() - 48 * dpi, plot.bottom() + 10 * dpi, battery_clock(now + future_seconds), 8, dpi, muted);
    if (recorded_points < 2) {
        battery_text(cr, plot.x + 12 * dpi, plot.y + 45 * dpi,
            view.history_enabled ? "Collecting battery history" : "History recording disabled", 11, dpi, muted);
        battery_text(cr, plot.x + 12 * dpi, plot.y + 67 * dpi,
            view.history_enabled ? "A new reading every 5 minutes" : "Enable history to record battery levels", 9, dpi, muted);
    }
    cairo_restore(cr);
}

static void fill_battery_container(Dock *dock) {
    auto root = dock->battery->root;
    root->type = ::vbox;
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->battery->raw_window->cr;
        auto dpi = dock->battery->raw_window->dpi;
        paint_popup_background(cr, c->real_bounds, dpi);
    };
    auto content = root->child(::vbox, FILL_SPACE, FILL_SPACE);
    content->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->battery->raw_window->dpi;
        c->wanted_pad = Bounds(20 * dpi, 16 * dpi, 20 * dpi, 12 * dpi);
    };
    auto sized_row = [content](double height) {
        auto row = content->child(FILL_SPACE, height);
        row->pre_layout = [height](Container *root, Container *c, const Bounds &) {
            auto dock = (Dock *) root->user_data;
            c->wanted_bounds.h = height * dock->battery->raw_window->dpi;
        };
        return row;
    };
    auto header = sized_row(24);
    header->type = ::hbox;
    header->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        battery_text(dock->battery->raw_window->cr, c->real_bounds.x, c->real_bounds.y,
            "Battery", 13, dock->battery->raw_window->dpi, {.12, .17, .24, 1}, -1, true);
    };
    header->child(FILL_SPACE, FILL_SPACE);
    auto history_toggle = header->child(170, FILL_SPACE);
    history_toggle->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dock = (Dock *) root->user_data;
        c->wanted_bounds.w = 170 * dock->battery->raw_window->dpi;
    };
    history_toggle->when_clicked = [](Container *, Container *) { toggle_battery_history(); };
    history_toggle->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->battery->raw_window->cr;
        auto dpi = dock->battery->raw_window->dpi;
        const bool enabled = battery_snapshot().history_enabled;
        auto b = c->real_bounds;
        battery_text(cr, b.x + 36 * dpi , b.y + 5 * dpi, enabled ? "History enabled" : "History disabled",
            9, dpi, {.40, .46, .54, 1});
        double x = b.right() - 42 * dpi;
        RGBA fill = enabled ? RGBA(.08, .48, .88, 1) : RGBA(.81, .85, .90, 1);
        if (c->state.mouse_hovering) fill = enabled ? RGBA(.06, .40, .78, 1) : RGBA(.72, .79, .87, 1);
        set_argb(cr, fill);
        drawRoundedRect(cr, x, b.y, 42 * dpi, 24 * dpi, 12 * dpi, 1);
        cairo_fill(cr);
        set_argb(cr, {1, 1, 1, 1});
        cairo_arc(cr, x + (enabled ? 30 : 12) * dpi, b.y + 12 * dpi, 9 * dpi, 0, 2 * M_PI);
        cairo_fill(cr);
    };
    auto summary = sized_row(114);
    summary->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->battery->raw_window->cr;
        auto dpi = dock->battery->raw_window->dpi;
        const auto view = battery_snapshot();
        const auto &s = view.status;
        auto b = c->real_bounds;
        RGBA ink = {.12, .17, .24, 1}, muted = {.40, .46, .54, 1};
        battery_text(cr, b.x, b.y + dpi, s.valid ? std::format("{:.0f}%", s.percentage) : "—", 30, dpi, ink, -1, true);
        std::string state = "Waiting for data";
        if (s.valid) {
            switch (s.state) {
                case 1: state = "Charging"; break;
                case 2: state = "On battery"; break;
                case 3: state = "Battery empty"; break;
                case 4: state = "Fully charged"; break;
                case 5: state = "Charging paused"; break;
                case 6: state = "Discharge paused"; break;
                default: state = "Status unavailable"; break;
            }
        }
        battery_text(cr, b.x + 135 * dpi, b.y + 11 * dpi, state, 11, dpi, muted);
        std::string estimate = s.valid ? "An estimate will appear while discharging." : s.reading_reason;
        if (s.valid && s.state == 2) {
            estimate = view.remaining_seconds > 0 ? "About " + battery_duration(view.remaining_seconds) + " remaining" :
                "Estimating remaining time…";
        } else if (s.valid && s.protector_enabled) {
            estimate = "Charge protection is enabled.";
        } else if (s.valid && s.state == 1) {
            estimate = "Connected to power · battery is charging";
        }
        battery_text(cr, b.x, b.y + 54 * dpi, estimate, 11, dpi, ink, b.w);
        std::string detail = "Estimates adapt to your current usage.";
        if (s.valid && s.state == 2 && view.remaining_seconds > 0) {
            auto end = view.updated + static_cast<std::time_t>(view.remaining_seconds);
            std::tm today{}, then{};
            localtime_r(&view.updated, &today);
            localtime_r(&end, &then);
            detail = "Estimated empty at " + battery_clock(end, today.tm_yday != then.tm_yday || today.tm_year != then.tm_year);
        } else if (s.valid && s.state == 2) {
            detail = "Waiting for a reliable discharge rate.";
        }
        if (s.valid) battery_text(cr, b.x, b.y + 86 * dpi, detail, 10, dpi, muted, b.w);
    };
    auto graph_title = sized_row(25);
    graph_title->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->battery->raw_window->dpi;
        auto cr = dock->battery->raw_window->cr;
        auto view = battery_snapshot();
        auto b = c->real_bounds;
        battery_text(cr, b.x, b.y, "Last 3 hours · charge %", 10, dpi, {.40, .46, .54, 1});
        std::string rate = "";
        if (view.status.valid && view.status.state == 2) {
            if (view.percent_per_hour > 0) rate = std::format("−{:.1f}% / hr", view.percent_per_hour);
            if (std::isfinite(view.status.energy_rate) && view.status.energy_rate > .1)
                rate += std::format("  {:.1f} W", view.status.energy_rate);
            if (rate.empty()) rate = "Measuring discharge…";
        }
        auto bounds = draw_text(cr, 0, 0, rate, 9 * dpi, false);
        battery_text(cr, b.right() - bounds.w, b.y, rate, 9, dpi, {.08, .48, .78, 1});
    };
    auto graph = sized_row(175);
    graph->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        paint_battery_graph(dock->battery->raw_window->cr, c->real_bounds, dock->battery->raw_window->dpi,
            battery_snapshot(), root->mouse_current_x, c->state.mouse_hovering);
    };
    graph->when_mouse_motion = request_damage;
    graph->when_mouse_enters_container = request_damage;
    graph->when_mouse_leaves_container = request_damage;
    auto legend = sized_row(24);
    legend->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->battery->raw_window->dpi;
        battery_text(dock->battery->raw_window->cr, c->real_bounds.x, c->real_bounds.y,
            "Solid: recorded charge   ·   Dashed: estimate", 8, dpi, {.40, .46, .54, 1});
    };
    for (bool protector : {false, true}) {
        auto control = sized_row(70);
        control->when_clicked = [protector](Container *, Container *) { request_battery_action(protector); };
        control->when_paint = [protector](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto cr = dock->battery->raw_window->cr;
            auto dpi = dock->battery->raw_window->dpi;
            const auto view = battery_snapshot();
            const auto &s = view.status;
            const bool enabled = protector ? s.protector_enabled : s.battery_saver;
            const bool available = (protector ? s.protector_available : s.saver_available) && !view.pending;
            auto b = c->real_bounds;
            set_argb(cr, {.88, .91, .95, 1});
            cairo_set_line_width(cr, dpi);
            cairo_move_to(cr, b.x, b.y);
            cairo_line_to(cr, b.right(), b.y);
            cairo_stroke(cr);
            RGBA ink = available ? RGBA(.12, .17, .24, 1) : RGBA(.43, .47, .53, 1);
            battery_text(cr, b.x, b.y + 9 * dpi, protector ? "Battery protector" : "Battery saver", 11, dpi, ink, -1, true);
            std::string reason = protector ? s.protector_reason : s.saver_reason;
            if (reason.empty()) reason = "Checking power service…";
            const std::string label = enabled ? "On" : "Off";
            const auto label_bounds = draw_text(cr, 0, 0, label, 9 * dpi, false);
            double width = protector ? label_bounds.w + 16 * dpi : 42 * dpi;
            double height = protector ? label_bounds.h + 10 * dpi : 24 * dpi;
            battery_text(cr, b.x, b.y + 32 * dpi, reason, 9, dpi, {.40, .46, .54, 1}, b.w - width - 8 * dpi);
            double x = b.right() - width, y = b.y + 28 * dpi - height * .5;
            RGBA fill = enabled ? RGBA(.08, .48, .88, available ? 1 : .4) : RGBA(.81, .85, .90, available ? 1 : .5);
            if (available && c->state.mouse_hovering) fill = enabled ? RGBA(.06, .40, .78, 1) : RGBA(.72, .79, .87, 1);
            set_argb(cr, fill);
            drawRoundedRect(cr, x, y, width, height, (protector ? 6 : 12) * dpi, 1);
            cairo_fill(cr);
            if (protector) {
                battery_text(cr, x + 8 * dpi, y + 5 * dpi, label, 9, dpi,
                    enabled ? RGBA(1, 1, 1, 1) : ink);
            } else {
                set_argb(cr, {1, 1, 1, 1});
                cairo_arc(cr, x + (enabled ? 30 : 12) * dpi, y + 12 * dpi, 9 * dpi, 0, 2 * M_PI);
                cairo_fill(cr);
            }
        };
    }
    auto feedback = content->child(FILL_SPACE, FILL_SPACE);
    feedback->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto view = battery_snapshot();
        auto message = view.pending ? "Applying power setting…" : view.action_error;
        if (message.empty()) message = view.history_error;
        if (message.empty()) return;
        battery_text(dock->battery->raw_window->cr, c->real_bounds.x, c->real_bounds.y + 4 * dock->battery->raw_window->dpi,
            message, 8, dock->battery->raw_window->dpi,
            view.action_error.empty() && view.history_error.empty() ? RGBA(.40, .46, .54, 1) : RGBA(.70, .23, .16, 1), c->real_bounds.w);
    };
}
