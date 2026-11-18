#ifndef mpris_h_INCLUDED
#define mpris_h_INCLUDED

#include <functional>
#include <string>
#include <vector>

// Tracks MPRIS media players (org.mpris.MediaPlayer2.*) on the session bus and
// sends them transport commands. Everything here runs on the compositor main
// thread, which is the thread that dispatches the session bus connection.
// All D-Bus traffic is asynchronous; nothing blocks the compositor.
namespace mpris {
    struct Player {
        std::string name;  // well known bus name, e.g. org.mpris.MediaPlayer2.firefox.instance_1_23
        std::string owner; // unique bus name (signals are sent from this)
        int pid = -1;      // process that owns the bus name
        std::string identity;
        std::string desktop_entry;
        std::string status; // "Playing", "Paused", "Stopped" or "" when unknown
        std::string title;
        std::string artist;
        bool can_control = true;
        bool can_play = true;
        bool can_pause = true;
        bool can_go_next = false;
        bool can_go_previous = false;

        bool playing() const { return status == "Playing"; }
        // A player worth offering controls for: it has (or had) media loaded.
        bool active() const { return can_control && (status == "Playing" || status == "Paused" || !title.empty()); }
    };

    // Installs the signal match and lists the players already running. Cheap to
    // call repeatedly; does nothing until the session bus is connected.
    void ensure_started();

    // Removes the filter and cancels outstanding calls. Must be called before
    // the session bus connection is closed.
    void stop();

    // Re-reads the properties of every known player. Some players do not emit
    // PropertiesChanged for everything, so call this when controls are shown.
    void refresh();

    const std::vector<Player> &players();

    // Called (on the main thread) whenever a player appears, disappears or
    // changes state.
    void set_on_change(std::function<void()> callback);

    void play_pause(const std::string &name);
    void next(const std::string &name);
    void previous(const std::string &name);
}

#endif // mpris_h_INCLUDED
