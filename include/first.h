#ifndef first_h_INCLUDED
#define first_h_INCLUDED

#include "globals.h"

extern Globals *globals;
extern bool started_directly_from_hyprland;

void init_mylar(void* h);
void exit_mylar(void* h);

#endif // first_h_INCLUDED
