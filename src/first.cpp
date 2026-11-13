#include "first.h"
#include "pango_font_cache.h"

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
bool started_directly_from_hyprland = false;

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

#include <fontconfig/fontconfig.h>
#include <filesystem>

void load_in_fonts() {
    //char *home = getenv("HOME");
    //std::string font_directory(home);
    //font_directory += "/.config/mylar/fonts";
    std::string font_directory = "/usr/share/mylardesktop/fonts";

    FcInit();
    FcConfig *now = FcConfigGetCurrent();
    // Add each file rather than the directory: FcConfigAppFontAddDir trusts the on-disk
    // cache, which is keyed on the directory's mtime, so a font overwritten in place by
    // `make install` keeps its stale charset (missing glyphs render as tofu).
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(font_directory, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        FcConfigAppFontAddFile(now, (const FcChar8 *) entry.path().c_str());
    }
    FcConfigBuildFonts(now);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) { // When started as a plugin
#ifdef TRACY_ENABLE
    TracyAppInfo("Mylar Desktop", 13);
#endif

    set_api(handle);

    settings::load_save_settings(false, set); // load
    settings::load_save_settings(true, set); // save
    load_in_fonts();

    try {
        heart::begin();
    } catch (...) {

    }

    return {"Mylardesktop", "Mylar is a smooth and beautiful wayland desktop, written on Hyprland", "jmanc3", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    try {
        settings::load_save_settings(true, set); // save
        heart::end();
    } catch (...) {

    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The compositor thread outlives this plugin; workers release their own
    // caches when their rendering loops finish.
    cleanup_cached_pango_fonts();
    drawing::cleanup_thread_resources();
}

void init_mylar(void* h) { // When started directly from hyprland
    started_directly_from_hyprland = true;
    PLUGIN_INIT(h);
}

void exit_mylar(void* h) {
    PLUGIN_EXIT();
}
