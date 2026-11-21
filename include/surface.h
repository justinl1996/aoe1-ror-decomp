#ifndef SURFACE_H
#define SURFACE_H

// SDL2 display layer (workflow/docs/sdl_port_tasks.md). Built only with
// AOE_SDL. SDL.h is included only by src/surface.cpp, never by headers.

#include <windows.h>

#ifdef AOE_SDL

// Attach SDL to the game's own window. The game keeps its window procedure
// and message loop; SDL only draws into the window. Returns 1 on success.
int surface_attach_window(HWND wnd);

// Release SDL's hold on the window and shut SDL down.
void surface_detach_window(void);

#endif // AOE_SDL

#endif // SURFACE_H
