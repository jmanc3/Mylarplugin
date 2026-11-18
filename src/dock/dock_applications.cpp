#include "dock/dock_applications.h"

#include "dock/dock_internal.h"

#include "desktop_icons.h"
#include "icon_browser.h"
#include "settings.h"
#include "text_editor.h"

#include <dbus/dbus.h>
#include <hyprutils/path/Path.hpp>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <linux/input-event-codes.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>
#include <unistd.h>

struct Script : UserData {
    std::string name;
    std::vector<std::string> keywords;
    std::vector<std::string> categories;
    std::string generic_name;
    std::string lowercase_name;
    int priority = -1;
    int historical_ranking = -1;
    int match_level = 100;
    std::string full_path;
    std::shared_ptr<cairo_surface_t> icon_surface;
    int icon_size = 0;

    std::string path;

    bool path_is_full_command = false;

    bool selected = false;

    // A row from the settings search instead of an executable.
    bool is_setting = false;
    settings::SearchResult setting;
    // Sorting: how often and how recently it was picked, then match_level
    // (see match_script), then the original order.
    double history = 0;
    int base_order = 0;

    // An installed application (a .desktop file) instead of a $PATH executable.
    // full_path is the .desktop file, exec what it runs.
    bool is_desktop = false;
    std::string desktop_id;
    std::string exec;
    std::string icon;
    bool terminal = false;
    // Its .desktop file keeps it to other desktops (OnlyShowIn / NotShowIn).
    // Only listed with set->applications_show_other_desktops.
    bool other_desktop = false;

    // The "Run as command..." row, listed last while there is search text:
    // runs the text as typed, arguments included. full_path holds the text.
    bool is_run_command = false;

    // One of our own programs, listed like an application. Names which one.
    std::string builtin;

    // First seen by the rescan made while the menu was already open. Kept
    // below everything that was listed, so rows don't move under the user.
    bool late = false;
};

static std::vector<Script> scripts;
static std::mutex scripts_mutex;
static std::shared_future<std::vector<Script>> scripts_refresh;

/*
 * The last selections made in this menu, oldest first, kept in
 * ~/.config/mylar/search.txt (one per line) and used to sort the options.
 */
static constexpr int selection_history_max = 100;
static std::mutex selection_history_mutex;
static std::vector<std::string> selection_history;
static bool selection_history_loaded = false;

static std::filesystem::path selection_history_path() {
    const char *home = getenv("HOME");
    return std::filesystem::path(home ? home : "") / ".config/mylar/search.txt";
}

static void selection_history_load_locked() {
    if (selection_history_loaded)
        return;
    selection_history_loaded = true;
    std::ifstream in(selection_history_path());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            selection_history.push_back(line);
    }
    if ((int) selection_history.size() > selection_history_max)
        selection_history.erase(selection_history.begin(), selection_history.end() - selection_history_max);
}

static std::string selection_key(const Script &s) {
    if (s.is_setting)
        return "setting\t" + s.setting.page + "\t" + s.setting.section + "\t" + s.setting.title;
    if (s.is_desktop)
        return "desktop\t" + s.desktop_id;
    return "app\t" + s.name;
}

static void selection_history_record(const std::string &key) {
    if (key.find('\n') != std::string::npos)
        return;
    std::lock_guard<std::mutex> lock(selection_history_mutex);
    selection_history_load_locked();
    selection_history.push_back(key);
    if ((int) selection_history.size() > selection_history_max)
        selection_history.erase(selection_history.begin(), selection_history.end() - selection_history_max);
    auto path = selection_history_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    for (const auto &entry : selection_history)
        out << entry << "\n";
}

// Every pick counts one, plus up to a half more the more recent it was.
static std::unordered_map<std::string, double> selection_history_scores() {
    std::lock_guard<std::mutex> lock(selection_history_mutex);
    selection_history_load_locked();
    std::unordered_map<std::string, double> scores;
    const double n = selection_history.size();
    for (int i = 0; i < (int) selection_history.size(); i++)
        scores[selection_history[i]] += 1.0 + .5 * (i + 1) / n;
    return scores;
}

static Container *make_field(Container *parent, bool only_numbers, std::string initial_value, std::function<void(std::string)> on_change) {
    auto pad = parent->child(FILL_SPACE, FILL_SPACE);
    STextEditorOptions options;
    options.multiline = false;
    options.only_numbers = only_numbers;
    options.font_size = 11;
    options.window = [](Container *root) { return static_cast<Dock *>(root->user_data)->applications->raw_window; };
    options.viewport = [](Container *root, Container *c) {
        auto dpi = static_cast<Dock *>(root->user_data)->applications->raw_window->dpi;
        // The search icon sits before the text.
        auto bounds = c->real_bounds;
        bounds.x += 38 * dpi;
        bounds.w = std::max(0.0, bounds.w - 38 * dpi - 11 * dpi);
        return bounds;
    };
    options.on_change = std::move(on_change);
    setup_text_editor(pad, std::move(initial_value), std::move(options));
    pad->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dpi = static_cast<Dock *>(root->user_data)->applications->raw_window->dpi;
        auto bounds = measure_text_editor(root, c);
        c->wanted_bounds.w = FILL_SPACE;
        c->wanted_bounds.h = bounds.h + 11 * dpi * 2 * .8;
    };

    pad->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock*)root->user_data;
        auto cr = dock->applications->raw_window->drawing_context;
        auto dpi = dock->applications->raw_window->dpi;

        set_argb(cr, c->active ? accent : RGBA(.8, .8, .8, 1));
        auto b = c->real_bounds;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0 * dpi);
        cr->stroke();

        set_argb(cr, {1, 1, 1, 1});
        auto minus_border = c->real_bounds;
        minus_border.shrink(std::round(1 * dpi));
        b = minus_border;
        drawRoundedRect(cr, b.x, b.y, b.w, b.h, 10 * dpi, 1.0);
        //set_rect(cr, minus_border);
        cr->fill();

        const std::string glyph = "\uE721"; // Search
        const RGBA glyph_color(0, 0, 0, c->active ? .9 : .5);
        auto gb = draw_text(cr, 0, 0, glyph, 13 * dpi, false, icon_font, -1, -1, glyph_color);
        draw_text(cr, c->real_bounds.x + 14 * dpi, c->real_bounds.y + (c->real_bounds.h - gb.h) * .5, glyph, 13 * dpi, true,
                  icon_font, -1, -1, glyph_color);

        // Says what can be searched until something is typed.
        if (static_cast<STextEditorData *>(c->user_data)->text.empty()) {
            const std::string prompt = "Desktop files, Scripts, Settings...";
            const RGBA prompt_color(0, 0, 0, .4);
            auto pb = draw_text(cr, 0, 0, prompt, 11 * dpi, false, set->font, -1, -1, prompt_color);
            draw_text(cr, c->real_bounds.x + 38 * dpi, c->real_bounds.y + (c->real_bounds.h - pb.h) * .5, prompt, 11 * dpi,
                      true, set->font, -1, -1, prompt_color);
        }

        paint_text_editor(root, c, RGBA(0, 0, 0, 1), accent);
    };
    return pad;
}

static void scripts_load(std::vector<Script> &temp_scripts) {
    temp_scripts.clear();

    // go through every directory in $PATH environment variable
    // add to our scripts list if the files we check are executable
    const auto env_path = getenv("PATH");
    std::string paths = env_path ? env_path : "";

    std::replace(paths.begin(), paths.end(), ':', ' ');

    std::stringstream ss(paths);
    std::string string_path;
    while (ss >> string_path) {
        if (auto *dir = opendir(string_path.c_str())) {
            struct dirent *dp;
            while ((dp = readdir(dir)) != NULL) {
                struct stat st, ln;

                // This is what determines if its an executable and its from dmenu and
                // its not good. oh well
                static int flag[26];
#define FLAG(x) (flag[(x) - 'a'])

                const char *path = string_path.c_str();
                if ((!stat(path, &st) &&
                     (FLAG('a') || dp->d_name[0] != '.') /* hidden files      */
                     && (!FLAG('b') || S_ISBLK(st.st_mode)) /* block special     */
                     && (!FLAG('c') || S_ISCHR(st.st_mode)) /* character special */
                     && (!FLAG('d') || S_ISDIR(st.st_mode)) /* directory         */
                     && (!FLAG('e') || access(path, F_OK) == 0) /* exists            */
                     && (!FLAG('f') || S_ISREG(st.st_mode)) /* regular file      */
                     && (!FLAG('g') || st.st_mode & S_ISGID) /* set-group-id flag */
                     && (!FLAG('h') ||
                         (!lstat(path, &ln) && S_ISLNK(ln.st_mode))) /* symbolic link */
                     && (!FLAG('p') || S_ISFIFO(st.st_mode)) /* named pipe        */
                     && (!FLAG('r') || access(path, R_OK) == 0) /* readable          */
                     && (!FLAG('s') || st.st_size > 0) /* not empty         */
                     && (!FLAG('u') || st.st_mode & S_ISUID) /* set-user-id flag  */
                     && (!FLAG('w') || access(path, W_OK) == 0) /* writable          */
                     && (!FLAG('x') || access(path, X_OK) == 0)) !=
                    FLAG('v')) {
                    /* executable        */

                    if (!(FLAG('q'))) {
                        bool already_have_this_script = false;
                        std::string name = std::string(dp->d_name);
                        for (const auto &script: temp_scripts) {
                            if (script.name == name) {
                                already_have_this_script = true;
                                break;
                            }
                        }
                        if (already_have_this_script)
                            continue;

                        Script entry;
                        auto *script = &entry;
                        script->name = name;
                        script->lowercase_name = script->name;
                        std::transform(script->lowercase_name.begin(),
                                       script->lowercase_name.end(),
                                       script->lowercase_name.begin(), ::tolower);

                        script->full_path = path;
                        script->full_path += "/" + name;
                        script->path = path;
                        if (!script->path.empty()) {
                            if (script->path[script->path.length() - 1] == '/' ||
                                script->path[script->path.length() - 1] == '\\') {
                                script->path.erase(script->path.begin() +
                                                   (script->path.length() - 1));
                            }
                        }

                        temp_scripts.push_back(std::move(entry));
                    }
                }
            }
            closedir(dir);
        }
    }

    if (temp_scripts.empty())
        return;
}

static std::vector<std::string> split_list(const char *text, char separator) {
    std::vector<std::string> parts;
    std::stringstream ss(text ? text : "");
    std::string part;
    while (std::getline(ss, part, separator))
        if (!part.empty())
            parts.push_back(part);
    return parts;
}

/*
 * Where .desktop files live, most important first: the user's data directory,
 * then $XDG_DATA_DIRS, then the usual places in case the session's
 * environment doesn't list them (flatpak, snap, nix).
 */
static std::vector<std::filesystem::path> application_directories() {
    std::vector<std::filesystem::path> dirs;
    auto add = [&dirs](const std::filesystem::path &data_dir) {
        if (data_dir.empty() || !data_dir.is_absolute())
            return;
        std::error_code ec;
        auto dir = std::filesystem::canonical(data_dir / "applications", ec);
        if (ec || !std::filesystem::is_directory(dir, ec))
            return;
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end())
            dirs.push_back(dir);
    };
    const char *home_env = getenv("HOME");
    const std::filesystem::path home = home_env ? home_env : "";
    const char *data_home = getenv("XDG_DATA_HOME");
    if (data_home && *data_home)
        add(data_home);
    else if (!home.empty())
        add(home / ".local/share");
    const char *data_dirs = getenv("XDG_DATA_DIRS");
    for (const auto &dir : split_list(data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share", ':'))
        add(dir);
    if (!home.empty()) {
        add(home / ".local/share");
        add(home / ".local/share/flatpak/exports/share");
        add(home / ".nix-profile/share");
    }
    add("/usr/local/share");
    add("/usr/share");
    add("/var/lib/flatpak/exports/share");
    add("/var/lib/snapd/desktop");
    add("/run/current-system/sw/share");
    return dirs;
}

static bool program_exists(const std::string &program) {
    if (program.find('/') != std::string::npos)
        return access(program.c_str(), X_OK) == 0;
    for (const auto &dir : split_list(getenv("PATH"), ':'))
        if (access((dir + "/" + program).c_str(), X_OK) == 0)
            return true;
    return false;
}

// The applications a menu should offer, sorted by name. The ones for other
// desktops are included, marked Script::other_desktop. The .desktop files in
// desktop_folder (the one the desktop icons show) are listed too, after the
// installed ones, so a copy of an installed application isn't listed twice.
static void desktop_entries_load(std::vector<Script> &entries, const std::string &desktop_folder) {
    entries.clear();
    const auto current_desktops = split_list(getenv("XDG_CURRENT_DESKTOP"), ':');
    auto names_current_desktop = [&current_desktops](const std::string &list) {
        for (const auto &desktop : split_list(list.c_str(), ';'))
            if (std::find(current_desktops.begin(), current_desktops.end(), desktop) != current_desktops.end())
                return true;
        return false;
    };

    // Desktop file ids. A file overrides the ones with its id in later
    // directories, even when all it does is hide them.
    std::unordered_set<std::string> seen;
    // Each directory with how deep to look in it.
    std::vector<std::pair<std::filesystem::path, int>> dirs;
    for (const auto &dir : application_directories())
        dirs.emplace_back(dir, 3);
    if (!desktop_folder.empty()) {
        std::error_code ec;
        auto dir = std::filesystem::canonical(desktop_folder, ec);
        if (!ec && std::filesystem::is_directory(dir, ec) &&
            std::find_if(dirs.begin(), dirs.end(), [&dir](const auto &d) { return d.first == dir; }) == dirs.end())
            dirs.emplace_back(dir, 0);
    }
    for (const auto &[dir, max_depth] : dirs) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            dir, std::filesystem::directory_options::follow_directory_symlink |
                 std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            // Applications aren't nested deep, and a symlink could loop.
            if (it.depth() >= max_depth)
                it.disable_recursion_pending();
            const auto &path = it->path();
            if (path.extension() != ".desktop")
                continue;
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec))
                continue;
            std::string id = path.lexically_relative(dir).string();
            std::replace(id.begin(), id.end(), '/', '-');
            if (!seen.insert(id).second)
                continue;

            auto entry = desktop_file::read_entry(path);
            auto value = [&entry](const char *key) {
                auto found = entry.find(key);
                return found == entry.end() ? std::string() : found->second;
            };
            auto type = value("Type");
            if (!type.empty() && type != "Application")
                continue;
            if (value("Hidden") == "true" || value("NoDisplay") == "true")
                continue;
            auto exec = desktop_file::exec_without_field_codes(value("Exec"));
            if (exec.empty())
                continue;
            auto try_exec = value("TryExec");
            if (!try_exec.empty() && !program_exists(try_exec))
                continue;
            // Old LSP Plugins packages (EasyEffects pulls them in) install
            // one launcher per audio plugin, hundreds of them.
            if (id.starts_with("in.lsp_plug.lsp_plugins_"))
                continue;

            Script script;
            script.is_desktop = true;
            if (!current_desktops.empty()) {
                auto only_show_in = value("OnlyShowIn");
                script.other_desktop = (!only_show_in.empty() && !names_current_desktop(only_show_in)) ||
                                       names_current_desktop(value("NotShowIn"));
            }
            script.desktop_id = id;
            script.name = value("Name");
            if (script.name.empty())
                script.name = path.stem().string();
            script.lowercase_name = to_lower(script.name);
            script.generic_name = to_lower(value("GenericName"));
            for (auto &keyword : split_list(value("Keywords").c_str(), ';'))
                script.keywords.push_back(to_lower(keyword));
            script.full_path = path.string();
            script.exec = exec;
            script.icon = value("Icon");
            script.terminal = value("Terminal") == "true";
            entries.push_back(std::move(script));
        }
    }
    // Our own programs, sorted in with the applications.
    Script icon_browser;
    icon_browser.is_desktop = true;
    icon_browser.builtin = "icon_browser";
    icon_browser.desktop_id = "mylar-icon-browser";
    icon_browser.name = "Mylar Icon Browser";
    icon_browser.lowercase_name = to_lower(icon_browser.name);
    icon_browser.generic_name = "icon browser";
    icon_browser.keywords = {"icons", "icon theme", "yad"};
    icon_browser.full_path = "Browse the icons of every installed theme";
    icon_browser.icon = "preferences-desktop-icons";
    entries.push_back(std::move(icon_browser));

    std::stable_sort(entries.begin(), entries.end(), [](const Script &a, const Script &b) {
        return a.lowercase_name < b.lowercase_name;
    });
}

// Applications first, then everything executable in $PATH.
static std::shared_future<std::vector<Script>> load_scripts_async() {
    // Read the setting here, not on the loading thread.
    std::string desktop_folder;
    if (auto resolved = Hyprutils::Path::resolvePath(set->desktop_folder))
        desktop_folder = resolved.value();
    return std::async(std::launch::async, [desktop_folder] {
        std::vector<Script> loaded;
        desktop_entries_load(loaded, desktop_folder);
        std::vector<Script> executables;
        scripts_load(executables);
        loaded.insert(loaded.end(), std::make_move_iterator(executables.begin()),
                      std::make_move_iterator(executables.end()));
        return loaded;
    }).share();
}

// True when text starts the string or any later word of it.
static bool starts_a_word(const std::string &words, const std::string &text) {
    for (size_t at = words.find(text); at != std::string::npos; at = words.find(text, at + 1))
        if (at == 0 || !isalnum((unsigned char) words[at - 1]))
            return true;
    return false;
}

/*
 * How well an entry matches the search text: 0 exact name, 1 name prefix,
 * 2 a later word of the name, the generic name or a keyword, -1 not at all.
 * Executables are matched as typed, applications ignoring case.
 */
static int match_script(const Script &s, const std::string &text, const std::string &lowercase_text) {
    if (!s.is_desktop)
        return s.name == text ? 0 : s.name.starts_with(text) ? 1 : -1;
    if (s.lowercase_name == lowercase_text)
        return 0;
    if (s.lowercase_name.starts_with(lowercase_text))
        return 1;
    if (starts_a_word(s.lowercase_name, lowercase_text) || starts_a_word(s.generic_name, lowercase_text))
        return 2;
    for (const auto &keyword : s.keywords)
        if (keyword.starts_with(lowercase_text))
            return 2;
    return -1;
}

static void watch_applications_scripts(Dock *dock, MylarWindow *applications,
                                      std::shared_future<std::vector<Script>> refresh,
                                      std::function<void(const std::vector<Script> &)> merge) {
    windowing::timer(dock->app, 30, [dock, applications, refresh, merge](void *) {
        {
            std::lock_guard<std::mutex> lock(dock->app->mutex);
            if (dock->applications != applications)
                return;
            if (refresh.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                watch_applications_scripts(dock, applications, refresh, merge);
                return;
            }
            merge(refresh.get());
        }
        // Rendering acquires the app mutex and reruns the current search filter.
        windowing::redraw_now(applications->raw_window);
    }, nullptr);
}

static std::string shell_quote(const std::string &text) {
    std::string quoted = "'";
    for (char ch : text) {
        if (ch == '\'')
            quoted += "'\\''";
        else
            quoted += ch;
    }
    return quoted + "'";
}

// For applications that ask for a terminal: $TERMINAL when set, otherwise
// the first common emulator that is installed.
static std::string run_in_terminal(const std::string &command) {
    return "for t in \"$TERMINAL\" alacritty kitty foot konsole xterm; do "
           "command -v \"$t\" >/dev/null 2>&1 && exec \"$t\" -e sh -c " + shell_quote(command) + "; done";
}

// Records the pick, then launches the application or executable, or opens
// the setting. The caller closes the menu.
static void activate_script(const Script *s) {
    if (s->is_run_command) {
        launch_command(s->full_path);
        return;
    }
    selection_history_record(selection_key(*s));
    if (s->builtin == "icon_browser") {
        icon_browser::open();
    } else if (s->is_setting) {
        auto result = s->setting;
        main_thread([result]() {
            settings::open_search_result(result);
        });
    } else if (s->is_desktop) {
        launch_command(s->terminal ? run_in_terminal(s->exec) : s->exec);
    } else {
        launch_command(s->full_path);
    }
}

/*
 * Keyboard focus moves between the search field, the settings gear and the power button
 * with Tab / Shift+Tab, using the shared tab traversal (named containers with key handlers).
 */
static void applications_move_focus(Container *root, Container *c, bool previous) {
    auto dock = (Dock *) root->user_data;
    auto applications = dock->applications;
    std::weak_ptr<bool> lifetime = c->lifetime;
    // Next frame, or the newly focused container would also receive this Tab.
    windowing::timer(dock->app, 1, [dock, applications, root, c, previous, lifetime](void *) {
        std::lock_guard<std::mutex> lock(dock->app->mutex);
        if (lifetime.expired() || !applications || dock->applications != applications)
            return;
        if (previous)
            activate_previous_activatable(root, c);
        else
            activate_next_activatable(root, c);
        windowing::redraw(applications->raw_window);
    }, nullptr);
}

/*
 * Power menu: sign out, shut down, restart (the computer) and reload (this plugin).
 */
struct PowerItem {
    const char *label;
    const char *icon;
};
static const PowerItem power_items[] = {
    {"Sign Out", "\uE77B"},
    {"Shut Down", "\uE7E8"},
    {"Restart", "\uE777"},
    {"Reload", "\uE117"},
};
static constexpr int power_item_count = sizeof(power_items) / sizeof(power_items[0]);

// systemd-logind over the system bus. interactive=true lets polkit ask when needed.
static void login1_call(std::string method) {
    std::thread([method]() {
        DBusError error;
        dbus_error_init(&error);
        DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
        if (!connection) {
            fprintf(stderr, "mylar: system bus unavailable for %s: %s\n", method.c_str(), error.message ? error.message : "");
            dbus_error_free(&error);
            return;
        }
        dbus_connection_set_exit_on_disconnect(connection, false);
        DBusMessage *message = dbus_message_new_method_call("org.freedesktop.login1", "/org/freedesktop/login1",
                                                            "org.freedesktop.login1.Manager", method.c_str());
        dbus_bool_t interactive = true;
        if (message && dbus_message_append_args(message, DBUS_TYPE_BOOLEAN, &interactive, DBUS_TYPE_INVALID)) {
            // Long timeout: polkit may be waiting on an authentication prompt.
            DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, 120000, &error);
            if (reply)
                dbus_message_unref(reply);
            else
                fprintf(stderr, "mylar: login1 %s failed: %s\n", method.c_str(), error.message ? error.message : "");
        }
        if (message)
            dbus_message_unref(message);
        dbus_error_free(&error);
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
    }).detach();
}

// Path of the loaded plugin .so, so it can be unloaded and loaded again.
static std::string plugin_path() {
    Dl_info info;
    if (!dladdr((void *) &plugin_path, &info) || !info.dli_fname)
        return "";
    std::error_code ec;
    auto path = std::filesystem::canonical(info.dli_fname, ec);
    return ec ? std::string(info.dli_fname) : path.string();
}

static void run_power_action(int index) {
    if (index == 0) {
        // Same as the "Log out" shortcut: hyprshutdown when installed, otherwise exit Hyprland.
        launch_command("command -v hyprshutdown >/dev/null 2>&1 && hyprshutdown || hyprctl dispatch 'hl.dsp.exit()'");
    } else if (index == 1) {
        login1_call("PowerOff");
    } else if (index == 2) {
        login1_call("Reboot");
    } else if (index == 3) {
        auto path = plugin_path();
        if (path.empty())
            return;
        // Runs outside the plugin, which is gone between the two commands.
        auto quoted = shell_quote(path);
        launch_command("sleep 0.3; hyprctl plugin unload " + quoted + "; sleep 1; hyprctl plugin load " + quoted);
    }
}

static void close_power_menu(Dock *dock, bool refocus_applications) {
    if (!dock->power_menu)
        return;
    windowing::close_window(dock->power_menu->raw_window);
    if (refocus_applications && dock->applications) {
        auto applications = dock->applications;
        later(30, [dock, applications](Timer *) {
            if (dock->applications == applications)
                windowing::request_keyboard_focus(applications->raw_window);
        });
    }
}

static void activate_power_item(Dock *dock, int index) {
    if (index < 0 || index >= power_item_count)
        return;
    // Closing the applications menu also closes this child popup.
    if (dock->applications)
        windowing::close_window(dock->applications->raw_window);
    run_power_action(index);
}

static void open_power_menu(Container *root, Container *anchor, bool via_keyboard) {
    auto dock = (Dock *) root->user_data;
    auto parent = dock->applications;
    if (!parent || parent->popup_window || dock->power_menu)
        return;
    auto dpi = parent->raw_window->dpi;
    static constexpr int row_h = 36;
    static constexpr int pad = 6;
    const int w = 200;
    const int h = pad * 2 + row_h * power_item_count;
    RawWindowSettings settings = make_icon_anchored_popup_settings(anchor, dpi, w, h);
    // Above the button, extending left from its right edge.
    settings.popup.anchor = RawWindowSettings::PopupAnchor::TOP_RIGHT;
    settings.popup.gravity = RawWindowSettings::PopupGravity::TOP_LEFT;
    settings.popup.offset_y = -6;
    settings.keyboard_focus_on_click = true;
    auto menu = open_mylar_popup(parent, settings);
    if (!menu)
        return;
    dock->power_menu = menu;
    auto menu_root = menu->root;
    menu_root->user_data = dock;
    menu_root->wanted_bounds.w = FILL_SPACE;
    menu_root->wanted_bounds.h = FILL_SPACE;
    menu_root->on_closed = [menu](Container *root) {
        auto dock = (Dock *) root->user_data;
        if (dock->power_menu == menu)
            dock->power_menu = nullptr;
    };
    menu_root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto window = dock->power_menu->raw_window;
        paint_popup_background(window->drawing_context, c->real_bounds, window->dpi);
    };

    // Keyboard selection; -1 until the keyboard or the mouse picks a row.
    auto selected = std::make_shared<int>(via_keyboard ? 0 : -1);
    menu_root->when_key_event = [selected](Container *root, Container *, int, bool pressed, xkb_keysym_t sym, int mods,
                                           bool, std::string) {
        if (!pressed)
            return;
        auto dock = (Dock *) root->user_data;
        int &index = *selected;
        bool back = sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab ||
                    (sym == XKB_KEY_Tab && (mods & Modifier::MOD_SHIFT));
        if (back) {
            index = index <= 0 ? power_item_count - 1 : index - 1;
        } else if (sym == XKB_KEY_Down || sym == XKB_KEY_Tab) {
            index = index < 0 ? 0 : (index + 1) % power_item_count;
        } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) {
            activate_power_item(dock, index);
            return;
        } else if (sym == XKB_KEY_Escape) {
            close_power_menu(dock, true);
            return;
        } else {
            return;
        }
        if (dock->power_menu)
            windowing::redraw(dock->power_menu->raw_window);
    };

    auto list = menu_root->child(::vbox, FILL_SPACE, FILL_SPACE);
    list->pre_layout = [](Container *root, Container *c, const Bounds &) {
        auto dpi = ((Dock *) root->user_data)->power_menu->raw_window->dpi;
        c->wanted_pad = Bounds(pad, pad, pad, pad).scale(dpi);
    };
    for (int i = 0; i < power_item_count; i++) {
        auto row = list->child(FILL_SPACE, row_h);
        row->pre_layout = [](Container *root, Container *c, const Bounds &) {
            c->wanted_bounds.h = row_h * ((Dock *) root->user_data)->power_menu->raw_window->dpi;
        };
        row->when_paint = [i, selected](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto window = dock->power_menu->raw_window;
            auto cr = window->drawing_context;
            auto dpi = window->dpi;
            auto b = c->real_bounds;
            if (c->state.mouse_pressing || c->state.mouse_hovering || *selected == i) {
                set_argb(cr, c->state.mouse_pressing ? RGBA(0, 0, 0, .14) : RGBA(0, 0, 0, .07));
                drawRoundedRect(cr, b.x, b.y, b.w, b.h, 6 * dpi, 1.0);
                cr->fill();
            }
            const RGBA ink(0, 0, 0, .9);
            auto ib = draw_text(cr, 0, 0, power_items[i].icon, 13 * dpi, false, icon_font);
            draw_text(cr, b.x + 12 * dpi, b.y + b.h * .5 - ib.h * .5, power_items[i].icon, 13 * dpi, true, icon_font,
                      -1, -1, ink);
            auto tb = draw_text(cr, 0, 0, power_items[i].label, 11 * dpi, false, set->font, -1, -1, ink);
            draw_text(cr, b.x + 40 * dpi, b.y + b.h * .5 - tb.h * .5, power_items[i].label, 11 * dpi, true, set->font,
                      -1, -1, ink);
        };
        row->when_mouse_enters_container = [i, selected](Container *root, Container *) {
            *selected = i;
            auto dock = (Dock *) root->user_data;
            windowing::redraw(dock->power_menu->raw_window);
        };
        row->when_clicked = [i](Container *root, Container *c) {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            activate_power_item((Dock *) root->user_data, i);
        };
    }

    later(30, [dock, menu](Timer *) {
        if (dock->power_menu == menu)
            windowing::request_keyboard_focus(menu->raw_window);
    });
    windowing::redraw(menu->raw_window);
}

void fill_applications_container(Container *root) {
    root->when_paint = [](Container *root, Container *c) {
        auto dock = (Dock *) root->user_data;
        auto cr = dock->applications->raw_window->drawing_context;
        paint_popup_background(cr, c->real_bounds, dock->applications->raw_window->dpi);
    };

    static const float pad_amount = 16;
    auto padded = root->child(FILL_SPACE, FILL_SPACE);
    padded->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->applications->raw_window->dpi;
        c->wanted_pad = Bounds(pad_amount, pad_amount, pad_amount, pad_amount).scale(dpi);
        c->spacing = 8 * dpi;
    };

    struct SearchState {
        std::string field_text;
        int active_option = 0;
        bool keyboard_option_changed = false;
        // The selected entry is only highlighted while the field has focus.
        Container *field = nullptr;
        // Settings matching field_text, turned into rows on the next layout.
        std::vector<settings::SearchResult> settings_results;
        bool settings_rows_stale = false;
        std::unordered_map<std::string, double> history = selection_history_scores();
    };
    auto search = std::make_shared<SearchState>();

    // One row of the list, for an executable or a setting.
    static auto add_script_row = [](Container *scroll_content, std::shared_ptr<SearchState> search, const Script &entry) {
        auto s = new Script(entry);
        auto o = scroll_content->child(FILL_SPACE, FILL_SPACE);
        o->user_data = s;
        o->pre_layout = [](Container *root, Container *c, const Bounds &b) {
            auto dock = (Dock *) root->user_data;
            auto dpi = dock->applications->raw_window->dpi;
            c->wanted_bounds.h = 56 * dpi;
        };
        o->when_paint = [search](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto s = (Script *) c->user_data;
            auto name = s->name;
            auto full = s->full_path;
            auto dpi = dock->applications->raw_window->dpi;
            auto cr = dock->applications->raw_window->drawing_context;
            const int icon_size = std::round(get_icon_size(dpi));
            if (icons_loaded && s->icon_size != icon_size) {
                s->icon_size = icon_size;
                s->icon_surface.reset();
                auto icon_name = c3ic_fix_wm_class(name);
                std::string path;
                if (s->is_setting) {
                    path = one_shot_icon(icon_size, {"preferences-system", "preferences-desktop", "application-x-executable"});
                } else if (s->is_run_command) {
                    path = one_shot_icon(icon_size, {"system-run", "utilities-terminal", "application-x-executable"});
                } else if (s->is_desktop) {
                    // Icon= is a theme icon name or the path of an image.
                    if (s->icon.starts_with('/') && access(s->icon.c_str(), R_OK) == 0)
                        path = s->icon;
                    if (path.empty()) {
                        std::vector<std::string> candidates;
                        auto stem = std::filesystem::path(s->full_path).stem().string();
                        for (const auto &candidate : {s->icon, to_lower(s->icon), stem, to_lower(stem)})
                            if (!candidate.empty())
                                candidates.push_back(candidate);
                        if (!s->builtin.empty()) {
                            candidates.push_back("preferences-desktop-theme");
                            candidates.push_back("applications-graphics");
                        }
                        candidates.push_back("application-x-executable");
                        path = one_shot_icon(icon_size, candidates);
                    }
                } else {
                    path = one_shot_icon(icon_size, {
                        name, to_lower(name), icon_name, to_lower(icon_name), "application-x-executable"
                    });
                }
                if (!path.empty()) {
                    cairo_surface_t *surface = nullptr;
                    load_icon_full_path(&surface, path, icon_size);
                    if (surface)
                        s->icon_surface = std::shared_ptr<cairo_surface_t>(surface, cairo_surface_destroy);
                }
            }
            auto scroll = (ScrollContainer *) c->parent->parent;
            const auto &clip_bounds = scroll->real_bounds;
            const bool needs_clip = c->real_bounds.x < clip_bounds.x ||
                                    c->real_bounds.y < clip_bounds.y ||
                                    c->real_bounds.x + c->real_bounds.w > clip_bounds.x + clip_bounds.w ||
                                    c->real_bounds.y + c->real_bounds.h > clip_bounds.y + clip_bounds.h;
            if (needs_clip) {
                cr->save();
                cr->rectangle(clip_bounds.x, clip_bounds.y, clip_bounds.w, clip_bounds.h);
                cr->clip_path();
            }
            if (s->selected && search->field && search->field->active) {
                set_argb(cr, accent);
            } else {
                set_argb(cr, {0, 0, 0, .15});
            }
            drawRoundedRect(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w,
                            c->real_bounds.h,
                            10 * dock->applications->raw_window->dpi,
                            std::round(1.0 * dpi));
            cr->stroke();

            const auto text_x = c->real_bounds.x + (20 * dpi) + icon_size;
            const auto text_width = std::max(1.0, c->real_bounds.w - (30 * dpi) - icon_size);
            const int name_size = std::round(13 * dpi);
            const int path_size = std::round(10 * dpi);
            const RGBA name_color(0, 0, 0, 1);
            const RGBA path_color(.5, .5, .5, 1);
            auto name_bounds = draw_text(cr, 0, 0, name, name_size, false, set->font, -1, -1,
                                         name_color, true);
            auto path_bounds = draw_text(cr, 0, 0, full, path_size, false, set->font, -1, -1,
                                         path_color, false);
            const auto line_gap = 0 * dpi;
            const auto text_height = name_bounds.h + line_gap + path_bounds.h;
            const auto text_y = c->real_bounds.y + (c->real_bounds.h - text_height) * .5;
            draw_text(cr, text_x, text_y, name, name_size, true, set->font,
                      std::round(text_width), std::round(name_size * PANGO_SCALE), name_color, false);
            draw_text(cr, text_x, text_y + name_bounds.h + line_gap, full, path_size, true, set->font,
                      std::round(text_width), std::round(path_size * PANGO_SCALE), path_color, false);
            if (s->icon_surface) {
                const auto icon_height = cairo_image_surface_get_height(s->icon_surface.get());
                cr->set_source_surface(s->icon_surface.get(),
                    c->real_bounds.x + 10 * dpi,
                    center_y(c, icon_height));
                cr->paint_source();
            }

            if (needs_clip)
                cr->restore();
        };
        o->when_mouse_enters_container = [search](Container *root, Container *c) {
            auto &active_option = search->active_option; auto &keyboard_option_changed = search->keyboard_option_changed;
            int index = 0;
            int alive_index = 0;
            for (auto sibling : c->parent->children) {
                auto s = (Script *) sibling->user_data;
                s->selected = sibling == c;
                if (sibling == c)
                    index = alive_index;
                if (sibling->exists)
                    alive_index++;
            }
            active_option = index;
            keyboard_option_changed = false;
            auto dock = (Dock *) root->user_data;
            windowing::redraw(dock->applications->raw_window);
        };
        o->when_clicked = [search](Container *root, Container *c) {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            auto dock = (Dock *) root->user_data;
            auto s = (Script *) c->user_data;
            activate_script(s);
            search->active_option = 0;
            windowing::close_window(dock->applications->raw_window);
        };
    };

    auto field = make_field(padded, false, "", [search](std::string text) {
        auto &field_text = search->field_text; auto &active_option = search->active_option; auto &keyboard_option_changed = search->keyboard_option_changed;
        field_text = std::move(text);
        active_option = 0;
        keyboard_option_changed = false;
        search->settings_results = settings::search(field_text, 8);
        search->settings_rows_stale = true;
    });
    search->field = field;
    field->name = "applications_search";
    static_cast<STextEditorData *>(field->user_data)->options.on_tab = [](Container *root, Container *c, bool previous) {
        applications_move_focus(root, c, previous);
    };
    auto edit_key = std::move(field->when_key_event);
    field->when_key_event = [edit_key, search](Container *root, Container *c, int key, bool pressed, xkb_keysym_t sym,
                                     int mods, bool is_text, std::string text) {
        auto &field_text = search->field_text; auto &active_option = search->active_option; auto &keyboard_option_changed = search->keyboard_option_changed;
        if (!c->active)
            return;
        edit_key(root, c, key, pressed, sym, mods, is_text, text);
        if (pressed) {
            auto dock = (Dock *) root->user_data;
            if (sym == XKB_KEY_Escape) {
                windowing::close_window(dock->applications->raw_window);
                active_option = 0;
            } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                if (auto o_ = container_by_name("scroll_content", root)) {
                    auto o = ((ScrollContainer *) o_)->content;
                    bool none_matched = true;
                    for (auto ch: o->children) {
                        if (ch->exists) {
                            auto s = (Script *) ch->user_data;
                            if (s->selected) {
                                // The row's copy is from the last layout.
                                if (s->is_run_command)
                                    s->full_path = field_text;
                                activate_script(s);
                                active_option = 0;
                                none_matched = false;
                            }
                        }
                    }
                    if (none_matched) {
                        if (!field_text.empty())
                            launch_command(field_text);
                    }
                }
                windowing::close_window(dock->applications->raw_window);
            } else if (sym == XKB_KEY_Up) {
                active_option--;
                keyboard_option_changed = true;
                windowing::redraw(dock->applications->raw_window);
            } else if (sym == XKB_KEY_Down) {
                active_option++;
                keyboard_option_changed = true;
                windowing::redraw(dock->applications->raw_window);
            }
        }
    };

    auto scroll_parent = padded->child(FILL_SPACE, FILL_SPACE);
    ScrollPaneSettings scroll_settings(1.0);
    scroll_settings.right_inline_track = true;
    // Rows are 56px tall and the list can hold hundreds of entries.
    scroll_settings.wheel_step = 84;
    scroll_settings.wheel_max_acceleration = 4;
    auto scroll = make_newscrollpane_as_child(scroll_parent, scroll_settings, [](Container *root) {
        auto dock = ((Dock *) root->user_data);
        auto applications = dock->applications;
        return DrawContext({applications->raw_window->drawing_context, applications->raw_window->dpi, [dock, applications]() {
            // Scroll animations can finish after this popup has closed or been replaced.
            if (dock->applications != applications)
                return;
            windowing::redraw(applications->raw_window);
        }});
    });
    auto scroll_content = scroll->content;
    scroll->name = "scroll_content";
    scroll->pre_layout = [search](Container *root, Container *c_, const Bounds &b) {
        auto &field_text = search->field_text; auto &active_option = search->active_option; auto &keyboard_option_changed = search->keyboard_option_changed;
        auto scroll = ((ScrollContainer *) c_);
        auto animation_active = datum<bool>(scroll, "applications_scroll_animation_active");
        if (*animation_active) {
            auto animation_start = datum<double>(scroll, "applications_scroll_animation_start");
            auto animation_target = datum<double>(scroll, "applications_scroll_animation_target");
            auto animation_start_time = datum<long>(scroll, "applications_scroll_animation_start_time");
            const auto elapsed = get_current_time_in_ms() - *animation_start_time;
            const auto progress = std::clamp(elapsed / 180.0, 0.0, 1.0);
            const auto eased_progress = 1.0 - (1.0 - progress) * (1.0 - progress) * (1.0 - progress);
            scroll->scroll_v_visual = *animation_start + (*animation_target - *animation_start) * eased_progress;
            if (progress == 1.0) {
                scroll->scroll_v_visual = *animation_target;
                *animation_active = false;
            } else {
                auto dock = (Dock *) root->user_data;
                windowing::redraw(dock->applications->raw_window);
            }
        }
        auto data = (ScrollData *) scroll->user_data;
        auto c = scroll->content;
        if (data->func) {
            DrawContext ctx = data->func(root);
            int amount = std::round(12.0f * (ctx.dpi));
            scroll->right->children[0]->wanted_bounds.h = amount;
            scroll->right->children[2]->wanted_bounds.h = amount;
            scroll->bottom->children[0]->wanted_bounds.w = amount;
            scroll->bottom->children[2]->wanted_bounds.w = amount;
            scroll->settings.right_width = amount;
            scroll->settings.right_arrow_height = amount;
            scroll->settings.bottom_height = amount;
            scroll->settings.bottom_arrow_width = amount;
        }
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->applications->raw_window->dpi;
        c->spacing = 5 * dpi;
        if (search->settings_rows_stale) {
            search->settings_rows_stale = false;
            for (int i = (int) c->children.size() - 1; i >= 0; i--) {
                auto ch = c->children[i];
                if (((Script *) ch->user_data)->is_setting) {
                    c->children.erase(c->children.begin() + i);
                    delete ch;
                }
            }
            for (int i = 0; i < (int) search->settings_results.size(); i++) {
                const auto &r = search->settings_results[i];
                Script entry;
                entry.is_setting = true;
                entry.setting = r;
                entry.name = r.title;
                entry.full_path = "Settings  ›  " + r.breadcrumb;
                entry.base_order = i;
                auto it = search->history.find(selection_key(entry));
                entry.history = it == search->history.end() ? 0 : it->second;
                add_script_row(c, search, entry);
            }
        }
        int alive_count = 0;
        const auto lowercase_text = to_lower(field_text);
        for (auto ch: c->children) {
            auto s = (Script *) ch->user_data;
            s->selected = false;
            if (s->is_run_command) {
                s->full_path = field_text;
                s->match_level = field_text.find_first_not_of(' ') == std::string::npos ? -1 : 4;
            } else {
                // Settings come after every application and executable.
                s->match_level = s->is_setting ? 3 : match_script(*s, field_text, lowercase_text);
            }
            ch->exists = s->match_level >= 0;
            if (ch->exists)
                alive_count++;
        }
        std::stable_sort(c->children.begin(), c->children.end(), [](Container *a, Container *b) {
            if (a->exists != b->exists)
                return a->exists;
            auto sa = (Script *) a->user_data;
            auto sb = (Script *) b->user_data;
            if (sa->is_run_command != sb->is_run_command)
                return sb->is_run_command;
            if (sa->late != sb->late)
                return sb->late;
            if (sa->history != sb->history)
                return sa->history > sb->history;
            if (sa->match_level != sb->match_level)
                return sa->match_level < sb->match_level;
            return sa->base_order < sb->base_order;
        });
        if (alive_count != 0) {
            if (active_option < 0)
                active_option = 0;
            if (active_option > alive_count - 1)
                active_option = alive_count - 1;
        }
        int index_of_alive_child = 0;
        for (auto ch: c->children) {
            auto s = (Script *) ch->user_data;
            if (ch->exists) {
                if (index_of_alive_child == active_option)
                    s->selected = true;
                index_of_alive_child++;
            }
        }
        for (auto ch: c->children) {
            auto s = (Script *) ch->user_data;
            if (!s->selected)
                continue;
            auto animation_active = datum<bool>(scroll, "applications_scroll_animation_active");
            auto animation_option = datum<int>(scroll, "applications_scroll_animation_option");
            if (*animation_active && *animation_option == active_option) {
                keyboard_option_changed = false;
                break;
            }
            if (!keyboard_option_changed)
                break;
            const auto &viewport = scroll->real_bounds;
            double target = scroll->scroll_v_visual;
            if (ch->real_bounds.y < viewport.y)
                target += viewport.y - ch->real_bounds.y;
            else if (ch->real_bounds.y + ch->real_bounds.h > viewport.y + viewport.h)
                target += viewport.y + viewport.h - ch->real_bounds.y - ch->real_bounds.h;
            const auto min_offset = std::min(0.0, viewport.h - scroll->content->real_bounds.h);
            target = std::clamp(target, min_offset, 0.0);
            if (target != scroll->scroll_v_visual) {
                auto animation_start = datum<double>(scroll, "applications_scroll_animation_start");
                auto animation_target = datum<double>(scroll, "applications_scroll_animation_target");
                auto animation_start_time = datum<long>(scroll, "applications_scroll_animation_start_time");
                *animation_start = scroll->scroll_v_visual;
                *animation_target = target;
                *animation_start_time = get_current_time_in_ms();
                *animation_option = active_option;
                *animation_active = true;
                scroll->scroll_v_real = target;
                auto dock = (Dock *) root->user_data;
                windowing::redraw(dock->applications->raw_window);
            } else if (*animation_active) {
                *animation_active = false;
                scroll->scroll_v_real = scroll->scroll_v_visual;
            }
            keyboard_option_changed = false;
            break;
        }
    };

    auto rebuild_scripts = [scroll_content, scroll, search](const std::vector<Script> &entries) {
    auto &active_option = search->active_option; auto &keyboard_option_changed = search->keyboard_option_changed;
    for (auto child : scroll_content->children)
        delete child;
    scroll_content->children.clear();
    active_option = 0;
    keyboard_option_changed = false;
    scroll->scroll_v_real = 0;
    scroll->scroll_v_visual = 0;
    *datum<bool>(scroll, "applications_scroll_animation_active") = false;
    for (int i = 0; i < (int) entries.size(); i++) {
        if (entries[i].other_desktop && !set->applications_show_other_desktops)
            continue;
        Script entry = entries[i];
        entry.base_order = i;
        auto it = search->history.find(selection_key(entry));
        entry.history = it == search->history.end() ? 0 : it->second;
        add_script_row(scroll_content, search, entry);
    }
    Script run_command;
    run_command.is_run_command = true;
    run_command.name = "Run as command...";
    add_script_row(scroll_content, search, run_command);
    // Deleted above with the rest.
    search->settings_rows_stale = true;

    };
    /*
     * The list is first filled from the previous scan and a new scan starts
     * every time the menu opens. When it finishes, entries the list doesn't
     * have yet go to the end (see Script::late) and the ones it has stay where
     * they are, so nothing moves under a selection the user is making.
     * Entries that are gone stay until the menu is opened again.
     */
    auto merge_scripts = [scroll_content, search](const std::vector<Script> &entries) {
        std::unordered_map<std::string, Script *> listed;
        for (auto child : scroll_content->children) {
            auto s = (Script *) child->user_data;
            if (!s->is_setting && !s->is_run_command)
                listed[selection_key(*s)] = s;
        }
        int order = scroll_content->children.size();
        for (const auto &fresh : entries) {
            if (fresh.other_desktop && !set->applications_show_other_desktops)
                continue;
            auto key = selection_key(fresh);
            if (auto found = listed.find(key); found != listed.end()) {
                // What it launches and its icon may have changed.
                auto s = found->second;
                s->full_path = fresh.full_path;
                s->path = fresh.path;
                s->exec = fresh.exec;
                s->terminal = fresh.terminal;
                if (s->icon != fresh.icon) {
                    s->icon = fresh.icon;
                    s->icon_size = 0;
                }
                continue;
            }
            Script entry = fresh;
            entry.late = true;
            entry.base_order = order++;
            auto it = search->history.find(key);
            entry.history = it == search->history.end() ? 0 : it->second;
            add_script_row(scroll_content, search, entry);
        }
    };
    std::shared_future<std::vector<Script>> refresh;
    {
        std::lock_guard<std::mutex> lock(scripts_mutex);
        const bool refresh_ready = scripts_refresh.valid() &&
            scripts_refresh.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        if (refresh_ready)
            scripts = scripts_refresh.get();
        rebuild_scripts(scripts);
        if (!scripts_refresh.valid() || refresh_ready)
            scripts_refresh = load_scripts_async();
        refresh = scripts_refresh;
    }
    auto dock = static_cast<Dock *>(root->user_data);
    watch_applications_scripts(dock, dock->applications, refresh, merge_scripts);

    // The menu opens with the caret in the search field, blinking.
    set_active(root, {field}, root, true, false);

    auto bottom = padded->child(::hbox, FILL_SPACE, 32);
    bottom->alignment = ALIGN_RIGHT;
    bottom->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto dock = (Dock *) root->user_data;
        auto dpi = dock->applications->raw_window->dpi;
        c->wanted_bounds.h = 36 * dpi;
        //c->wanted_pad = Bounds(pad_amount, pad_amount, pad_amount, pad_amount).scale(dpi);
        c->spacing = 8 * dpi;
        for (auto ch : c->children) {
            ch->wanted_bounds.w = c->wanted_bounds.h;
            ch->wanted_bounds.h = c->wanted_bounds.h;
        }
    };
    static const std::vector<std::string> icons = {
        "\uE713", // Settings
        "\uE7E8", // PowerButton
    };
    static const char *button_names[] = {"applications_settings", "applications_power"};
    auto activate_button = [](Container *root, Container *c, int i, bool via_keyboard) {
        auto dock = (Dock *) root->user_data;
        if (i == 0) {
            windowing::close_window(dock->applications->raw_window);
            main_thread([]() {
                settings::start("Display");
            });
        } else {
            open_power_menu(root, c, via_keyboard);
        }
    };
    for (int i = 0; i < 2; i++) {
        auto b = bottom->child(FILL_SPACE, FILL_SPACE);
        b->name = button_names[i];
        b->when_paint = [i](Container *root, Container *c) {
            auto dock = (Dock *) root->user_data;
            auto dpi = dock->applications->raw_window->dpi;
            auto cr = dock->applications->raw_window->drawing_context;
            auto rb = c->real_bounds;
            const bool menu_open = i == 1 && dock->power_menu;
            if (c->state.mouse_pressing || c->state.mouse_hovering || c->active || menu_open) {
                set_argb(cr, c->state.mouse_pressing ? RGBA(0, 0, 0, .16) : RGBA(0, 0, 0, .08));
                drawRoundedRect(cr, rb.x, rb.y, rb.w, rb.h, 8 * dpi, 1.0);
                cr->fill();
            }
            if (c->active) {
                // Keyboard focus ring.
                set_argb(cr, accent);
                drawRoundedRect(cr, rb.x, rb.y, rb.w, rb.h, 8 * dpi, std::max(1.0, (double) std::round(1.5 * dpi)));
                cr->stroke();
            }

            auto b = draw_text(cr, 0, 0, icons[i], 16 * dpi, false, icon_font);
            draw_text(cr,
                c->real_bounds.x + c->real_bounds.w * .5 - b.w * .5,
                c->real_bounds.y + c->real_bounds.h * .5 - b.h * .5, icons[i], 16 * dpi, true, icon_font, -1, -1, {0, 0, 0, 1});
        };
        b->when_key_event = [i, activate_button](Container *root, Container *c, int, bool pressed, xkb_keysym_t sym,
                                                 int mods, bool, std::string) {
            if (!c->active || !pressed)
                return;
            auto dock = (Dock *) root->user_data;
            if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
                applications_move_focus(root, c, sym == XKB_KEY_ISO_Left_Tab || (mods & Modifier::MOD_SHIFT));
            } else if (sym == XKB_KEY_space || sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                activate_button(root, c, i, true);
            } else if (sym == XKB_KEY_Escape) {
                windowing::close_window(dock->applications->raw_window);
            }
        };
        b->when_clicked = [i, activate_button](Container *root, Container *c) {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            activate_button(root, c, i, false);
        };
    }
}

void preload_scripts() {
    std::lock_guard<std::mutex> lock(scripts_mutex);
    if (!scripts_refresh.valid())
        scripts_refresh = load_scripts_async();
}
