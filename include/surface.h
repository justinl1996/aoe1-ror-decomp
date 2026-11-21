#ifndef SURFACE_H
#define SURFACE_H

// SDL2 display layer (workflow/docs/sdl_port_tasks.md). Built only with
// AOE_SDL. SDL.h is included only by port/sdl/*.cpp, never by headers.

#include <windows.h>

#ifdef AOE_SDL

#include <ddraw.h>

// Attach SDL to the game's own window. The game keeps its window procedure
// and message loop; SDL only draws into the window. Returns 1 on success.
int surface_attach_window(HWND wnd);

// Release SDL's hold on the window and shut SDL down.
void surface_detach_window(void);

// In-memory DirectDraw (port/sdl/ddraw_memory.cpp). Takes the place of
// DirectDrawCreate: the game's DirectDraw calls then draw into memory
// surfaces, and writes to the primary surface reach the window through the
// surface_present_* functions below.
extern "C" HRESULT WINAPI surface_DirectDrawCreate(GUID FAR* guid, LPDIRECTDRAW FAR* out, IUnknown FAR* outer);

// Presenting the primary surface (port/sdl/surface.cpp).
//
// set_target: the primary surface was created for this window. In exclusive
// mode its pixels map to the window's client area; otherwise it stands for
// the desktop, and its pixels are in screen coordinates.
void surface_present_set_target(HWND wnd, int exclusive, long width, long height);
// The primary surface's palette changed.
void surface_present_palette(const PALETTEENTRY* entries);
// The primary surface's pixels inside rect changed. dc holds those pixels
// as a DIB section with the current palette as its color table.
void surface_present_primary(const unsigned char* bits, long pitch, long width, long height, const RECT* rect, HDC dc);

#endif // AOE_SDL

#endif // SURFACE_H
