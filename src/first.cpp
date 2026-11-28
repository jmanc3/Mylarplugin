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
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <unistd.h>
#include <vector>

static std::filesystem::path staged_fonts_dir() {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    std::filesystem::path base = (runtime_dir && *runtime_dir) ? runtime_dir : "/tmp";
    return base / ("mylar-fonts-" + std::to_string(getpid()));
}

static std::string hash_file_contents(const std::filesystem::path &path, std::vector<char> &bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!in.eof() && in.fail())
        return {};
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char b : bytes) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long) h);
    return hex;
}

static std::filesystem::path stage_font_file(const std::filesystem::path &src,
                                             const std::filesystem::path &stage_dir) {
    std::vector<char> bytes;
    std::string digest = hash_file_contents(src, bytes);
    if (digest.empty())
        return src;

    std::error_code ec;
    std::filesystem::create_directories(stage_dir, ec);
    if (ec)
        return src;

    std::string name = src.stem().string() + "-" + digest + src.extension().string();
    std::filesystem::path dst = stage_dir / name;
    if (std::filesystem::exists(dst, ec) && std::filesystem::file_size(dst, ec) == bytes.size() && !ec)
        return dst;

    std::filesystem::path tmp = stage_dir / (name + ".tmp");
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
        return src;
    if (!bytes.empty())
        out.write(bytes.data(), (std::streamsize) bytes.size());
    out.close();
    if (!out) {
        std::filesystem::remove(tmp, ec);
        return src;
    }
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return src;
    }
    return dst;
}

void cleanup_staged_fonts() {
    std::error_code ec;
    std::filesystem::remove_all(staged_fonts_dir(), ec);
}

void load_in_fonts() {
    //char *home = getenv("HOME");
    //std::string font_directory(home);
    //font_directory += "/.config/mylar/fonts";
    std::string font_directory = "/usr/share/mylardesktop/fonts";
    std::filesystem::path stage_dir = staged_fonts_dir();

    FcInit();
    FcConfig *now = FcConfigGetCurrent();
    // Clear previous app fonts so a plugin reload doesn't keep stale Fontconfig
    // patterns ahead of updated fonts, and stage each font at a content-hashed path:
    // Cairo's process-wide FT font cache is keyed by filename alone and holds live
    // FT_Faces in its holdover cache, so overwriting icons.ttf in place via
    // `make install` corrupts or hides glyphs on reload if the path is reused.
    FcConfigAppFontClear(now);
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(font_directory, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::filesystem::path staged = stage_font_file(entry.path(), stage_dir);
        FcConfigAppFontAddFile(now, (const FcChar8 *) staged.c_str());
    }
    FcConfigBuildFonts(now);

    if (PangoFontMap *def = pango_cairo_font_map_get_default();
        def && PANGO_IS_FC_FONT_MAP(def)) {
        pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(def));
    }
}

// Give a fresh install a wallpaper: pick a random bundled one if the user has none yet.
void install_default_wallpaper() {
    const char *home = getenv("HOME");
    if (!home)
        return;
    std::error_code ec;
    std::filesystem::path wall = std::filesystem::path(home) / ".config/mylar/wall.png";
    if (std::filesystem::exists(wall, ec))
        return;

    std::vector<std::filesystem::path> candidates;
    for (const auto &entry : std::filesystem::directory_iterator("/usr/share/mylardesktop/wallpapers", ec)) {
        if (entry.is_regular_file(ec))
            candidates.push_back(entry.path());
    }
    if (candidates.empty())
        return;

    std::random_device rd;
    std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
    std::filesystem::create_directories(wall.parent_path(), ec);
    std::filesystem::copy_file(candidates[pick(rd)], wall, std::filesystem::copy_options::skip_existing, ec);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) { // When started as a plugin
#ifdef TRACY_ENABLE
    TracyAppInfo("Mylar Desktop", 13);
#endif

    set_api(handle);
    stop_client_key_repeat();

    settings::load_save_settings(false, set); // load
    settings::load_save_settings(true, set); // save
    load_in_fonts();
    install_default_wallpaper();

    try {
        heart::begin();
    } catch (...) {

    }

    return {"Mylardesktop", "Mylar is a smooth and beautiful wayland desktop, written on Hyprland", "jmanc3", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    stop_client_key_repeat();
    try {
        heart::end();
    } catch (const std::exception &error) {
        fprintf(stderr, "Mylar: plugin teardown failed: %s\n", error.what());
    }
    // The compositor thread outlives this plugin; workers release their own
    // caches when their rendering loops finish.
    cleanup_cached_pango_fonts();
    drawing::cleanup_thread_resources();
    cleanup_staged_fonts();
}

void init_mylar(void* h) { // When started directly from hyprland
    started_directly_from_hyprland = true;
    PLUGIN_INIT(h);
}

void exit_mylar(void* h) {
    PLUGIN_EXIT();
}
