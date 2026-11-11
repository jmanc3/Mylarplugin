//
// Created by jmanc3 on 5/28/21.
//

#ifndef WINBAR_SIMPLE_DBUS_H
#define WINBAR_SIMPLE_DBUS_H

//#include "notifications.h"
#include "dbus/dbus.h"

#include <string>
#include <utility>
#include <vector>
#include <mutex>

struct DBusConnection;
struct App;

struct NotificationInfo;

struct NotificationAction {
    std::string id;
    std::string label;
    
    void (*callback)(NotificationInfo *) = nullptr;
};

enum NotificationReasonClosed {
    EXPIRED = 1,
    DISMISSED_BY_USER = 2,
    CLOSED_BY_CLOSE_NOTIFICATION_CALL = 3,
    UNDEFINED_OR_RESERVED_REASON = 4,
};

struct NotificationInfo {
    dbus_uint32_t id = 0;
    std::string time_started;
    std::string icon_path;
    std::string calling_dbus_client;
    bool sent_to_action_center = false;
    bool removed_from_action_center = false;
    bool sent_by_winbar = false;
    
    void (*on_ignore)(NotificationInfo *) = nullptr;
    
    void *user_data = nullptr;
    
    std::string app_name;
    std::string app_icon;
    std::string summary;
    std::string body;
    dbus_int32_t expire_timeout_in_milliseconds = -1; // 0 means never, -1 means the server (us) decides
    
    std::vector<NotificationAction> actions;
    
    std::string x_kde_appname;
    std::string x_kde_origin_name;
    std::string x_kde_display_appname;
    std::string desktop_entry;
    std::string x_kde_eventId;
    std::string x_kde_reply_placeholder_text = "Reply...";
    std::string x_kde_reply_submit_button_text = "Send";
    std::string x_kde_reply_submit_button_icon_name = "document-send";
    std::vector<std::string> x_kde_urls;
};

struct BluetoothRequest {
    DBusConnection *connection;
    DBusMessage *message;
    std::string type;
    std::string pin;
    std::string object_path;
    
    BluetoothRequest(DBusConnection *connection, DBusMessage *message, const std::string &type) {
        this->connection = connection;
        if (connection) dbus_connection_ref(connection);
        this->message = message;
        this->type = type;
        if (this->message)
            dbus_message_ref(message);
    }
    
    ~BluetoothRequest() {
        if (connection) dbus_connection_unref(connection);
        if (this->message)
            dbus_message_unref(message);
    }
};

extern DBusConnection *dbus_connection_session;
extern DBusConnection *dbus_connection_system;

extern std::vector<std::string> running_dbus_services;
struct BluetoothInterface;
extern std::vector<BluetoothInterface *> bluetooth_interfaces;
extern bool bluetooth_running;

extern bool network_manager_running;

struct BatteryStatus {
    bool present = false;
    bool valid = false;
    double percentage = -1;
    double energy = 0;
    double energy_full = 0;
    double energy_rate = 0;
    long long time_to_empty = 0;
    unsigned state = 0;
    bool battery_saver = false;
    bool saver_available = false;
    bool protector_enabled = false;
    bool protector_available = false;
    std::string reading_reason = "Waiting for battery information…";
    std::string saver_reason;
    std::string protector_reason;
    std::string profile_service;
    std::string profile_path;
    std::vector<std::string> battery_paths;
};

// Blocking calls with bounded D-Bus timeouts; invoke from a worker thread.
BatteryStatus dbus_read_battery();
bool dbus_set_battery_saver(bool enabled, std::string &error);
bool dbus_set_battery_protector(bool enabled, std::string &error);

// Wi-Fi backend (ported from winbar's wifi_backend). NetworkManager is used when it
// owns its bus name; otherwise wpa_supplicant's control sockets are used directly.
// Every call blocks with bounded timeouts; use from a worker thread.
struct WifiAccessPoint {
    std::string path;
    std::string ssid;
    std::string bssid;
    int strength = 0;
    unsigned frequency = 0;
    bool secured = false;
    bool enterprise = false;
    bool active = false;
    // "wpa-psk", "sae", "none" (WEP), "owe", "wpa-eap", or empty for an open network.
    std::string key_mgmt;
    // Human readable: "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2/WPA3", "WPA3", "802.1X", "OWE", or empty.
    std::string security;
    std::vector<std::string> saved_connections;
};

struct WifiDevice {
    std::string path;
    std::string interface;
    // NMDeviceState values (wpa_supplicant states are mapped onto them).
    unsigned state = 0;
    unsigned state_reason = 0;
    long long last_scan = -1;
    std::string active_access_point;
};

// The interface carrying the default route, found the way winbar's wifi_state() did
// (route table + /sys/class/net). Independent of NetworkManager / wpa_supplicant.
struct NetworkLink {
    std::string interface;
    bool up = false;
    bool wired = false;
};

// Cheap (a few small file reads); safe to call from any thread.
NetworkLink default_network_link();

struct WifiStatus {
    // Filled even when no Wi-Fi backend is running (e.g. a desktop on Ethernet).
    NetworkLink link;
    bool service_running = false;
    // "NetworkManager" or "wpa_supplicant".
    std::string backend;
    bool wireless_enabled = false;
    bool hardware_enabled = true;
    unsigned state = 0;
    // NMConnectivityState: 0 unknown, 1 none, 2 captive portal, 3 limited, 4 full.
    unsigned connectivity = 0;
    std::string portal_uri;
    std::vector<WifiDevice> devices;
    // Access points seen by `device_path`, one entry per SSID (strongest wins).
    std::string device_path;
    std::vector<WifiAccessPoint> access_points;
};

struct WifiBus;
WifiBus *wifi_backend_open();
void wifi_backend_close(WifiBus *bus);
// An empty or unknown device path selects the first wireless device.
WifiStatus wifi_backend_read(WifiBus *bus, const std::string &device_path);
bool wifi_backend_scan(WifiBus *bus, const std::string &device_path, std::string &error);
bool wifi_backend_set_enabled(WifiBus *bus, const std::string &device_path, bool enabled, std::string &error);
// Activates a saved profile when one exists, otherwise creates one from `password`.
bool wifi_backend_connect(WifiBus *bus, const std::string &device_path, const WifiAccessPoint &ap,
                          const std::string &password, std::string &active_connection,
                          std::string &created_connection, std::string &error);
bool wifi_backend_disconnect(WifiBus *bus, const std::string &device_path, std::string &error);
bool wifi_backend_forget(WifiBus *bus, const std::vector<std::string> &connections, std::string &error);
// NMActiveConnectionState: 1 activating, 2 activated, 3 deactivating, 4 deactivated (0 when gone).
unsigned wifi_backend_active_state(WifiBus *bus, const std::string &active_connection);
// Call once a connection attempt settles (connected, failed or abandoned). For
// wpa_supplicant this re-enables networks SELECT_NETWORK disabled and saves on success.
void wifi_backend_finish(WifiBus *bus, const std::string &active_connection, bool connected);
// Asks NetworkManager to re-probe internet access; returns NMConnectivityState (0 without NM).
unsigned wifi_backend_check_connectivity(WifiBus *bus);

void network_manager_request_scan(std::string device_path);

DBusMessage *get_property(std::string bus_name, std::string path, std::string iface, std::string property_name);

template<typename T>
T get_num_property(std::string bus_name, std::string path, std::string iface, std::string property_name) {
    DBusMessage *iface_reply = get_property(bus_name, path, iface, property_name);
    if (!iface_reply)
        return -1;
    
    // Get the serial in serial_reply
    DBusMessageIter iter4;
    dbus_message_iter_init(iface_reply, &iter4);
    DBusMessageIter iter5;
    dbus_message_iter_recurse(&iter4, &iter5);
    T reply = -1;
    dbus_message_iter_get_basic(&iter5, &reply);
    dbus_message_unref(iface_reply);
    return reply;
}

void network_manager_service_get_all_devices();

bool dbus_startup_ready();
void dbus_start(DBusBusType dbusType);

void dbus_end();

bool dbus_gnome_show_overview();

bool dbus_kde_show_desktop();

bool dbus_kde_show_desktop_grid();

void notification_closed_signal(App *app, NotificationInfo *ni, NotificationReasonClosed reason);

void notification_action_invoked_signal(App *app, NotificationInfo *ni, NotificationAction action);

double dbus_get_kde_max_brightness();

double dbus_get_kde_current_brightness();

void highlight_windows(const std::vector<std::string> &windows);

void highlight_window(std::string window_id_as_string);

/// Number from 0 to 1
bool dbus_kde_set_brightness(double percentage);

bool dbus_kde_running();

bool dbus_gnome_running();

double dbus_get_gnome_brightness();

/// Number from 0 to 100
bool dbus_set_gnome_brightness(double percentage);

void dbus_computer_logoff();

void dbus_computer_shut_down();

void dbus_computer_restart();

void dbus_open_in_folder(std::string path);

// Call Bluetooth setup and operations on the compositor main thread.
bool dbus_bluetooth_agent_ready();
extern void (*on_bluetooth_request)(BluetoothRequest *);

void register_agent_if_needed();

void unregister_agent_if_needed();

void upower_service_started();

void upower_service_ended();

enum struct BluetoothInterfaceType {
    Error = 0,
    Device = 1,
    Adapter = 2,
};

struct BluetoothInterface {
    virtual ~BluetoothInterface() = default;
    std::string object_path;
    std::string mac_address;
    std::string name;
    std::string alias;
    std::string upower_path;
    BluetoothInterfaceType type = BluetoothInterfaceType::Error;
};

struct BluetoothCallbackInfo {
    BluetoothCallbackInfo(BluetoothInterface *blue_interface, std::string command,
                          void (*function)(BluetoothCallbackInfo *));
    
    std::string mac_address;
    std::string object_path;
    std::string command;
    std::string message;
    void (*function)(BluetoothCallbackInfo *) = nullptr;
    bool succeeded = false;
};

struct Device : BluetoothInterface {
    std::string icon;
    std::string adapter;
    bool paired = false;
    bool connected = false;
    bool bonded = false;
    bool trusted = false;
    std::string percentage;
    
    Device(std::string string) {
        object_path = std::move(string);
        type = BluetoothInterfaceType::Device;
    }
    
    void connect(void (*function)(BluetoothCallbackInfo *));
    
    void disconnect(void (*function)(BluetoothCallbackInfo *));
    
    void trust(void (*function)(BluetoothCallbackInfo *));
    
    void untrust(void (*function)(BluetoothCallbackInfo *));
    
    void pair(void (*function)(BluetoothCallbackInfo *));
    
    void cancel_pair(void (*function)(BluetoothCallbackInfo *));
    
    void unpair(void (*function)(BluetoothCallbackInfo *));
};

struct Adapter : BluetoothInterface {
    bool powered = false;
    bool discovering = false;
    
    Adapter(std::string string) {
        object_path = std::move(string);
        type = BluetoothInterfaceType::Adapter;
    }
    
    void scan_on(void (*function)(BluetoothCallbackInfo *));
    
    void scan_off(void (*function)(BluetoothCallbackInfo *));
    
    void power_on(void (*function)(BluetoothCallbackInfo *));
    
    void power_off(void (*function)(BluetoothCallbackInfo *));
};

bool become_default_bluetooth_agent();

void update_devices();

extern void (*on_any_bluetooth_property_changed)();


#endif //WINBAR_SIMPLE_DBUS_H
