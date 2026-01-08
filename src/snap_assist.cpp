#include "snap_assist.h"

#include "heart.h"

void snap_assist::open(int monitor, int cid) {
    auto c = get_cid_container(cid);
    if (!c)
        return;

    auto type = (SnapPosition) *datum<int>(c, "snap_type");
    if (type == SnapPosition::MAX || type == SnapPosition::NONE)
        return;
    
    auto cdata = (ClientInfo *) c->user_data;

    std::vector<int> ids;
    ids.push_back(cid);
    for (auto grouped_id : cdata->grouped_with)
        ids.push_back(grouped_id);
    
    std::vector<SnapPosition> open_slots;
    
    if (type == SnapPosition::LEFT || type == SnapPosition::RIGHT) {
        if (type == SnapPosition::LEFT) {
            if (groupable(SnapPosition::RIGHT, ids)) {
                open_slots.push_back(SnapPosition::RIGHT);
            } else if (groupable(SnapPosition::TOP_RIGHT, ids)) {
                open_slots.push_back(SnapPosition::TOP_RIGHT);
            } else if (groupable(SnapPosition::BOTTOM_RIGHT, ids)) {
                open_slots.push_back(SnapPosition::BOTTOM_RIGHT);
            }
        } else {
            if (groupable(SnapPosition::LEFT, ids)) {
                open_slots.push_back(SnapPosition::LEFT);
            } else if (groupable(SnapPosition::TOP_LEFT, ids)) {
                open_slots.push_back(SnapPosition::TOP_LEFT);
            } else if (groupable(SnapPosition::BOTTOM_LEFT, ids)) {
                open_slots.push_back(SnapPosition::BOTTOM_LEFT);
            }            
        }
    } else {
        if (type == SnapPosition::TOP_LEFT) {
            for (auto pos : {SnapPosition::BOTTOM_LEFT, SnapPosition::BOTTOM_RIGHT, SnapPosition::TOP_RIGHT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::BOTTOM_LEFT) {
            for (auto pos : {SnapPosition::TOP_LEFT, SnapPosition::TOP_RIGHT, SnapPosition::BOTTOM_RIGHT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::BOTTOM_RIGHT) {
            for (auto pos : {SnapPosition::TOP_RIGHT, SnapPosition::TOP_LEFT, SnapPosition::BOTTOM_LEFT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        } else if (type == SnapPosition::TOP_RIGHT) {
            for (auto pos : {SnapPosition::BOTTOM_RIGHT, SnapPosition::BOTTOM_LEFT, SnapPosition::TOP_LEFT})
                if (groupable(pos, ids))
                    open_slots.push_back(pos);
        }
    }

    // open containers
}

void snap_assist::close() {
    
}

