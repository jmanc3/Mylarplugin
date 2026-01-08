#include "first.h"

#include "heart.h"

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

    second::begin();

    return {"Mylardesktop", "Mylar is a smooth and beautiful wayland desktop, written on Hyprland", "jmanc3", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
   second::end(); 
   std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void init_mylar(void* h) { // When started directly from hyprland
    PLUGIN_INIT(h);
}

void exit_mylar(void* h) {
    PLUGIN_EXIT();
}

