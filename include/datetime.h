#pragma once

#include <chrono>
#include <ctime>
#include <string>
#include <vector>

// Date & time helpers shared by the settings page and the dock.
//
// Mirrors GNOME's Date & Time panel: when systemd-timedated (org.freedesktop.timedate1)
// is on the system bus it is used exactly like gnome-control-center does (SetNTP,
// SetTime, SetTimezone with interactive polkit auth). Otherwise (runit, OpenRC, ...)
// the same operations are done through pkexec.
namespace datetime {
    struct TimezoneEntry {
        std::string id;      // "America/Chicago"
        std::string city;    // "Chicago"
        std::string country; // "United States"
    };

    struct Status {
        bool can_ntp = false;  // An NTP backend exists that we can toggle
        bool ntp = false;      // Automatic date & time is on
        std::string timezone;  // "America/Chicago"
    };

    // ---- Formatting (cheap, safe on any thread) ----

    // The current local time, in the zone /etc/localtime points at right now (so a
    // time zone change is picked up without restarting).
    std::chrono::local_seconds now_local();
    std::tm now_tm();

    // "14:05" or "2:05 PM" depending on the Time Format setting.
    std::string clock_text(const std::tm &t);
    std::string clock_text();

    // Converts a wall-clock time in the current zone (month is 0-11) to a unix time.
    std::time_t local_to_unix(int year, int month, int day, int hour, int minute, int second);

    // GNOME's Date & Time row label: "11 November 2026, 14:05" / "11 November 2026, 2:05 PM".
    std::string long_datetime_text();

    // "CST (Chicago, United States)".
    std::string timezone_label(const std::string &id);
    std::string timezone_abbreviation(const std::string &id);

    // ---- Queries / changes (blocking, call off the UI thread) ----

    Status query_status();
    std::string current_timezone();
    const std::vector<TimezoneEntry> &timezones();

    // Each returns an empty string on success, or an error message.
    std::string set_ntp(bool enabled);
    std::string set_time(std::time_t unix_time);
    std::string set_timezone(const std::string &id);
}
