#pragma once

struct Container;

extern bool nightlight_on;
// Starts or stops hyprsunset's warmer colour temperature.
void toggle_nightlight();

void fill_extra_container(Container *root);
