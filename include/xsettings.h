#pragma once

// Minimal XSETTINGS manager (_XSETTINGS_S0) for Xwayland DPI scaling.
//
// GTK3, Chromium, CEF (e.g. Spotify), and Electron on X11 read DPI from
// XSETTINGS (Gdk/UnscaledDPI / Xft/DPI) with higher priority than xrdb's
// Xft.dpi, and only receive live DPI change notifications via XSETTINGS.
// When another daemon (such as xsettingsd started by kded) already owns
// _XSETTINGS_S0 with stale DPI values, we copy its other settings (theme,
// icons, font, cursor) before taking ownership so apps keep their look.

namespace xsettings {
    // Sets the target X11 DPI (e.g. 96, 144, 192) and publishes it over XSETTINGS.
    // Safe to call from the compositor's main thread.
    void set_dpi(int dpi);

    // Stops the background XSETTINGS thread and releases _XSETTINGS_S0.
    void stop();
}
