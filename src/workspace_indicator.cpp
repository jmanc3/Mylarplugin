#include "workspace_indicator.h"

#include "heart.h"
#include <cstring>

// Is this per monitor?
void workspace_indicator::on_change(int cid) {
    auto indicator = actual_root->child(FILL_SPACE, FILL_SPACE);
    indicator->custom_type = (int) TYPE::WORKSPACE_CHANGE_INDICATOR;
    indicator->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        
    };
    indicator->interactable = false;

    indicator->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage != (int) STAGE::RENDER_POST_WINDOWS)
            return;
        auto bounds = bounds_reserved_monitor(rid);
        bounds.scale(s);

        float w = std::round(200 * s);
        float h = std::round(34 * s);
        auto spaces_raw = hypriso->get_workspaces(rid);
        std::vector<int> spaces;
        bool has_first = false;
        for (auto s : spaces_raw)
            if (s == 1)
                has_first = true;
        if (has_first) {
            for (auto s : spaces_raw) {
                if (s == 1)
                    break;
                spaces.push_back(s);
            }
            for (int i = 1; i <= spaces_raw[spaces_raw.size() - 1]; i++)
                spaces.push_back(i);
        } else {
            for (auto s : spaces_raw)
                spaces.push_back(s);
        }

        if (spaces.size() == 1) {
            // This makes it so that a single dot never happens and that there is always atleast two
            spaces.push_back(12341234);
        }
        auto active = hypriso->get_active_workspace(rid);        

        int index = 0;
        for (int i = 0; i < spaces.size(); i++) {
            if (spaces[i] == active) {
                index = i;
                break;
            }
        }
        
        w = h * spaces.size();
        if (spaces.size() <= 1) {
            return;
        }
        
        auto b = Bounds(bounds.x + bounds.w * .5 - w * .5, bounds.y + bounds.h - h * 2, w, h);
        {
            auto larger = b;
            larger.x -= 4 * s;
            larger.w += 8 * s;
            auto sm = larger;
            sm.shrink(1.0);
            //render_drop_shadow(rid, 1, {0, 0, 0, 0.1}, h * .5, 2.0, sm);
            rect(larger, {0, 0, 0, 1}, 0, h * .5, 2.0, true);
            larger.shrink(1.0);
            border(larger, {0, 0, 0, 1}, 1, 0, h * .5, 2.0, true);
        }

        float dot_w = 4 * s;
        float start_x = b.x + h * .5 - dot_w * .5;
        float start_y = b.y + h * .5 - dot_w * .5;
        for (int i = 0; i < spaces.size(); i++) {
            auto col = RGBA(.5, .5, .5, 1);
            float size_boost = 0;
            if (i == index) {
                col = RGBA(.8, .8, .8, 1);
                size_boost = 2 * s;
            }
            
            rect({start_x - size_boost * .5, start_y - size_boost * .5,
                  dot_w + size_boost, dot_w + size_boost}, col, 0, dot_w * .5 + size_boost * .5, 2.0, false);
            start_x += h;
        }
        hypriso->damage_box(b.scale(1.0 / s));
    };

    later(650.0f, [indicator](Timer *) {
        for (int i = 0; i < actual_root->children.size(); i++) {
            auto c = actual_root->children[i];
            if (c == indicator) {
                delete c;
                actual_root->children.erase(actual_root->children.begin() + i);
                break;
            }
        }
        damage_all();
    });
}

