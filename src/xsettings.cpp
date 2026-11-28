#include "xsettings.h"

#include "hypriso.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <xcb/xcb.h>

namespace {

enum XSettingType : uint8_t {
    XSETTINGS_TYPE_INTEGER = 0,
    XSETTINGS_TYPE_STRING = 1,
    XSETTINGS_TYPE_COLOR = 2,
};

struct XSetting {
    XSettingType type = XSETTINGS_TYPE_INTEGER;
    uint32_t last_change_serial = 0;
    int32_t int_val = 0;
    std::string str_val;
    std::array<uint16_t, 4> color_val = {0, 0, 0, 0};
};

uint16_t read_u16(const uint8_t *p, bool big_endian) {
    if (big_endian)
        return (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
    return (uint16_t) (p[0] | ((uint16_t) p[1] << 8));
}

uint32_t read_u32(const uint8_t *p, bool big_endian) {
    if (big_endian)
        return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

void write_u16(std::vector<uint8_t> &buf, uint16_t v) {
    if constexpr (std::endian::native == std::endian::big) {
        buf.push_back((uint8_t) ((v >> 8) & 0xff));
        buf.push_back((uint8_t) (v & 0xff));
    } else {
        buf.push_back((uint8_t) (v & 0xff));
        buf.push_back((uint8_t) ((v >> 8) & 0xff));
    }
}

void write_u32(std::vector<uint8_t> &buf, uint32_t v) {
    if constexpr (std::endian::native == std::endian::big) {
        buf.push_back((uint8_t) ((v >> 24) & 0xff));
        buf.push_back((uint8_t) ((v >> 16) & 0xff));
        buf.push_back((uint8_t) ((v >> 8) & 0xff));
        buf.push_back((uint8_t) (v & 0xff));
    } else {
        buf.push_back((uint8_t) (v & 0xff));
        buf.push_back((uint8_t) ((v >> 8) & 0xff));
        buf.push_back((uint8_t) ((v >> 16) & 0xff));
        buf.push_back((uint8_t) ((v >> 24) & 0xff));
    }
}

size_t pad4(size_t n) {
    return (4 - (n % 4)) % 4;
}

bool parse_xsettings(const uint8_t *data, size_t len, std::map<std::string, XSetting> &out, uint32_t &out_serial) {
    if (len < 12)
        return false;
    if (data[0] != 0 && data[0] != 1)
        return false;
    const bool big_endian = (data[0] == 1);
    out_serial = read_u32(data + 4, big_endian);
    const uint32_t count = read_u32(data + 8, big_endian);

    size_t pos = 12;
    std::map<std::string, XSetting> parsed;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 4 > len)
            return false;
        uint8_t type = data[pos];
        uint16_t name_len = read_u16(data + pos + 2, big_endian);
        pos += 4;

        size_t name_pad = pad4(name_len);
        if (pos + name_len + name_pad + 4 > len)
            return false;
        std::string name(reinterpret_cast<const char *>(data + pos), name_len);
        pos += name_len + name_pad;

        XSetting entry;
        entry.last_change_serial = read_u32(data + pos, big_endian);
        pos += 4;

        if (type == XSETTINGS_TYPE_INTEGER) {
            if (pos + 4 > len)
                return false;
            entry.type = XSETTINGS_TYPE_INTEGER;
            entry.int_val = (int32_t) read_u32(data + pos, big_endian);
            pos += 4;
        } else if (type == XSETTINGS_TYPE_STRING) {
            if (pos + 4 > len)
                return false;
            uint32_t str_len = read_u32(data + pos, big_endian);
            pos += 4;
            size_t str_pad = pad4(str_len);
            if (pos + str_len + str_pad > len)
                return false;
            entry.type = XSETTINGS_TYPE_STRING;
            entry.str_val.assign(reinterpret_cast<const char *>(data + pos), str_len);
            pos += str_len + str_pad;
        } else if (type == XSETTINGS_TYPE_COLOR) {
            if (pos + 8 > len)
                return false;
            entry.type = XSETTINGS_TYPE_COLOR;
            for (int c = 0; c < 4; ++c)
                entry.color_val[c] = read_u16(data + pos + c * 2, big_endian);
            pos += 8;
        } else {
            return false;
        }

        parsed[std::move(name)] = std::move(entry);
    }

    out = std::move(parsed);
    return true;
}

std::vector<uint8_t> serialize_xsettings(const std::map<std::string, XSetting> &settings, uint32_t serial) {
    std::vector<uint8_t> buf;
    buf.reserve(512);
    buf.push_back(std::endian::native == std::endian::big ? 1 : 0);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    write_u32(buf, serial);
    write_u32(buf, (uint32_t) settings.size());

    for (const auto &[name, entry] : settings) {
        if (name.size() > 0xffff)
            continue;
        buf.push_back((uint8_t) entry.type);
        buf.push_back(0);
        write_u16(buf, (uint16_t) name.size());
        buf.insert(buf.end(), name.begin(), name.end());
        buf.insert(buf.end(), pad4(name.size()), 0);
        write_u32(buf, entry.last_change_serial);

        if (entry.type == XSETTINGS_TYPE_INTEGER) {
            write_u32(buf, (uint32_t) entry.int_val);
        } else if (entry.type == XSETTINGS_TYPE_STRING) {
            write_u32(buf, (uint32_t) entry.str_val.size());
            buf.insert(buf.end(), entry.str_val.begin(), entry.str_val.end());
            buf.insert(buf.end(), pad4(entry.str_val.size()), 0);
        } else if (entry.type == XSETTINGS_TYPE_COLOR) {
            for (uint16_t ch : entry.color_val)
                write_u16(buf, ch);
        }
    }
    return buf;
}

xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name) {
    auto cookie = xcb_intern_atom(conn, 0, (uint16_t) std::strlen(name), name);
    auto *reply = xcb_intern_atom_reply(conn, cookie, nullptr);
    if (!reply)
        return XCB_ATOM_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

bool read_owner_settings(xcb_connection_t *conn, xcb_window_t win, xcb_atom_t atom_settings,
                         std::map<std::string, XSetting> &out, uint32_t &out_serial) {
    auto cookie = xcb_get_property(conn, 0, win, atom_settings, atom_settings, 0, 65536);
    auto *reply = xcb_get_property_reply(conn, cookie, nullptr);
    if (!reply)
        return false;
    bool ok = false;
    if (reply->type == atom_settings && reply->format == 8) {
        const auto *val = static_cast<const uint8_t *>(xcb_get_property_value(reply));
        int len = xcb_get_property_value_length(reply);
        if (val && len >= 12)
            ok = parse_xsettings(val, (size_t) len, out, out_serial);
    }
    free(reply);
    return ok;
}

bool dpi_settings_match(const std::map<std::string, XSetting> &settings, int dpi) {
    const int32_t scaled_dpi = dpi * 1024;
    auto check_int = [&](const char *key, int32_t expected) {
        auto it = settings.find(key);
        return it != settings.end() && it->second.type == XSETTINGS_TYPE_INTEGER && it->second.int_val == expected;
    };
    return check_int("Xft/DPI", scaled_dpi) &&
           check_int("Gdk/UnscaledDPI", scaled_dpi) &&
           check_int("Gdk/WindowScalingFactor", 1);
}

void set_int_setting(std::map<std::string, XSetting> &settings, const char *key, int32_t val, uint32_t serial) {
    auto &entry = settings[key];
    if (entry.type == XSETTINGS_TYPE_INTEGER && entry.int_val == val && entry.last_change_serial != 0)
        return;
    entry.type = XSETTINGS_TYPE_INTEGER;
    entry.int_val = val;
    entry.last_change_serial = serial;
}

struct XSettingsState {
    std::mutex mu;
    std::thread worker;
    std::atomic<bool> stop_requested{false};
    std::atomic<int> target_dpi{0};
    int wake_fd = -1;
};

XSettingsState state;

void wake_worker() {
    if (state.wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t ignored = write(state.wake_fd, &one, sizeof(one));
        (void) ignored;
    }
}

void drain_wake() {
    if (state.wake_fd >= 0) {
        uint64_t val = 0;
        while (read(state.wake_fd, &val, sizeof(val)) > 0) {}
    }
}

void wait_interruptible(int ms) {
    if (state.wake_fd < 0)
        return;
    pollfd pfd{};
    pfd.fd = state.wake_fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, ms) > 0)
        drain_wake();
}

void worker_main() {
    using clock = std::chrono::steady_clock;

    while (!state.stop_requested.load(std::memory_order_acquire)) {
        const char *display = std::getenv("DISPLAY");
        if (!hypriso || !hypriso->xwayland_ready() || !display || !*display || state.target_dpi.load() <= 0) {
            wait_interruptible(250);
            continue;
        }

        int screen_number = 0;
        xcb_connection_t *conn = xcb_connect(display, &screen_number);
        if (!conn || xcb_connection_has_error(conn)) {
            if (conn)
                xcb_disconnect(conn);
            wait_interruptible(1000);
            continue;
        }

        xcb_screen_t *screen = nullptr;
        auto iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
        for (int i = 0; iter.rem; ++i, xcb_screen_next(&iter)) {
            if (i == screen_number) {
                screen = iter.data;
                break;
            }
        }
        if (!screen) {
            xcb_disconnect(conn);
            wait_interruptible(1000);
            continue;
        }

        std::string sel_name = "_XSETTINGS_S" + std::to_string(screen_number);
        xcb_atom_t atom_selection = intern_atom(conn, sel_name.c_str());
        xcb_atom_t atom_settings = intern_atom(conn, "_XSETTINGS_SETTINGS");
        xcb_atom_t atom_manager = intern_atom(conn, "MANAGER");
        if (atom_selection == XCB_ATOM_NONE || atom_settings == XCB_ATOM_NONE || atom_manager == XCB_ATOM_NONE) {
            xcb_disconnect(conn);
            wait_interruptible(1000);
            continue;
        }

        xcb_window_t owner_window = xcb_generate_id(conn);
        uint32_t values[] = {1, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE};
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, owner_window, screen->root, -1, -1, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                          XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);

        std::map<std::string, XSetting> settings;
        uint32_t serial = 0;
        bool owns_selection = false;
        int applied_dpi = 0;

        auto publish = [&](int dpi, bool claim_selection) {
            ++serial;
            const int32_t scaled_dpi = dpi * 1024;
            set_int_setting(settings, "Xft/DPI", scaled_dpi, serial);
            set_int_setting(settings, "Gdk/UnscaledDPI", scaled_dpi, serial);
            set_int_setting(settings, "Gdk/WindowScalingFactor", 1, serial);

            auto buf = serialize_xsettings(settings, serial);
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, owner_window, atom_settings, atom_settings,
                                8, (uint32_t) buf.size(), buf.data());

            if (claim_selection) {
                xcb_set_selection_owner(conn, owner_window, atom_selection, XCB_CURRENT_TIME);
                auto *owner_reply = xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, atom_selection), nullptr);
                owns_selection = owner_reply && owner_reply->owner == owner_window;
                free(owner_reply);

                if (owns_selection) {
                    xcb_client_message_event_t msg{};
                    msg.response_type = XCB_CLIENT_MESSAGE;
                    msg.format = 32;
                    msg.window = screen->root;
                    msg.type = atom_manager;
                    msg.data.data32[0] = XCB_CURRENT_TIME;
                    msg.data.data32[1] = atom_selection;
                    msg.data.data32[2] = owner_window;
                    xcb_send_event(conn, 0, screen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                                   reinterpret_cast<const char *>(&msg));
                }
            }
            xcb_flush(conn);
            applied_dpi = dpi;
        };

        // Preserve any settings already published by another daemon (e.g. xsettingsd).
        {
            auto *owner_reply = xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, atom_selection), nullptr);
            xcb_window_t prev_owner = owner_reply ? owner_reply->owner : XCB_NONE;
            free(owner_reply);
            if (prev_owner != XCB_NONE && prev_owner != owner_window) {
                uint32_t prev_serial = 0;
                if (read_owner_settings(conn, prev_owner, atom_settings, settings, prev_serial))
                    serial = std::max(serial, prev_serial);
            }
        }

        int initial_dpi = state.target_dpi.load(std::memory_order_acquire);
        if (initial_dpi > 0)
            publish(initial_dpi, true);

        bool reclaim_pending = false;
        clock::time_point reclaim_at{};
        std::vector<clock::time_point> recent_reclaims;

        int xcb_fd = xcb_get_file_descriptor(conn);
        while (!state.stop_requested.load(std::memory_order_acquire) && !xcb_connection_has_error(conn)) {
            int timeout_ms = -1;
            if (reclaim_pending) {
                auto now = clock::now();
                if (now >= reclaim_at) {
                    timeout_ms = 0;
                } else {
                    timeout_ms = (int) std::chrono::duration_cast<std::chrono::milliseconds>(reclaim_at - now).count();
                    timeout_ms = std::max(1, timeout_ms);
                }
            }

            pollfd pfds[2]{};
            pfds[0].fd = xcb_fd;
            pfds[0].events = POLLIN;
            pfds[1].fd = state.wake_fd;
            pfds[1].events = POLLIN;

            int ret = poll(pfds, 2, timeout_ms);
            if (ret < 0)
                continue;

            if (pfds[1].revents & POLLIN) {
                drain_wake();
                if (state.stop_requested.load(std::memory_order_acquire))
                    break;
                int wanted_dpi = state.target_dpi.load(std::memory_order_acquire);
                if (wanted_dpi > 0 && wanted_dpi != applied_dpi) {
                    publish(wanted_dpi, !owns_selection);
                    reclaim_pending = false;
                }
            }

            if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
                xcb_generic_event_t *ev = nullptr;
                while ((ev = xcb_poll_for_event(conn)) != nullptr) {
                    uint8_t type = ev->response_type & ~0x80;
                    if (type == XCB_SELECTION_CLEAR) {
                        auto *sc = reinterpret_cast<xcb_selection_clear_event_t *>(ev);
                        if (sc->selection == atom_selection) {
                            owns_selection = false;
                            reclaim_pending = true;
                            reclaim_at = clock::now() + std::chrono::milliseconds(200);
                        }
                    }
                    free(ev);
                }
            }

            if (reclaim_pending && clock::now() >= reclaim_at) {
                reclaim_pending = false;
                int wanted_dpi = state.target_dpi.load(std::memory_order_acquire);
                if (!owns_selection && wanted_dpi > 0) {
                    auto now = clock::now();
                    recent_reclaims.erase(
                        std::remove_if(recent_reclaims.begin(), recent_reclaims.end(),
                                       [&](clock::time_point tp) { return now - tp > std::chrono::seconds(10); }),
                        recent_reclaims.end());

                    if (recent_reclaims.size() < 5) {
                        auto *owner_reply = xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, atom_selection), nullptr);
                        xcb_window_t cur_owner = owner_reply ? owner_reply->owner : XCB_NONE;
                        free(owner_reply);

                        std::map<std::string, XSetting> their_settings;
                        uint32_t their_serial = 0;
                        bool has_their_settings = (cur_owner != XCB_NONE && cur_owner != owner_window) &&
                                                  read_owner_settings(conn, cur_owner, atom_settings, their_settings, their_serial);

                        if (!has_their_settings || !dpi_settings_match(their_settings, wanted_dpi)) {
                            if (has_their_settings) {
                                settings = std::move(their_settings);
                                serial = std::max(serial, their_serial);
                            }
                            recent_reclaims.push_back(now);
                            publish(wanted_dpi, true);
                        }
                    }
                }
            }
        }

        if (!xcb_connection_has_error(conn)) {
            xcb_destroy_window(conn, owner_window);
            xcb_flush(conn);
        }
        xcb_disconnect(conn);
    }
}

} // namespace

void xsettings::set_dpi(int dpi) {
    if (dpi <= 0)
        return;
    std::lock_guard<std::mutex> lock(state.mu);
    state.target_dpi.store(dpi, std::memory_order_release);
    if (state.wake_fd < 0)
        state.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (!state.worker.joinable()) {
        state.stop_requested.store(false, std::memory_order_release);
        state.worker = std::thread(worker_main);
    }
    wake_worker();
}

void xsettings::stop() {
    std::thread to_join;
    {
        std::lock_guard<std::mutex> lock(state.mu);
        state.stop_requested.store(true, std::memory_order_release);
        wake_worker();
        to_join = std::move(state.worker);
    }
    if (to_join.joinable())
        to_join.join();
    std::lock_guard<std::mutex> lock(state.mu);
    if (state.wake_fd >= 0) {
        close(state.wake_fd);
        state.wake_fd = -1;
    }
    state.target_dpi.store(0, std::memory_order_release);
}
