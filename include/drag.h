#pragma once

namespace drag {
    void begin(int client_id);
    void motion(int client_id);
    void end(int client_id);
    
    void snap_window(int snap_mon, int cid, int pos);
    void merge_client_into_existant_groups(int cid, bool create_snap_assist_if_needed);
    
    bool dragging();
    int drag_window();
}
