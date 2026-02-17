#include "first.h"

#include "heart.h"
#include "settings.h"
#include "hypriso.h"

#include <dbus/dbus-shared.h>
#include <thread>

#include <hyprland/src/plugins/PluginAPI.hpp>

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

Globals *globals = new Globals;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) { // When started as a plugin
#ifdef TRACY_ENABLE
    TracyAppInfo("Mylar Desktop", 13);
#endif
    
    globals->api = handle;

    settings::load_save_settings(false, set); // load
    settings::load_save_settings(true, set); // save

    heart::begin();

    return {"Mylardesktop", "Mylar is a smooth and beautiful wayland desktop, written on Hyprland", "jmanc3", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    heart::end(); 
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void init_mylar(void* h) { // When started directly from hyprland
    PLUGIN_INIT(h);
}

void exit_mylar(void* h) {
    PLUGIN_EXIT();
}

