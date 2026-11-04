#pragma once

struct Dock;

void fill_brightness_container(Dock *dock);

float get_brightness();
void set_brightness(float amount);

extern float brightness_level;
