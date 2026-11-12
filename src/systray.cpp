#include "systray.h"

#include "dock/dock.h"
#include "hypriso.h"
#include "simple_dbus.h"

#include <dbus/dbus.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>

/*************************************************
 *
 * Shared item list (read by the docks)
 *
 *************************************************/

static std::mutex views_mutex;
static std::vector<TrayItemView> views;
static unsigned long views_revision = 0;
static unsigned long icon_revision_counter = 0;
static bool running = false;
// Bumped by stop() so late D-Bus replies and timers from a previous run do nothing.
static unsigned long generation = 0;

static void publish();

std::vector<TrayItemView> systray::snapshot(unsigned long *revision) {
    std::lock_guard<std::mutex> lock(views_mutex);
    if (revision)
        *revision = views_revision;
    return views;
}

unsigned long systray::revision() {
    std::lock_guard<std::mutex> lock(views_mutex);
    return views_revision;
}

/*************************************************
 *
 * D-Bus helpers
 *
 *************************************************/

static const char *WATCHER_PATH = "/StatusNotifierWatcher";
static const char *KDE_WATCHER = "org.kde.StatusNotifierWatcher";
static const char *FDO_WATCHER = "org.freedesktop.StatusNotifierWatcher";
static const char *KDE_ITEM = "org.kde.StatusNotifierItem";
static const char *FDO_ITEM = "org.freedesktop.StatusNotifierItem";
static const char *DBUSMENU = "com.canonical.dbusmenu";

static std::vector<DBusPendingCall *> pending_calls;

struct PendingData {
    unsigned long generation = 0;
    std::function<void(DBusMessage *)> callback;
};

static void pending_notify(DBusPendingCall *pending, void *user_data) {
    auto data = (PendingData *) user_data;
    std::erase(pending_calls, pending);
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);
    if (data->generation == generation && running && data->callback)
        data->callback(reply);
    if (reply)
        dbus_message_unref(reply);
    dbus_pending_call_unref(pending);
}

static void free_pending_data(void *user_data) {
    delete (PendingData *) user_data;
}

// Sends `message` (and unrefs it); `callback` gets the reply, or nullptr/an error message.
static void call_async(DBusMessage *message, std::function<void(DBusMessage *)> callback, int timeout_ms = 3000) {
    if (!message)
        return;
    if (!dbus_connection_session) {
        dbus_message_unref(message);
        return;
    }
    DBusPendingCall *pending = nullptr;
    if (!dbus_connection_send_with_reply(dbus_connection_session, message, &pending, timeout_ms) || !pending) {
        dbus_message_unref(message);
        return;
    }
    dbus_message_unref(message);
    auto data = new PendingData;
    data->generation = generation;
    data->callback = std::move(callback);
    if (!dbus_pending_call_set_notify(pending, pending_notify, data, free_pending_data)) {
        delete data;
        dbus_pending_call_cancel(pending);
        dbus_pending_call_unref(pending);
        return;
    }
    pending_calls.push_back(pending);
}

static void send_no_reply(DBusMessage *message) {
    if (!message)
        return;
    if (dbus_connection_session) {
        dbus_message_set_no_reply(message, TRUE);
        dbus_connection_send(dbus_connection_session, message, nullptr);
    }
    dbus_message_unref(message);
}

static bool is_error(DBusMessage *reply) {
    return !reply || dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR;
}

static std::string iter_string(DBusMessageIter *it) {
    int type = dbus_message_iter_get_arg_type(it);
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter inner;
        dbus_message_iter_recurse(it, &inner);
        return iter_string(&inner);
    }
    if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
        const char *value = nullptr;
        dbus_message_iter_get_basic(it, &value);
        return value ? value : "";
    }
    return "";
}

static bool iter_bool(DBusMessageIter *it, bool fallback) {
    int type = dbus_message_iter_get_arg_type(it);
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter inner;
        dbus_message_iter_recurse(it, &inner);
        return iter_bool(&inner, fallback);
    }
    if (type == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t value = FALSE;
        dbus_message_iter_get_basic(it, &value);
        return value;
    }
    return fallback;
}

static int iter_int(DBusMessageIter *it, int fallback) {
    int type = dbus_message_iter_get_arg_type(it);
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter inner;
        dbus_message_iter_recurse(it, &inner);
        return iter_int(&inner, fallback);
    }
    if (type == DBUS_TYPE_INT32 || type == DBUS_TYPE_UINT32) {
        dbus_int32_t value = 0;
        dbus_message_iter_get_basic(it, &value);
        return value;
    }
    return fallback;
}

// a(iiay): picks the largest icon and converts it from network byte order ARGB.
static TrayIconPixels iter_pixmaps(DBusMessageIter *it) {
    TrayIconPixels best;
    if (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_VARIANT) {
        DBusMessageIter inner;
        dbus_message_iter_recurse(it, &inner);
        return iter_pixmaps(&inner);
    }
    if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY)
        return best;
    DBusMessageIter array;
    dbus_message_iter_recurse(it, &array);
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
        DBusMessageIter fields;
        dbus_message_iter_recurse(&array, &fields);
        int w = iter_int(&fields, 0);
        dbus_message_iter_next(&fields);
        int h = iter_int(&fields, 0);
        dbus_message_iter_next(&fields);
        if (w > 0 && h > 0 && w <= 1024 && h <= 1024 && w * h > best.w * best.h &&
            dbus_message_iter_get_arg_type(&fields) == DBUS_TYPE_ARRAY) {
            DBusMessageIter bytes_iter;
            dbus_message_iter_recurse(&fields, &bytes_iter);
            const unsigned char *bytes = nullptr;
            int length = 0;
            dbus_message_iter_get_fixed_array(&bytes_iter, &bytes, &length);
            if (bytes && length >= w * h * 4) {
                best.w = w;
                best.h = h;
                best.argb.resize((size_t) w * h);
                for (int i = 0; i < w * h; i++) {
                    uint32_t a = bytes[i * 4 + 0], r = bytes[i * 4 + 1], g = bytes[i * 4 + 2], b = bytes[i * 4 + 3];
                    r = (r * a + 127) / 255;
                    g = (g * a + 127) / 255;
                    b = (b * a + 127) / 255;
                    best.argb[i] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }
        }
        dbus_message_iter_next(&array);
    }
    return best;
}

/*************************************************
 *
 * StatusNotifierItem
 *
 *************************************************/

struct SniItem {
    std::string key;
    std::string service; // bus name the item was registered with
    std::string path;
    std::string owner;   // unique name, which is what signals come from
    std::string interface = KDE_ITEM;

    std::string title;
    std::string status;
    std::string icon_name;
    std::string attention_icon_name;
    std::string theme_path;
    std::string menu_path;
    TrayIconPixels pixmap;
    TrayIconPixels attention_pixmap;
    bool item_is_menu = false;
    bool loaded = false;
    bool fetching = false;
    bool fetch_again = false;
    unsigned long icon_revision = 0;
};

static std::vector<std::unique_ptr<SniItem>> sni_items;
static bool owns_watcher = false;
static bool filter_added = false;
static bool watcher_path_registered = false;
static std::string host_name;
static std::string external_watcher; // well-known name of someone else's watcher when we are only a host

static SniItem *find_sni(const std::string &key) {
    for (auto &item : sni_items)
        if (item->key == key)
            return item.get();
    return nullptr;
}

static void emit_watcher_signal(const char *member, const std::string *argument) {
    if (!owns_watcher || !dbus_connection_session)
        return;
    for (auto interface : {KDE_WATCHER, FDO_WATCHER}) {
        auto signal = dbus_message_new_signal(WATCHER_PATH, interface, member);
        if (!signal)
            continue;
        if (argument) {
            const char *value = argument->c_str();
            dbus_message_append_args(signal, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
        }
        dbus_connection_send(dbus_connection_session, signal, nullptr);
        dbus_message_unref(signal);
    }
}

static void fetch_sni_properties(SniItem *item);

static void parse_sni_properties(SniItem *item, DBusMessage *reply) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(reply, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter dict;
    dbus_message_iter_recurse(&iter, &dict);
    item->title.clear();
    item->menu_path.clear();
    item->pixmap = {};
    item->attention_pixmap = {};
    item->icon_name.clear();
    item->attention_icon_name.clear();
    item->theme_path.clear();
    item->item_is_menu = false;
    std::string id, tooltip;
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&dict, &entry);
        std::string name = iter_string(&entry);
        dbus_message_iter_next(&entry);
        if (name == "Id")
            id = iter_string(&entry);
        else if (name == "Title")
            item->title = iter_string(&entry);
        else if (name == "Status")
            item->status = iter_string(&entry);
        else if (name == "IconName")
            item->icon_name = iter_string(&entry);
        else if (name == "AttentionIconName")
            item->attention_icon_name = iter_string(&entry);
        else if (name == "IconThemePath")
            item->theme_path = iter_string(&entry);
        else if (name == "Menu")
            item->menu_path = iter_string(&entry);
        else if (name == "ItemIsMenu")
            item->item_is_menu = iter_bool(&entry, false);
        else if (name == "IconPixmap")
            item->pixmap = iter_pixmaps(&entry);
        else if (name == "AttentionIconPixmap")
            item->attention_pixmap = iter_pixmaps(&entry);
        else if (name == "ToolTip") {
            // (sa(iiay)ss): icon, pixmaps, title, description
            DBusMessageIter variant;
            dbus_message_iter_recurse(&entry, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRUCT) {
                DBusMessageIter fields;
                dbus_message_iter_recurse(&variant, &fields);
                dbus_message_iter_next(&fields);
                dbus_message_iter_next(&fields);
                tooltip = iter_string(&fields);
            }
        }
        dbus_message_iter_next(&dict);
    }
    if (item->menu_path == "/")
        item->menu_path.clear();
    if (item->title.empty())
        item->title = !tooltip.empty() ? tooltip : id;
    if (item->status.empty())
        item->status = "Active";
    item->icon_revision = ++icon_revision_counter;
}

static void fetch_sni_properties(SniItem *item) {
    if (item->fetching) {
        item->fetch_again = true;
        return;
    }
    item->fetching = true;
    auto message = dbus_message_new_method_call(item->service.c_str(), item->path.c_str(), "org.freedesktop.DBus.Properties", "GetAll");
    if (!message)
        return;
    const char *interface = item->interface.c_str();
    dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID);
    std::string key = item->key;
    call_async(message, [key](DBusMessage *reply) {
        auto item = find_sni(key);
        if (!item)
            return;
        item->fetching = false;
        if (is_error(reply)) {
            // Some items only implement the freedesktop flavor of the interface.
            if (!item->loaded && item->interface == KDE_ITEM) {
                item->interface = FDO_ITEM;
                fetch_sni_properties(item);
            }
            return;
        }
        parse_sni_properties(item, reply);
        item->loaded = true;
        publish();
        if (item->fetch_again) {
            item->fetch_again = false;
            fetch_sni_properties(item);
        }
    });
}

static void resolve_owner(SniItem *item) {
    if (item->service.starts_with(':')) {
        item->owner = item->service;
        return;
    }
    auto message = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner");
    if (!message)
        return;
    const char *name = item->service.c_str();
    dbus_message_append_args(message, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    std::string key = item->key;
    call_async(message, [key](DBusMessage *reply) {
        auto item = find_sni(key);
        if (!item || is_error(reply))
            return;
        const char *owner = nullptr;
        if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) && owner)
            item->owner = owner;
    });
}

// `registered` is what an item passed to RegisterStatusNotifierItem (a bus name or an object path),
// or an entry of a watcher's RegisteredStatusNotifierItems ("name" or "name/path").
static void add_sni_item(std::string registered, const char *sender) {
    std::string service, path;
    if (registered.starts_with('/')) {
        service = sender ? sender : "";
        path = registered;
    } else {
        auto slash = registered.find('/');
        service = slash == std::string::npos ? registered : registered.substr(0, slash);
        path = slash == std::string::npos ? "/StatusNotifierItem" : registered.substr(slash);
    }
    if (service.empty())
        return;
    std::string key = service + path;
    if (find_sni(key))
        return;
    auto item = std::make_unique<SniItem>();
    item->key = key;
    item->service = service;
    item->path = path;
    auto raw = item.get();
    sni_items.push_back(std::move(item));
    resolve_owner(raw);
    fetch_sni_properties(raw);
    std::string announced = registered.starts_with('/') ? key : registered;
    emit_watcher_signal("StatusNotifierItemRegistered", &announced);
}

static void remove_sni_items_of(const std::string &name) {
    bool changed = false;
    for (int i = (int) sni_items.size() - 1; i >= 0; i--) {
        auto &item = sni_items[i];
        if (item->service == name || item->owner == name) {
            std::string key = item->key;
            sni_items.erase(sni_items.begin() + i);
            emit_watcher_signal("StatusNotifierItemUnregistered", &key);
            changed = true;
        }
    }
    if (changed)
        publish();
}

static DBusHandlerResult watcher_message(DBusConnection *connection, DBusMessage *message, void *) {
    static const char *introspection =
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
        "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node>\n"
        " <interface name=\"org.kde.StatusNotifierWatcher\">\n"
        "  <method name=\"RegisterStatusNotifierItem\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>\n"
        "  <method name=\"RegisterStatusNotifierHost\"><arg name=\"service\" type=\"s\" direction=\"in\"/></method>\n"
        "  <property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>\n"
        "  <property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>\n"
        "  <property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>\n"
        "  <signal name=\"StatusNotifierItemRegistered\"><arg type=\"s\"/></signal>\n"
        "  <signal name=\"StatusNotifierItemUnregistered\"><arg type=\"s\"/></signal>\n"
        "  <signal name=\"StatusNotifierHostRegistered\"/>\n"
        "  <signal name=\"StatusNotifierHostUnregistered\"/>\n"
        " </interface>\n"
        " <interface name=\"org.freedesktop.DBus.Properties\">\n"
        "  <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
        "  <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
        " </interface>\n"
        " <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
        "  <method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method>\n"
        " </interface>\n"
        "</node>\n";

    auto reply_empty = [&]() {
        auto reply = dbus_message_new_method_return(message);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
    };
    auto append_property = [](DBusMessageIter *iter, const std::string &name) {
        if (name == "RegisteredStatusNotifierItems") {
            DBusMessageIter variant, array;
            dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "as", &variant);
            dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &array);
            for (auto &item : sni_items) {
                const char *value = item->key.c_str();
                dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &value);
            }
            dbus_message_iter_close_container(&variant, &array);
            dbus_message_iter_close_container(iter, &variant);
        } else if (name == "IsStatusNotifierHostRegistered") {
            DBusMessageIter variant;
            dbus_bool_t value = TRUE;
            dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
            dbus_message_iter_close_container(iter, &variant);
        } else {
            DBusMessageIter variant;
            dbus_int32_t value = 0;
            dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "i", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &value);
            dbus_message_iter_close_container(iter, &variant);
        }
    };

    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect")) {
        auto reply = dbus_message_new_method_return(message);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &introspection, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "Get")) {
        const char *interface = nullptr, *property = nullptr;
        if (!dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID) || !property)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        std::string name = property;
        if (name != "RegisteredStatusNotifierItems" && name != "IsStatusNotifierHostRegistered" && name != "ProtocolVersion") {
            auto error = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
            dbus_connection_send(connection, error, nullptr);
            dbus_message_unref(error);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        auto reply = dbus_message_new_method_return(message);
        DBusMessageIter iter;
        dbus_message_iter_init_append(reply, &iter);
        append_property(&iter, name);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "GetAll")) {
        auto reply = dbus_message_new_method_return(message);
        DBusMessageIter iter, dict;
        dbus_message_iter_init_append(reply, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
        for (auto name : {"RegisteredStatusNotifierItems", "IsStatusNotifierHostRegistered", "ProtocolVersion"}) {
            DBusMessageIter entry;
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
            append_property(&entry, name);
            dbus_message_iter_close_container(&dict, &entry);
        }
        dbus_message_iter_close_container(&iter, &dict);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    for (auto interface : {KDE_WATCHER, FDO_WATCHER}) {
        if (dbus_message_is_method_call(message, interface, "RegisterStatusNotifierItem")) {
            const char *service = nullptr;
            if (dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID) && service)
                add_sni_item(service, dbus_message_get_sender(message));
            reply_empty();
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (dbus_message_is_method_call(message, interface, "RegisterStatusNotifierHost")) {
            reply_empty();
            emit_watcher_signal("StatusNotifierHostRegistered", nullptr);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult sni_signal_filter(DBusConnection *, DBusMessage *message, void *) {
    if (!running || dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        if (dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING, &new_owner,
                                  DBUS_TYPE_INVALID) && name && new_owner && !*new_owner) {
            remove_sni_items_of(name);
            if (old_owner && *old_owner)
                remove_sni_items_of(old_owner);
            // The watcher we were a host of went away: try to take over.
            if (!external_watcher.empty() && external_watcher == name)
                main_thread([]() {
                    if (running) {
                        systray::stop();
                        systray::start();
                    }
                });
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *interface = dbus_message_get_interface(message);
    const char *member = dbus_message_get_member(message);
    if (!interface || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    std::string iface = interface;

    // Someone else's watcher announcing items (host mode).
    if (!owns_watcher && (iface == KDE_WATCHER || iface == FDO_WATCHER)) {
        const char *service = nullptr;
        if (dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID) && service) {
            if (std::string(member) == "StatusNotifierItemRegistered") {
                add_sni_item(service, nullptr);
            } else if (std::string(member) == "StatusNotifierItemUnregistered") {
                std::string key = service;
                if (key.find('/') == std::string::npos)
                    key += "/StatusNotifierItem";
                std::erase_if(sni_items, [&](const auto &item) { return item->key == key; });
                publish();
            }
        }
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (iface == KDE_ITEM || iface == FDO_ITEM) {
        const char *sender = dbus_message_get_sender(message);
        const char *path = dbus_message_get_path(message);
        if (!sender || !path)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        for (auto &item : sni_items) {
            if ((item->owner == sender || item->service == sender) && item->path == path) {
                if (std::string(member) == "NewStatus") {
                    const char *status = nullptr;
                    if (dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &status, DBUS_TYPE_INVALID) && status) {
                        item->status = status;
                        publish();
                    }
                } else {
                    fetch_sni_properties(item.get());
                }
            }
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void start_host_of_external_watcher() {
    // Somebody else (Plasma, waybar, ...) is the watcher: register with them and list what they know.
    external_watcher = KDE_WATCHER;
    auto message = dbus_message_new_method_call(KDE_WATCHER, WATCHER_PATH, KDE_WATCHER, "RegisterStatusNotifierHost");
    if (message) {
        const char *name = host_name.c_str();
        dbus_message_append_args(message, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
        call_async(message, nullptr);
    }
    message = dbus_message_new_method_call(KDE_WATCHER, WATCHER_PATH, "org.freedesktop.DBus.Properties", "Get");
    if (!message)
        return;
    const char *interface = KDE_WATCHER;
    const char *property = "RegisteredStatusNotifierItems";
    dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);
    call_async(message, [](DBusMessage *reply) {
        if (is_error(reply))
            return;
        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
            return;
        DBusMessageIter variant;
        dbus_message_iter_recurse(&iter, &variant);
        if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY)
            return;
        DBusMessageIter array;
        dbus_message_iter_recurse(&variant, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
            add_sni_item(iter_string(&array), nullptr);
            dbus_message_iter_next(&array);
        }
    });
}

static void sni_start() {
    if (!dbus_connection_session)
        return;
    auto connection = dbus_connection_session;
    if (!filter_added)
        filter_added = dbus_connection_add_filter(connection, sni_signal_filter, nullptr, nullptr);
    for (auto rule : {"type='signal',interface='org.kde.StatusNotifierItem'",
                      "type='signal',interface='org.freedesktop.StatusNotifierItem'",
                      "type='signal',interface='org.kde.StatusNotifierWatcher'",
                      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'"})
        dbus_bus_add_match(connection, rule, nullptr);

    host_name = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
    dbus_bus_request_name(connection, host_name.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE, nullptr);

    static const DBusObjectPathVTable vtable = {.message_function = watcher_message};
    if (!watcher_path_registered)
        watcher_path_registered = dbus_connection_register_object_path(connection, WATCHER_PATH, &vtable, nullptr);

    DBusError error = DBUS_ERROR_INIT;
    int result = dbus_bus_request_name(connection, KDE_WATCHER, DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    dbus_error_free(&error);
    owns_watcher = result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER || result == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER;
    if (owns_watcher) {
        external_watcher.clear();
        dbus_bus_request_name(connection, FDO_WATCHER, DBUS_NAME_FLAG_DO_NOT_QUEUE, nullptr);
        emit_watcher_signal("StatusNotifierHostRegistered", nullptr);
    } else {
        start_host_of_external_watcher();
    }
}

static void sni_stop() {
    sni_items.clear();
    if (!dbus_connection_session)
        return;
    auto connection = dbus_connection_session;
    if (owns_watcher) {
        emit_watcher_signal("StatusNotifierHostUnregistered", nullptr);
        dbus_bus_release_name(connection, KDE_WATCHER, nullptr);
        dbus_bus_release_name(connection, FDO_WATCHER, nullptr);
    }
    owns_watcher = false;
    external_watcher.clear();
    if (!host_name.empty())
        dbus_bus_release_name(connection, host_name.c_str(), nullptr);
    if (watcher_path_registered)
        dbus_connection_unregister_object_path(connection, WATCHER_PATH);
    watcher_path_registered = false;
    if (filter_added)
        dbus_connection_remove_filter(connection, sni_signal_filter, nullptr);
    filter_added = false;
}

/*************************************************
 *
 * com.canonical.dbusmenu
 *
 *************************************************/

static std::string strip_mnemonic(const std::string &label) {
    std::string out;
    for (size_t i = 0; i < label.size(); i++) {
        if (label[i] == '_') {
            if (i + 1 < label.size() && label[i + 1] == '_') {
                out += '_';
                i++;
            }
            continue;
        }
        out += label[i];
    }
    return out;
}

// (ia{sv}av)
static TrayMenuItem parse_menu_node(DBusMessageIter *structure) {
    TrayMenuItem item;
    DBusMessageIter fields;
    dbus_message_iter_recurse(structure, &fields);
    item.id = iter_int(&fields, 0);
    dbus_message_iter_next(&fields);
    if (dbus_message_iter_get_arg_type(&fields) == DBUS_TYPE_ARRAY) {
        DBusMessageIter props;
        dbus_message_iter_recurse(&fields, &props);
        while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&props, &entry);
            std::string name = iter_string(&entry);
            dbus_message_iter_next(&entry);
            if (name == "label")
                item.label = strip_mnemonic(iter_string(&entry));
            else if (name == "type")
                item.separator = iter_string(&entry) == "separator";
            else if (name == "enabled")
                item.enabled = iter_bool(&entry, true);
            else if (name == "visible")
                item.visible = iter_bool(&entry, true);
            else if (name == "toggle-type")
                item.toggle_type = iter_string(&entry);
            else if (name == "toggle-state")
                item.toggle_state = iter_int(&entry, -1);
            dbus_message_iter_next(&props);
        }
    }
    dbus_message_iter_next(&fields);
    if (dbus_message_iter_get_arg_type(&fields) == DBUS_TYPE_ARRAY) {
        DBusMessageIter children;
        dbus_message_iter_recurse(&fields, &children);
        while (dbus_message_iter_get_arg_type(&children) == DBUS_TYPE_VARIANT) {
            DBusMessageIter variant;
            dbus_message_iter_recurse(&children, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRUCT)
                item.children.push_back(parse_menu_node(&variant));
            dbus_message_iter_next(&children);
        }
    }
    return item;
}

void systray::fetch_menu(const std::string &key, std::function<void(std::vector<TrayMenuItem>)> on_ready) {
    auto item = find_sni(key);
    if (!item || item->menu_path.empty()) {
        if (on_ready)
            on_ready({});
        return;
    }
    // Lets the app refresh the menu before it is shown.
    auto about = dbus_message_new_method_call(item->service.c_str(), item->menu_path.c_str(), DBUSMENU, "AboutToShow");
    if (about) {
        dbus_int32_t root_id = 0;
        dbus_message_append_args(about, DBUS_TYPE_INT32, &root_id, DBUS_TYPE_INVALID);
        call_async(about, nullptr);
    }
    auto message = dbus_message_new_method_call(item->service.c_str(), item->menu_path.c_str(), DBUSMENU, "GetLayout");
    if (!message) {
        if (on_ready)
            on_ready({});
        return;
    }
    dbus_int32_t parent = 0, depth = -1;
    DBusMessageIter iter, names;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &parent);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &depth);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "s", &names);
    dbus_message_iter_close_container(&iter, &names);
    call_async(message, [on_ready](DBusMessage *reply) {
        std::vector<TrayMenuItem> items;
        DBusMessageIter iter;
        if (!is_error(reply) && dbus_message_iter_init(reply, &iter)) {
            dbus_message_iter_next(&iter); // revision
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRUCT)
                items = parse_menu_node(&iter).children;
        }
        if (on_ready)
            on_ready(std::move(items));
    });
}

void systray::menu_clicked(const std::string &key, int id) {
    auto item = find_sni(key);
    if (!item || item->menu_path.empty())
        return;
    auto message = dbus_message_new_method_call(item->service.c_str(), item->menu_path.c_str(), DBUSMENU, "Event");
    if (!message)
        return;
    dbus_int32_t item_id = id;
    const char *event = "clicked";
    dbus_uint32_t timestamp = 0;
    DBusMessageIter iter, variant;
    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &item_id);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &event);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "i", &variant);
    dbus_int32_t data = 0;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &data);
    dbus_message_iter_close_container(&iter, &variant);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &timestamp);
    call_async(message, nullptr);
}

/*************************************************
 *
 * XEmbed system tray (Xwayland)
 *
 * Runs on its own thread: X round trips must never stall the compositor,
 * since Xwayland is itself a client of the compositor.
 *
 *************************************************/

static constexpr int XEMBED_ICON_SIZE = 24;
// Containers live far outside every monitor; Hyprland shows unmanaged X windows where they are.
static constexpr int XEMBED_OFFSCREEN = -30000;

struct XembedIcon {
    xcb_window_t window = 0;
    xcb_window_t container = 0;
    xcb_damage_damage_t damage = 0;
    bool mapped = true;
    long capture_due = 0; // 0 when no capture is scheduled
    std::string title;
    TrayIconPixels pixels;
    unsigned long icon_revision = 0;
};

struct XembedRequest {
    xcb_window_t window = 0;
    int button = 1;
};

// Shared between the X thread and the main thread.
static std::mutex xembed_mutex;
static std::vector<TrayItemView> xembed_views;
static std::vector<XembedRequest> xembed_requests;
static std::thread xembed_thread;
static std::atomic<bool> xembed_stop_requested = false;
static std::atomic<bool> xembed_running = false;
static int xembed_wake[2] = {-1, -1};
static int xembed_attempts = 0;

// Only touched by the X thread.
static xcb_connection_t *xconn = nullptr;
static xcb_screen_t *xscreen = nullptr;
static xcb_window_t owner_window = 0;
static xcb_atom_t atom_selection = 0, atom_opcode = 0, atom_manager = 0, atom_xembed = 0, atom_orientation = 0, atom_visual = 0,
                  atom_net_wm_name = 0, atom_utf8 = 0, atom_opacity = 0;
static bool have_damage = false;
static bool have_composite = false;
static uint8_t damage_event_base = 0;
static std::vector<XembedIcon> xicons;
static std::atomic<unsigned long> xembed_revision_counter = 0;

static long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void xembed_publish() {
    std::vector<TrayItemView> list;
    for (auto &icon : xicons) {
        if (!icon.mapped)
            continue;
        TrayItemView view;
        view.key = "xembed:" + std::to_string(icon.window);
        view.xembed = true;
        view.title = icon.title;
        view.pixels = icon.pixels;
        view.revision = icon.icon_revision;
        list.push_back(std::move(view));
    }
    {
        std::lock_guard<std::mutex> lock(xembed_mutex);
        xembed_views = std::move(list);
    }
    auto gen = generation;
    main_thread([gen]() {
        if (gen == generation && running)
            publish();
    });
}

static xcb_atom_t intern(const char *name) {
    auto reply = xcb_intern_atom_reply(xconn, xcb_intern_atom(xconn, 0, strlen(name), name), nullptr);
    xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

static XembedIcon *find_xicon(xcb_window_t window) {
    for (auto &icon : xicons)
        if (icon.window == window || icon.container == window)
            return &icon;
    return nullptr;
}

static std::string read_text_property(xcb_window_t window, xcb_atom_t property, xcb_atom_t type) {
    auto reply = xcb_get_property_reply(xconn, xcb_get_property(xconn, 0, window, property, type, 0, 256), nullptr);
    std::string text;
    if (reply && xcb_get_property_value_length(reply) > 0)
        text.assign((const char *) xcb_get_property_value(reply), xcb_get_property_value_length(reply));
    free(reply);
    return text;
}

static void read_xicon_title(XembedIcon &icon) {
    icon.title = read_text_property(icon.window, atom_net_wm_name, atom_utf8);
    if (icon.title.empty())
        icon.title = read_text_property(icon.window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING);
    if (icon.title.empty()) {
        // WM_CLASS is "instance\0class\0"
        auto wm_class = read_text_property(icon.window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
        auto first = wm_class.find('\0');
        icon.title = first == std::string::npos ? wm_class : wm_class.substr(first + 1);
        while (!icon.title.empty() && icon.title.back() == '\0')
            icon.title.pop_back();
    }
}

static void capture_xicon(XembedIcon &icon) {
    icon.capture_due = 0;
    auto geometry = xcb_get_geometry_reply(xconn, xcb_get_geometry(xconn, icon.window), nullptr);
    if (!geometry)
        return;
    int w = geometry->width, h = geometry->height, depth = geometry->depth;
    free(geometry);
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || (depth != 24 && depth != 32))
        return;
    auto image = xcb_get_image_reply(xconn, xcb_get_image(xconn, XCB_IMAGE_FORMAT_Z_PIXMAP, icon.window, 0, 0, w, h, ~0u), nullptr);
    if (!image)
        return;
    auto data = xcb_get_image_data(image);
    int length = xcb_get_image_data_length(image);
    if (data && length >= w * h * 4) {
        icon.pixels.w = w;
        icon.pixels.h = h;
        icon.pixels.argb.resize((size_t) w * h);
        memcpy(icon.pixels.argb.data(), data, (size_t) w * h * 4);
        if (depth == 24)
            for (auto &pixel : icon.pixels.argb)
                pixel |= 0xff000000;
        icon.icon_revision = ++xembed_revision_counter;
    }
    free(image);
}

static void schedule_xicon_capture(XembedIcon &icon, int delay_ms = 60) {
    if (!icon.capture_due)
        icon.capture_due = now_ms() + delay_ms;
}

static void send_xembed_message(xcb_window_t window, uint32_t message, uint32_t detail, uint32_t data1, uint32_t data2) {
    xcb_client_message_event_t event{};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.window = window;
    event.type = atom_xembed;
    event.data.data32[0] = XCB_CURRENT_TIME;
    event.data.data32[1] = message;
    event.data.data32[2] = detail;
    event.data.data32[3] = data1;
    event.data.data32[4] = data2;
    xcb_send_event(xconn, 0, window, XCB_EVENT_MASK_NO_EVENT, (const char *) &event);
}

static void dock_xicon(xcb_window_t window) {
    if (!window || find_xicon(window))
        return;
    auto attributes = xcb_get_window_attributes_reply(xconn, xcb_get_window_attributes(xconn, window), nullptr);
    if (!attributes)
        return;
    free(attributes);

    XembedIcon icon;
    icon.window = window;
    icon.container = xcb_generate_id(xconn);
    uint32_t values[] = {xscreen->black_pixel, 1, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY};
    xcb_create_window(xconn, XCB_COPY_FROM_PARENT, icon.container, xscreen->root, XEMBED_OFFSCREEN, XEMBED_OFFSCREEN, XEMBED_ICON_SIZE,
                      XEMBED_ICON_SIZE, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    uint32_t transparent = 0;
    xcb_change_property(xconn, XCB_PROP_MODE_REPLACE, icon.container, atom_opacity, XCB_ATOM_CARDINAL, 32, 1, &transparent);
    // Mylar skips windows of this class (see on_open_window in hypriso.cpp).
    const char wm_class[] = "mylar-xembed-tray\0mylar-xembed-tray";
    xcb_change_property(xconn, XCB_PROP_MODE_REPLACE, icon.container, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(wm_class), wm_class);

    uint32_t events = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(xconn, window, XCB_CW_EVENT_MASK, &events);
    // Keep the icon's contents in an off-screen pixmap so they can be read back wherever it sits.
    if (have_composite)
        xcb_composite_redirect_window(xconn, window, XCB_COMPOSITE_REDIRECT_MANUAL);
    xcb_reparent_window(xconn, window, icon.container, 0, 0);
    uint32_t size[] = {XEMBED_ICON_SIZE, XEMBED_ICON_SIZE};
    xcb_configure_window(xconn, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    send_xembed_message(window, 0 /* XEMBED_EMBEDDED_NOTIFY */, 0, icon.container, 0);
    xcb_map_window(xconn, window);
    xcb_map_window(xconn, icon.container);
    if (have_damage) {
        icon.damage = xcb_generate_id(xconn);
        xcb_damage_create(xconn, icon.damage, window, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
    }
    read_xicon_title(icon);
    xcb_flush(xconn);
    schedule_xicon_capture(icon, 150);
    xicons.push_back(icon);
    xembed_publish();
}

static void forget_xicon(xcb_window_t window, bool window_gone) {
    for (int i = (int) xicons.size() - 1; i >= 0; i--) {
        auto &icon = xicons[i];
        if (icon.window != window)
            continue;
        if (!window_gone) {
            if (icon.damage)
                xcb_damage_destroy(xconn, icon.damage);
            xcb_unmap_window(xconn, icon.window);
            xcb_reparent_window(xconn, icon.window, xscreen->root, 0, 0);
        }
        xcb_destroy_window(xconn, icon.container);
        xicons.erase(xicons.begin() + i);
        xcb_flush(xconn);
        xembed_publish();
        return;
    }
}

static void xembed_click(XembedIcon &icon, int button) {
    int x = XEMBED_ICON_SIZE / 2, y = XEMBED_ICON_SIZE / 2;
    for (int i = 0; i < 2; i++) {
        xcb_button_press_event_t event{};
        event.response_type = i == 0 ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE;
        event.detail = button;
        event.time = XCB_CURRENT_TIME;
        event.root = xscreen->root;
        event.event = icon.window;
        event.child = XCB_NONE;
        event.root_x = XEMBED_OFFSCREEN + x;
        event.root_y = XEMBED_OFFSCREEN + y;
        event.event_x = x;
        event.event_y = y;
        event.state = i == 0 ? 0 : (uint16_t) (XCB_BUTTON_MASK_1 << (button - 1));
        event.same_screen = 1;
        xcb_send_event(xconn, 0, icon.window, i == 0 ? XCB_EVENT_MASK_BUTTON_PRESS : XCB_EVENT_MASK_BUTTON_RELEASE, (const char *) &event);
    }
    xcb_flush(xconn);
}

// Returns false when the thread should exit (another tray took the selection).
static bool handle_x_event(xcb_generic_event_t *event) {
    uint8_t type = event->response_type & ~0x80;
    if (have_damage && type == damage_event_base + XCB_DAMAGE_NOTIFY) {
        auto damage = (xcb_damage_notify_event_t *) event;
        if (auto icon = find_xicon(damage->drawable)) {
            xcb_damage_subtract(xconn, damage->damage, XCB_NONE, XCB_NONE);
            schedule_xicon_capture(*icon);
        }
        return true;
    }
    switch (type) {
        case XCB_CLIENT_MESSAGE: {
            auto message = (xcb_client_message_event_t *) event;
            if (message->type == atom_opcode && message->data.data32[1] == 0 /* SYSTEM_TRAY_REQUEST_DOCK */)
                dock_xicon(message->data.data32[2]);
            break;
        }
        case XCB_DESTROY_NOTIFY: {
            auto destroy = (xcb_destroy_notify_event_t *) event;
            if (auto icon = find_xicon(destroy->window); icon && icon->window == destroy->window)
                forget_xicon(destroy->window, true);
            break;
        }
        case XCB_REPARENT_NOTIFY: {
            auto reparent = (xcb_reparent_notify_event_t *) event;
            if (auto icon = find_xicon(reparent->window); icon && icon->window == reparent->window && reparent->parent != icon->container)
                forget_xicon(reparent->window, true);
            break;
        }
        case XCB_MAP_NOTIFY: {
            auto map = (xcb_map_notify_event_t *) event;
            if (auto icon = find_xicon(map->window); icon && icon->window == map->window) {
                icon->mapped = true;
                schedule_xicon_capture(*icon);
                xembed_publish();
            }
            break;
        }
        case XCB_UNMAP_NOTIFY: {
            auto unmap = (xcb_unmap_notify_event_t *) event;
            if (auto icon = find_xicon(unmap->window); icon && icon->window == unmap->window) {
                icon->mapped = false;
                xembed_publish();
            }
            break;
        }
        case XCB_CONFIGURE_NOTIFY: {
            auto configure = (xcb_configure_notify_event_t *) event;
            if (auto icon = find_xicon(configure->window); icon && icon->window == configure->window) {
                if (configure->width != XEMBED_ICON_SIZE || configure->height != XEMBED_ICON_SIZE) {
                    uint32_t size[] = {XEMBED_ICON_SIZE, XEMBED_ICON_SIZE};
                    xcb_configure_window(xconn, icon->window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
                    xcb_flush(xconn);
                }
                schedule_xicon_capture(*icon);
            }
            break;
        }
        case XCB_PROPERTY_NOTIFY: {
            auto property = (xcb_property_notify_event_t *) event;
            if (auto icon = find_xicon(property->window); icon && icon->window == property->window &&
                (property->atom == atom_net_wm_name || property->atom == XCB_ATOM_WM_NAME)) {
                read_xicon_title(*icon);
                xembed_publish();
            }
            break;
        }
        case XCB_SELECTION_CLEAR: {
            auto clear = (xcb_selection_clear_event_t *) event;
            if (clear->selection == atom_selection)
                return false;
            break;
        }
        default:
            break;
    }
    return true;
}

// Becomes the tray selection owner. Returns false if that is not possible (or another tray exists).
static bool xembed_connect() {
    const char *display = getenv("DISPLAY");
    if (!display || !*display)
        return false;
    int screen_number = 0;
    xconn = xcb_connect(display, &screen_number);
    if (!xconn || xcb_connection_has_error(xconn))
        return false;
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(xconn));
    for (int i = 0; i < screen_number && screens.rem; i++)
        xcb_screen_next(&screens);
    xscreen = screens.data;
    if (!xscreen)
        return false;

    std::string selection_name = "_NET_SYSTEM_TRAY_S" + std::to_string(screen_number);
    atom_selection = intern(selection_name.c_str());
    atom_opcode = intern("_NET_SYSTEM_TRAY_OPCODE");
    atom_manager = intern("MANAGER");
    atom_xembed = intern("_XEMBED");
    atom_orientation = intern("_NET_SYSTEM_TRAY_ORIENTATION");
    atom_visual = intern("_NET_SYSTEM_TRAY_VISUAL");
    atom_net_wm_name = intern("_NET_WM_NAME");
    atom_utf8 = intern("UTF8_STRING");
    atom_opacity = intern("_NET_WM_WINDOW_OPACITY");

    // Don't fight an existing tray (e.g. xembed-sni-proxy, whose icons already arrive over SNI).
    auto owner = xcb_get_selection_owner_reply(xconn, xcb_get_selection_owner(xconn, atom_selection), nullptr);
    bool taken = owner && owner->owner != XCB_NONE;
    free(owner);
    if (taken)
        return false;

    owner_window = xcb_generate_id(xconn);
    uint32_t values[] = {1, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_create_window(xconn, XCB_COPY_FROM_PARENT, owner_window, xscreen->root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    uint32_t horizontal = 0;
    xcb_change_property(xconn, XCB_PROP_MODE_REPLACE, owner_window, atom_orientation, XCB_ATOM_CARDINAL, 32, 1, &horizontal);
    // Offer a 32-bit visual so icons can be drawn with real transparency.
    for (auto depths = xcb_screen_allowed_depths_iterator(xscreen); depths.rem; xcb_depth_next(&depths)) {
        if (depths.data->depth != 32)
            continue;
        auto visuals = xcb_depth_visuals_iterator(depths.data);
        if (visuals.rem) {
            uint32_t visual = visuals.data->visual_id;
            xcb_change_property(xconn, XCB_PROP_MODE_REPLACE, owner_window, atom_visual, XCB_ATOM_VISUALID, 32, 1, &visual);
            break;
        }
    }

    xcb_set_selection_owner(xconn, owner_window, atom_selection, XCB_CURRENT_TIME);
    owner = xcb_get_selection_owner_reply(xconn, xcb_get_selection_owner(xconn, atom_selection), nullptr);
    bool ours = owner && owner->owner == owner_window;
    free(owner);
    if (!ours)
        return false;

    if (auto damage = xcb_get_extension_data(xconn, &xcb_damage_id); damage && damage->present) {
        auto version = xcb_damage_query_version_reply(xconn, xcb_damage_query_version(xconn, 1, 1), nullptr);
        have_damage = version != nullptr;
        free(version);
        damage_event_base = damage->first_event;
    }
    if (auto composite = xcb_get_extension_data(xconn, &xcb_composite_id); composite && composite->present) {
        auto version = xcb_composite_query_version_reply(xconn, xcb_composite_query_version(xconn, 0, 4), nullptr);
        have_composite = version != nullptr;
        free(version);
    }

    // Tell icons that are waiting that a tray is here.
    xcb_client_message_event_t manager{};
    manager.response_type = XCB_CLIENT_MESSAGE;
    manager.format = 32;
    manager.window = xscreen->root;
    manager.type = atom_manager;
    manager.data.data32[0] = XCB_CURRENT_TIME;
    manager.data.data32[1] = atom_selection;
    manager.data.data32[2] = owner_window;
    xcb_send_event(xconn, 0, xscreen->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *) &manager);
    xcb_flush(xconn);
    return true;
}

static void xembed_thread_main() {
    if (xembed_connect()) {
        int xfd = xcb_get_file_descriptor(xconn);
        while (!xembed_stop_requested) {
            long now = now_ms();
            long next_due = 0;
            for (auto &icon : xicons)
                if (icon.capture_due && (!next_due || icon.capture_due < next_due))
                    next_due = icon.capture_due;
            int timeout = next_due ? (int) std::clamp(next_due - now, 0l, 1000l) : 1000;
            pollfd fds[2] = {{xfd, POLLIN, 0}, {xembed_wake[0], POLLIN, 0}};
            poll(fds, 2, timeout);
            if (xembed_stop_requested)
                break;
            if (fds[1].revents & POLLIN) {
                char buffer[64];
                while (read(xembed_wake[0], buffer, sizeof buffer) > 0) {}
            }

            std::vector<XembedRequest> requests;
            {
                std::lock_guard<std::mutex> lock(xembed_mutex);
                requests.swap(xembed_requests);
            }
            for (auto &request : requests)
                if (auto icon = find_xicon(request.window))
                    xembed_click(*icon, request.button);

            bool keep_going = true;
            while (auto event = xcb_poll_for_event(xconn)) {
                keep_going = handle_x_event(event) && keep_going;
                free(event);
            }
            if (!keep_going || xcb_connection_has_error(xconn))
                break;

            now = now_ms();
            bool captured = false;
            for (auto &icon : xicons) {
                if (icon.capture_due && icon.capture_due <= now) {
                    capture_xicon(icon);
                    captured = true;
                }
            }
            if (captured)
                xembed_publish();
        }
    }

    if (xconn) {
        if (!xcb_connection_has_error(xconn)) {
            while (!xicons.empty())
                forget_xicon(xicons.back().window, false);
            if (owner_window)
                xcb_destroy_window(xconn, owner_window);
            xcb_flush(xconn);
        }
        xcb_disconnect(xconn);
    }
    xconn = nullptr;
    xscreen = nullptr;
    owner_window = 0;
    have_damage = false;
    have_composite = false;
    xicons.clear();
    xembed_publish();
    xembed_running = false;
}

static void xembed_wake_up() {
    if (xembed_wake[1] != -1)
        write(xembed_wake[1], "x", 1);
}

static void xembed_stop() {
    if (xembed_thread.joinable()) {
        xembed_stop_requested = true;
        xembed_wake_up();
        xembed_thread.join();
    }
    xembed_stop_requested = false;
    for (auto &fd : xembed_wake) {
        if (fd != -1)
            close(fd);
        fd = -1;
    }
    std::lock_guard<std::mutex> lock(xembed_mutex);
    xembed_views.clear();
    xembed_requests.clear();
}

static void xembed_try_start() {
    if (!running || xembed_thread.joinable())
        return;
    // Connecting before Xwayland finished starting could block it forever (it waits on us).
    if (!hypriso->xwayland_ready()) {
        if (++xembed_attempts > 90)
            return;
        auto gen = generation;
        later(2000, [gen](Timer *) {
            if (gen == generation)
                xembed_try_start();
        });
        return;
    }
    if (pipe2(xembed_wake, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    xembed_running = true;
    xembed_thread = std::thread(xembed_thread_main);
}

static void xembed_request(const std::string &key, int button) {
    auto window = (xcb_window_t) std::strtoul(key.c_str() + 7, nullptr, 10);
    {
        std::lock_guard<std::mutex> lock(xembed_mutex);
        xembed_requests.push_back({window, button});
    }
    xembed_wake_up();
}

/*************************************************
 *
 * Public API
 *
 *************************************************/

static void publish() {
    std::vector<TrayItemView> list;
    for (auto &item : sni_items) {
        if (!item->loaded || item->status == "Passive")
            continue;
        TrayItemView view;
        view.key = item->key;
        view.title = item->title;
        view.needs_attention = item->status == "NeedsAttention";
        bool attention = view.needs_attention && (!item->attention_icon_name.empty() || !item->attention_pixmap.argb.empty());
        view.icon_name = attention ? item->attention_icon_name : item->icon_name;
        view.pixels = attention ? item->attention_pixmap : item->pixmap;
        // The dock prefers icon_name (it scales) and falls back to the pixels.
        view.icon_theme_path = item->theme_path;
        view.has_menu = !item->menu_path.empty();
        view.item_is_menu = item->item_is_menu;
        view.revision = item->icon_revision + (attention ? 1000000000ul : 0);
        list.push_back(std::move(view));
    }
    {
        std::lock_guard<std::mutex> lock(xembed_mutex);
        for (auto view : xembed_views) {
            // Keep icon revisions of both protocols apart for the docks' caches.
            view.revision += 1ul << 40;
            list.push_back(std::move(view));
        }
    }
    {
        std::lock_guard<std::mutex> lock(views_mutex);
        views = std::move(list);
        views_revision++;
    }
    dock::change_in_systray();
}

void systray::start() {
    if (running)
        return;
    running = true;
    xembed_attempts = 0;
    sni_start();
    xembed_try_start();
    publish();
}

void systray::stop() {
    if (!running)
        return;
    xembed_stop();
    sni_stop();
    for (auto pending : pending_calls) {
        dbus_pending_call_cancel(pending);
        dbus_pending_call_unref(pending);
    }
    pending_calls.clear();
    running = false;
    generation++;
    publish();
}

void systray::click(const std::string &key, int button) {
    if (key.starts_with("xembed:")) {
        xembed_request(key, button);
        return;
    }
    auto item = find_sni(key);
    if (!item)
        return;
    const char *method = button == 1 ? "Activate" : button == 2 ? "SecondaryActivate" : "ContextMenu";
    auto message = dbus_message_new_method_call(item->service.c_str(), item->path.c_str(), item->interface.c_str(), method);
    if (!message)
        return;
    dbus_int32_t x = 0, y = 0;
    dbus_message_append_args(message, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID);
    call_async(message, nullptr);
}

void systray::scroll(const std::string &key, int delta, bool horizontal) {
    if (key.starts_with("xembed:")) {
        xembed_request(key, horizontal ? (delta > 0 ? 7 : 6) : (delta > 0 ? 5 : 4));
        return;
    }
    auto item = find_sni(key);
    if (!item)
        return;
    auto message = dbus_message_new_method_call(item->service.c_str(), item->path.c_str(), item->interface.c_str(), "Scroll");
    if (!message)
        return;
    dbus_int32_t amount = delta;
    const char *orientation = horizontal ? "horizontal" : "vertical";
    dbus_message_append_args(message, DBUS_TYPE_INT32, &amount, DBUS_TYPE_STRING, &orientation, DBUS_TYPE_INVALID);
    send_no_reply(message);
}
