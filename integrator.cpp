//
// Created by jmanc3 on 8/29/25.
//

#include "integrator.h"

#include "../plugin/Mylarplugin/include/first.h"

static void *api_handle = nullptr;

void init_plugin_from_hyprland(void *handle) {
    init_mylar(handle);
    api_handle = handle;
}

void exit_plugin_from_hyprland() {
    exit_mylar(api_handle);
}
