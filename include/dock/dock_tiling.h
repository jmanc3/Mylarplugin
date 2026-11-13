#pragma once

struct Dock;

void update_tiling_menu(Dock *dock);
void fill_tiling_container(Dock *dock);
void fill_tiling_info_container(Dock *dock);
int tiling_info_height(Dock *dock);
