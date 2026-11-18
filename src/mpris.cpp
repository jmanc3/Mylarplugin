#include "mpris.h"

#include "hypriso.h"
#include "simple_dbus.h"

#include <algorithm>
#include <dbus/dbus.h>

namespace {

constexpr const char *kPrefix = "org.mpris.MediaPlayer2.";
constexpr const char *kPath = "/org/mpris/MediaPlayer2";
constexpr const char *kRootIface = "org.mpris.MediaPlayer2";
constexpr const char *kPlayerIface = "org.mpris.MediaPlayer2.Player";
constexpr const char *kPropsIface = "org.freedesktop.DBus.Properties";
constexpr int kCallTimeoutMs = 2000;

const char *kNameOwnerMatch =
    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
    "member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'";
const char *kPropertiesMatch =
    "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
    "path='/org/mpris/MediaPlayer2'";

struct State {
    DBusConnection *connection = nullptr; // connection the filter is installed on
    std::vector<mpris::Player> players;
    std::vector<DBusPendingCall *> pending;
    std::function<void()> on_change;
    bool change_queued = false;
};

State &state() {
    static State s;
    return s;
}

bool is_player_name(const char *name) {
    return name && std::string_view(name).starts_with(kPrefix);
}

mpris::Player *find_by_name(const std::string &name) {
    for (auto &p : state().players)
        if (p.name == name)
            return &p;
    return nullptr;
}

// Coalesces bursts of replies (a refresh sends several calls per player) into
// one notification.
void notify_changed() {
    auto &s = state();
    if (s.change_queued)
        return;
    s.change_queued = true;
    later_immediate([](Timer *) {
        auto &s = state();
        s.change_queued = false;
        if (s.connection && s.on_change)
            s.on_change();
    });
}

void forget_completed_calls() {
    std::erase_if(state().pending, [](DBusPendingCall *pending) {
        if (!dbus_pending_call_get_completed(pending))
            return false;
        dbus_pending_call_unref(pending);
        return true;
    });
}

// Sends `msg` and calls `handler(reply, name)` when it answers. The pending call
// is tracked so stop() can cancel it before the plugin goes away.
void call_async(DBusMessage *msg, const std::string &name, DBusPendingCallNotifyFunction handler) {
    auto &s = state();
    if (!msg)
        return;
    if (!s.connection) {
        dbus_message_unref(msg);
        return;
    }
    forget_completed_calls();
    DBusPendingCall *pending = nullptr;
    if (!dbus_connection_send_with_reply(s.connection, msg, &pending, kCallTimeoutMs) || !pending) {
        dbus_message_unref(msg);
        return;
    }
    dbus_message_unref(msg);
    auto *data = new std::string(name);
    if (!dbus_pending_call_set_notify(pending, handler, data, [](void *d) { delete (std::string *) d; })) {
        delete data;
        dbus_pending_call_cancel(pending);
        dbus_pending_call_unref(pending);
        return;
    }
    s.pending.push_back(pending);
}

DBusMessage *steal_method_return(DBusPendingCall *call) {
    DBusMessage *reply = dbus_pending_call_steal_reply(call);
    if (!reply)
        return nullptr;
    if (dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        dbus_message_unref(reply);
        return nullptr;
    }
    return reply;
}

std::string read_string(DBusMessageIter *it) {
    const int type = dbus_message_iter_get_arg_type(it);
    if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
        return "";
    const char *value = nullptr;
    dbus_message_iter_get_basic(it, &value);
    return value ? value : "";
}

bool read_bool(DBusMessageIter *it, bool fallback) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_BOOLEAN)
        return fallback;
    dbus_bool_t value = FALSE;
    dbus_message_iter_get_basic(it, &value);
    return value;
}

// Calls `fn(key, variant_value_iter)` for every entry of an a{sv} at `it`.
template <typename Fn>
void for_each_property(DBusMessageIter *it, Fn &&fn) {
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter dict;
    dbus_message_iter_recurse(it, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict, &entry);
        const std::string key = read_string(&entry);
        if (dbus_message_iter_next(&entry) && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter value;
            dbus_message_iter_recurse(&entry, &value);
            fn(key, &value);
        }
        dbus_message_iter_next(&dict);
    }
}

void parse_metadata(mpris::Player &p, DBusMessageIter *it) {
    p.title.clear();
    p.artist.clear();
    for_each_property(it, [&p](const std::string &key, DBusMessageIter *value) {
        if (key == "xesam:title") {
            p.title = read_string(value);
        } else if (key == "xesam:artist") {
            if (dbus_message_iter_get_arg_type(value) == DBUS_TYPE_ARRAY) {
                DBusMessageIter artists;
                dbus_message_iter_recurse(value, &artists);
                std::string joined;
                while (dbus_message_iter_get_arg_type(&artists) == DBUS_TYPE_STRING) {
                    if (!joined.empty())
                        joined += ", ";
                    joined += read_string(&artists);
                    dbus_message_iter_next(&artists);
                }
                p.artist = joined;
            } else {
                p.artist = read_string(value);
            }
        }
    });
}

// Applies an a{sv} of org.mpris.MediaPlayer2 or org.mpris.MediaPlayer2.Player
// properties. Returns true when something the UI shows changed.
bool apply_properties(mpris::Player &p, DBusMessageIter *it) {
    const mpris::Player before = p;
    for_each_property(it, [&p](const std::string &key, DBusMessageIter *value) {
        if (key == "PlaybackStatus") p.status = read_string(value);
        else if (key == "Metadata") parse_metadata(p, value);
        else if (key == "CanControl") p.can_control = read_bool(value, p.can_control);
        else if (key == "CanPlay") p.can_play = read_bool(value, p.can_play);
        else if (key == "CanPause") p.can_pause = read_bool(value, p.can_pause);
        else if (key == "CanGoNext") p.can_go_next = read_bool(value, p.can_go_next);
        else if (key == "CanGoPrevious") p.can_go_previous = read_bool(value, p.can_go_previous);
        else if (key == "Identity") p.identity = read_string(value);
        else if (key == "DesktopEntry") p.desktop_entry = read_string(value);
    });
    return before.status != p.status || before.title != p.title || before.artist != p.artist ||
           before.can_control != p.can_control || before.can_play != p.can_play ||
           before.can_pause != p.can_pause || before.can_go_next != p.can_go_next ||
           before.can_go_previous != p.can_go_previous || before.identity != p.identity ||
           before.desktop_entry != p.desktop_entry;
}

void on_get_all(DBusPendingCall *call, void *data) {
    DBusMessage *reply = steal_method_return(call);
    if (!reply)
        return;
    auto *p = find_by_name(*(std::string *) data);
    DBusMessageIter it;
    if (p && dbus_message_iter_init(reply, &it) && apply_properties(*p, &it))
        notify_changed();
    dbus_message_unref(reply);
}

void request_properties(const std::string &name, const char *iface) {
    auto *msg = dbus_message_new_method_call(name.c_str(), kPath, kPropsIface, "GetAll");
    if (!msg)
        return;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    call_async(msg, name, on_get_all);
}

void on_pid(DBusPendingCall *call, void *data) {
    DBusMessage *reply = steal_method_return(call);
    if (!reply)
        return;
    dbus_uint32_t pid = 0;
    if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID)) {
        if (auto *p = find_by_name(*(std::string *) data)) {
            if (p->pid != (int) pid) {
                p->pid = (int) pid;
                notify_changed();
            }
        }
    }
    dbus_message_unref(reply);
}

void on_owner(DBusPendingCall *call, void *data) {
    DBusMessage *reply = steal_method_return(call);
    if (!reply)
        return;
    const char *owner = nullptr;
    if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) && owner) {
        if (auto *p = find_by_name(*(std::string *) data))
            p->owner = owner;
    }
    dbus_message_unref(reply);
}

void bus_call(const std::string &name, const char *method, DBusPendingCallNotifyFunction handler) {
    auto *msg = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                             "org.freedesktop.DBus", method);
    if (!msg)
        return;
    const char *arg = name.c_str();
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
    call_async(msg, name, handler);
}

void add_player(const std::string &name, const std::string &owner) {
    auto &s = state();
    if (!find_by_name(name)) {
        mpris::Player p;
        p.name = name;
        s.players.push_back(p);
    }
    auto *p = find_by_name(name);
    if (!owner.empty())
        p->owner = owner;
    else
        bus_call(name, "GetNameOwner", on_owner);
    bus_call(name, "GetConnectionUnixProcessID", on_pid);
    request_properties(name, kRootIface);
    request_properties(name, kPlayerIface);
}

void remove_player(const std::string &name) {
    auto &players = state().players;
    const auto old = players.size();
    std::erase_if(players, [&name](const mpris::Player &p) { return p.name == name; });
    if (players.size() != old)
        notify_changed();
}

void on_list_names(DBusPendingCall *call, void *) {
    DBusMessage *reply = steal_method_return(call);
    if (!reply)
        return;
    char **names = nullptr;
    int count = 0;
    if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &names, &count, DBUS_TYPE_INVALID)) {
        for (int i = 0; i < count; i++)
            if (is_player_name(names[i]))
                add_player(names[i], "");
        dbus_free_string_array(names);
    }
    dbus_message_unref(reply);
}

DBusHandlerResult filter(DBusConnection *, DBusMessage *msg, void *) {
    // Return NOT_YET_HANDLED in every case: other filters on the shared session
    // connection (systray, notifications) may want the same signals.
    if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) && is_player_name(name)) {
            if (new_owner && *new_owner) {
                remove_player(name); // the owner (and so the pid) may have changed
                add_player(name, new_owner);
            } else {
                remove_player(name);
            }
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_is_signal(msg, kPropsIface, "PropertiesChanged") && dbus_message_has_path(msg, kPath)) {
        const char *sender = dbus_message_get_sender(msg);
        if (!sender)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        DBusMessageIter it;
        if (!dbus_message_iter_init(msg, &it))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        const std::string iface = read_string(&it);
        if (iface != kPlayerIface && iface != kRootIface)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        dbus_message_iter_next(&it);

        bool changed = false;
        for (auto &p : state().players) {
            if (p.owner != sender)
                continue;
            DBusMessageIter props = it;
            changed |= apply_properties(p, &props);
            bool invalidated = false;
            DBusMessageIter rest = it;
            if (dbus_message_iter_next(&rest) && dbus_message_iter_get_arg_type(&rest) == DBUS_TYPE_ARRAY) {
                DBusMessageIter names;
                dbus_message_iter_recurse(&rest, &names);
                invalidated = dbus_message_iter_get_arg_type(&names) == DBUS_TYPE_STRING;
            }
            if (invalidated)
                request_properties(p.name, iface == kPlayerIface ? kPlayerIface : kRootIface);
        }
        if (changed)
            notify_changed();
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void send_command(const std::string &name, const char *method) {
    auto &s = state();
    if (!s.connection || name.empty())
        return;
    auto *msg = dbus_message_new_method_call(name.c_str(), kPath, kPlayerIface, method);
    if (!msg)
        return;
    dbus_message_set_no_reply(msg, TRUE);
    dbus_connection_send(s.connection, msg, nullptr);
    dbus_message_unref(msg);
    dbus_connection_flush(s.connection);

    // Not every player announces its new state (or track) promptly; read it
    // back so the button reflects what actually happened.
    for (int delay : {150, 600}) {
        later(delay, [name](Timer *) {
            if (state().connection && find_by_name(name))
                request_properties(name, kPlayerIface);
        });
    }
}

} // namespace

void mpris::ensure_started() {
    auto &s = state();
    if (s.connection) {
        if (s.connection == dbus_connection_session)
            return;
        stop(); // the session bus was reconnected
    }
    if (!dbus_connection_session)
        return;
    if (!dbus_connection_add_filter(dbus_connection_session, filter, nullptr, nullptr))
        return;
    s.connection = dbus_connection_session;
    // Without an error argument these are sent without waiting for a reply.
    dbus_bus_add_match(s.connection, kNameOwnerMatch, nullptr);
    dbus_bus_add_match(s.connection, kPropertiesMatch, nullptr);

    auto *msg = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                             "org.freedesktop.DBus", "ListNames");
    call_async(msg, "", on_list_names);
}

void mpris::stop() {
    auto &s = state();
    auto pending = std::move(s.pending);
    s.pending.clear();
    for (auto *call : pending) {
        dbus_pending_call_cancel(call);
        dbus_pending_call_unref(call);
    }
    if (s.connection) {
        dbus_bus_remove_match(s.connection, kNameOwnerMatch, nullptr);
        dbus_bus_remove_match(s.connection, kPropertiesMatch, nullptr);
        dbus_connection_remove_filter(s.connection, filter, nullptr);
    }
    s.connection = nullptr;
    s.players.clear();
}

void mpris::refresh() {
    ensure_started();
    if (!state().connection)
        return;
    for (const auto &p : state().players) {
        request_properties(p.name, kPlayerIface);
        if (p.pid <= 0)
            bus_call(p.name, "GetConnectionUnixProcessID", on_pid);
    }
}

const std::vector<mpris::Player> &mpris::players() {
    return state().players;
}

void mpris::set_on_change(std::function<void()> callback) {
    state().on_change = std::move(callback);
}

void mpris::play_pause(const std::string &name) {
    send_command(name, "PlayPause");
}

void mpris::next(const std::string &name) {
    send_command(name, "Next");
}

void mpris::previous(const std::string &name) {
    send_command(name, "Previous");
}
