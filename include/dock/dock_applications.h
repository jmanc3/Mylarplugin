#pragma once

struct Container;

void fill_applications_container(Container *root);

// Starts (or keeps) the asynchronous application scan used by the launcher.
void preload_scripts();
