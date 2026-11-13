#include "datetime.h"

#include "hypriso.h"
#include "process.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct RunResult {
    int status = -1;
    std::string output;
};

// Runs argv directly (no shell) and collects stdout+stderr.
RunResult run(const std::vector<std::string> &argv) {
    RunResult result;
    std::mutex m;
    auto collect = [&result, &m](const char *bytes, size_t n) {
        std::lock_guard lock(m);
        result.output.append(bytes, n);
    };
    TinyProcessLib::Process process(argv, "", collect, collect);
    result.status = process.get_exit_status();
    return result;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n'))
        i++;
    return s.substr(i);
}

// ---------------- org.freedesktop.timedate1 (what GNOME uses) ----------------

const char *TIMEDATE_DEST = "--dest=org.freedesktop.timedate1";
const char *TIMEDATE_PATH = "/org/freedesktop/timedate1";

bool timedated_available() {
    static std::once_flag once;
    static bool available = false;
    std::call_once(once, []() {
        for (auto method : {"org.freedesktop.DBus.ListActivatableNames", "org.freedesktop.DBus.ListNames"}) {
            auto r = run({"dbus-send", "--system", "--print-reply", "--dest=org.freedesktop.DBus",
                          "/org/freedesktop/DBus", method});
            if (r.status == 0 && r.output.find("\"org.freedesktop.timedate1\"") != std::string::npos) {
                available = true;
                return;
            }
        }
    });
    return available;
}

std::string timedated_property(const std::string &name) {
    auto r = run({"dbus-send", "--system", "--print-reply=literal", TIMEDATE_DEST, TIMEDATE_PATH,
                  "org.freedesktop.DBus.Properties.Get", "string:org.freedesktop.timedate1", "string:" + name});
    if (r.status != 0)
        return "";
    return trim(r.output);
}

std::string timedated_call(const std::string &method, std::vector<std::string> args) {
    std::vector<std::string> argv = {"dbus-send", "--system", "--print-reply", TIMEDATE_DEST, TIMEDATE_PATH,
                                     "org.freedesktop.timedate1." + method};
    argv.insert(argv.end(), args.begin(), args.end());
    auto r = run(argv);
    if (r.status == 0)
        return "";
    auto msg = trim(r.output);
    if (msg.find("NotAuthorized") != std::string::npos || msg.find("AccessDenied") != std::string::npos ||
        msg.find("authentication") != std::string::npos)
        return "Not authorized to change the date and time";
    return msg.empty() ? "Failed to change the date and time" : msg;
}

// ---------------- Fallback for systems without timedated ----------------

const char *NTP_SERVICES[] = {"chronyd", "ntpd", "openntpd"};

struct RunitDirs {
    std::string available; // where service definitions live
    std::string enabled;   // the supervised directory (symlinks)
};

bool find_runit(RunitDirs &dirs) {
    static const RunitDirs candidates[] = {
        {"/etc/sv", "/var/service"},             // Void
        {"/etc/runit/sv", "/run/runit/service"}, // Artix
    };
    for (auto &c : candidates) {
        std::error_code ec;
        if (fs::is_directory(c.available, ec) && fs::is_directory(c.enabled, ec)) {
            dirs = c;
            return true;
        }
    }
    return false;
}

bool is_openrc() {
    std::error_code ec;
    return fs::exists("/sbin/openrc-run", ec) || fs::exists("/usr/sbin/openrc-run", ec) ||
           fs::exists("/usr/bin/openrc-run", ec);
}

std::string pkexec_error(const RunResult &r) {
    if (r.status == 126)
        return "Authentication was dismissed or denied";
    if (r.status == 127)
        return "Could not get permission (is a polkit agent running?)";
    auto msg = trim(r.output);
    return msg.empty() ? "The command failed" : msg;
}

std::string run_as_root_shell(const std::string &script) {
    auto r = run({"pkexec", "/bin/sh", "-c", script});
    return r.status == 0 ? "" : pkexec_error(r);
}

bool fallback_can_ntp() {
    RunitDirs dirs;
    std::error_code ec;
    if (find_runit(dirs)) {
        for (auto s : NTP_SERVICES)
            if (fs::exists(dirs.available + "/" + s, ec))
                return true;
        return false;
    }
    if (is_openrc()) {
        for (auto s : NTP_SERVICES)
            if (fs::exists(std::string("/etc/init.d/") + s, ec))
                return true;
    }
    return false;
}

bool fallback_ntp_enabled() {
    RunitDirs dirs;
    std::error_code ec;
    if (find_runit(dirs)) {
        for (auto s : NTP_SERVICES)
            if (fs::exists(dirs.enabled + "/" + s, ec) || fs::is_symlink(dirs.enabled + "/" + s, ec))
                return true;
        return false;
    }
    if (is_openrc()) {
        for (auto s : NTP_SERVICES)
            if (fs::exists(std::string("/etc/init.d/") + s, ec) && run({"rc-service", s, "status"}).status == 0)
                return true;
    }
    return false;
}

std::string fallback_set_ntp(bool enabled) {
    RunitDirs dirs;
    std::error_code ec;
    if (find_runit(dirs)) {
        std::string script;
        if (enabled) {
            for (auto s : NTP_SERVICES) {
                if (fs::exists(dirs.available + "/" + s, ec)) {
                    script = "ln -sfn '" + dirs.available + "/" + s + "' '" + dirs.enabled + "/" + s + "'";
                    break;
                }
            }
        } else {
            for (auto s : NTP_SERVICES) {
                auto link = dirs.enabled + "/" + s;
                if (fs::is_symlink(link, ec) || fs::exists(link, ec))
                    script += (script.empty() ? "" : " && ") + std::string("rm -f '") + link + "'";
            }
            if (script.empty())
                return "";
        }
        if (script.empty())
            return "No NTP service (chronyd, ntpd or openntpd) is installed";
        return run_as_root_shell(script);
    }
    if (is_openrc()) {
        std::string script;
        for (auto s : NTP_SERVICES) {
            if (!fs::exists(std::string("/etc/init.d/") + s, ec))
                continue;
            if (enabled) {
                script = std::string("rc-update add ") + s + " default && rc-service " + s + " start";
                break;
            }
            script += (script.empty() ? "" : "; ") + std::string("rc-service ") + s + " stop; rc-update del " + s + " default";
        }
        if (script.empty())
            return "No NTP service (chronyd, ntpd or openntpd) is installed";
        if (!enabled)
            script += "; true";
        return run_as_root_shell(script);
    }
    return "Automatic date & time isn't supported on this system";
}

bool valid_timezone_id(const std::string &id) {
    if (id.empty() || id.front() == '/' || id.find("..") != std::string::npos)
        return false;
    for (char ch : id)
        if (!(std::isalnum((unsigned char) ch) || ch == '/' || ch == '_' || ch == '-' || ch == '+'))
            return false;
    std::error_code ec;
    return fs::is_regular_file("/usr/share/zoneinfo/" + id, ec);
}

// ---------------- Zone lookup ----------------

const std::chrono::time_zone *zone_for(const std::string &id) {
    // Trivially destructible on purpose: a thread_local std::string registers a
    // thread-exit destructor that keeps the plugin from unloading.
    thread_local char cached_id[128];
    thread_local const std::chrono::time_zone *cached = nullptr;
    if (cached && id == cached_id)
        return cached;
    const std::chrono::time_zone *zone = nullptr;
    try {
        zone = std::chrono::locate_zone(id);
    } catch (...) {
        try {
            zone = std::chrono::current_zone();
        } catch (...) {
            zone = nullptr;
        }
    }
    if (id.size() < sizeof(cached_id)) {
        memcpy(cached_id, id.c_str(), id.size() + 1);
        cached = zone;
    } else {
        cached = nullptr;
    }
    return zone;
}

std::string city_from_id(const std::string &id) {
    auto slash = id.rfind('/');
    std::string city = slash == std::string::npos ? id : id.substr(slash + 1);
    std::replace(city.begin(), city.end(), '_', ' ');
    return city;
}

} // namespace

namespace datetime {

std::string current_timezone() {
    std::error_code ec;
    auto target = fs::read_symlink("/etc/localtime", ec);
    if (!ec) {
        auto s = target.string();
        auto pos = s.find("zoneinfo/");
        if (pos != std::string::npos)
            return s.substr(pos + 9);
    }
    std::ifstream in("/etc/timezone");
    std::string line;
    if (in && std::getline(in, line) && !trim(line).empty())
        return trim(line);
    try {
        return std::string(std::chrono::current_zone()->name());
    } catch (...) {
        return "UTC";
    }
}

std::chrono::local_seconds now_local() {
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    if (auto zone = zone_for(current_timezone()))
        return zone->to_local(now);
    return std::chrono::local_seconds(now.time_since_epoch());
}

std::tm now_tm() {
    using namespace std::chrono;
    auto lt = now_local();
    auto day = floor<days>(lt);
    year_month_day ymd{day};
    hh_mm_ss hms{lt - day};
    std::tm t{};
    t.tm_year = (int) ymd.year() - 1900;
    t.tm_mon = (int) (unsigned) ymd.month() - 1;
    t.tm_mday = (int) (unsigned) ymd.day();
    t.tm_hour = (int) hms.hours().count();
    t.tm_min = (int) hms.minutes().count();
    t.tm_sec = (int) hms.seconds().count();
    t.tm_wday = (int) weekday{day}.c_encoding();
    t.tm_yday = (int) (day - local_days{ymd.year() / January / 1}).count();
    t.tm_isdst = -1;
    return t;
}

std::string clock_text(const std::tm &t) {
    char buf[32];
    if (set && set->clock_24h) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    } else {
        int h = t.tm_hour % 12;
        if (h == 0)
            h = 12;
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
    }
    return buf;
}

std::string clock_text() {
    return clock_text(now_tm());
}

std::time_t local_to_unix(int y, int mon, int d, int h, int min, int sec) {
    using namespace std::chrono;
    local_seconds lt = local_days{year{y} / month{(unsigned) (mon + 1)} / day{(unsigned) d}} + hours{h} + minutes{min} + seconds{sec};
    if (auto zone = zone_for(current_timezone())) {
        try {
            return system_clock::to_time_t(zone->to_sys(lt, choose::earliest));
        } catch (...) {
        }
    }
    return system_clock::to_time_t(sys_seconds{lt.time_since_epoch()});
}

std::string long_datetime_text() {
    auto t = now_tm();
    char date[64];
    std::strftime(date, sizeof(date), "%B %Y", &t);
    return std::to_string(t.tm_mday) + " " + date + ", " + clock_text(t);
}

std::string timezone_abbreviation(const std::string &id) {
    try {
        auto zone = std::chrono::locate_zone(id);
        return zone->get_info(std::chrono::system_clock::now()).abbrev;
    } catch (...) {
        return "";
    }
}

std::string timezone_label(const std::string &id) {
    std::string city = city_from_id(id);
    std::string country;
    for (auto &entry : timezones()) {
        if (entry.id == id) {
            city = entry.city;
            country = entry.country;
            break;
        }
    }
    std::string place = country.empty() ? city : city + ", " + country;
    auto abbrev = timezone_abbreviation(id);
    if (abbrev.empty() || abbrev == city)
        return place;
    return abbrev + " (" + place + ")";
}

const std::vector<TimezoneEntry> &timezones() {
    static std::once_flag once;
    static std::vector<TimezoneEntry> list;
    std::call_once(once, []() {
        std::map<std::string, std::string> countries;
        {
            std::ifstream in("/usr/share/zoneinfo/iso3166.tab");
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                auto tab = line.find('\t');
                if (tab != std::string::npos)
                    countries[line.substr(0, tab)] = trim(line.substr(tab + 1));
            }
        }
        std::ifstream in("/usr/share/zoneinfo/zone1970.tab");
        if (!in)
            in.open("/usr/share/zoneinfo/zone.tab");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            std::stringstream ss(line);
            std::string codes, coords, id;
            std::getline(ss, codes, '\t');
            std::getline(ss, coords, '\t');
            std::getline(ss, id, '\t');
            if (id.empty())
                continue;
            auto code = codes.substr(0, codes.find(','));
            TimezoneEntry entry;
            entry.id = id;
            entry.city = city_from_id(id);
            auto it = countries.find(code);
            entry.country = it != countries.end() ? it->second : code;
            list.push_back(entry);
        }
        list.push_back({"UTC", "UTC", "Coordinated Universal Time"});
        std::sort(list.begin(), list.end(), [](const TimezoneEntry &a, const TimezoneEntry &b) {
            if (a.city != b.city)
                return a.city < b.city;
            return a.country < b.country;
        });
    });
    return list;
}

Status query_status() {
    Status status;
    status.timezone = current_timezone();
    if (timedated_available()) {
        auto can = timedated_property("CanNTP");
        status.can_ntp = can.empty() || can.find("true") != std::string::npos;
        status.ntp = timedated_property("NTP").find("true") != std::string::npos;
    } else {
        status.can_ntp = fallback_can_ntp();
        status.ntp = fallback_ntp_enabled();
    }
    return status;
}

std::string set_ntp(bool enabled) {
    if (timedated_available())
        return timedated_call("SetNTP", {enabled ? "boolean:true" : "boolean:false", "boolean:true"});
    return fallback_set_ntp(enabled);
}

std::string set_time(std::time_t unix_time) {
    if (timedated_available()) {
        long long usec = (long long) unix_time * 1000000LL;
        return timedated_call("SetTime", {"int64:" + std::to_string(usec), "boolean:false", "boolean:true"});
    }
    return run_as_root_shell("date -s @" + std::to_string((long long) unix_time) +
                             " >/dev/null && (hwclock --systohc 2>/dev/null; true)");
}

std::string set_timezone(const std::string &id) {
    if (!valid_timezone_id(id))
        return "Unknown time zone";
    if (timedated_available())
        return timedated_call("SetTimezone", {"string:" + id, "boolean:true"});
    auto r = run({"pkexec", "ln", "-sfn", "/usr/share/zoneinfo/" + id, "/etc/localtime"});
    return r.status == 0 ? "" : pkexec_error(r);
}

} // namespace datetime
