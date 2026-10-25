#ifndef dock_thumbnails_h_INCLUDED
#define dock_thumbnails_h_INCLUDED

#include <string>
#include <vector>

#include "container.h"

// Hover preview for dock icons. All entry points must be called on the
// compositor main thread (wrap calls from the dock thread in main_thread()).
namespace dock_thumbnails {
    // Shows (or refreshes) a preview for the given client ids next to the dock.
    // `icon` is the owning dock icon's bounds in dock-local logical pixels and
    // is used to center the preview on the icon. `source` identifies the dock
    // icon so the preview can be updated when its window list changes.
    void open(const std::vector<int> &ids, const Bounds &icon = {}, const std::string &source = "");

    // Replaces or repositions a preview that is already visible. Does nothing
    // when no preview is open. When `allow_switch` is false only the icon that
    // owns the preview may update it; hovering a new icon passes true so the
    // preview can switch immediately without waiting for the hover delay.
    void refresh_if_open(const std::string &source, const std::vector<int> &ids, const Bounds &icon,
                         bool allow_switch = false);

    // Starts a short grace period. The preview is kept if the cursor moves onto
    // it before the grace period expires.
    void close();

    // Cancels a pending close, e.g. when the cursor returns to the dock icon.
    void keep_open();

    // Removes one window from the visible preview and rebuilds with the rest.
    // Call on the compositor main thread. Destroys the preview if no ids remain.
    void remove_window(int cid);
};

#endif // dock_thumbnails_h_INCLUDED
