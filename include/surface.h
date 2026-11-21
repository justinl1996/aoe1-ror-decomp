#ifndef SURFACE_H
#define SURFACE_H

// SDL2 display and sound layer (workflow/docs/sdl_port_tasks.md), in
// port/sdl/. SDL.h is included only by port/sdl/*.cpp, never by headers.

#include <windows.h>
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

#ifdef __DSOUND_INCLUDED__
// In-memory DirectSound mixed by SDL (port/sdl/dsound_memory.cpp). Takes the
// place of DirectSoundCreate. Fails, so the game runs silent, when SDL has
// no audio device.
extern "C" HRESULT WINAPI surface_DirectSoundCreate(LPGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer);
#endif

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
// Show a change that was held back because the last present was too recent.
// Called once per iteration of the game's main loop.
void surface_present_flush(void);

// Window layout (Task 4). The game window is a borderless window covering
// the screen (the default) or an ordinary resizable window; either way the
// game's frame is scaled to fit, keeping its aspect ratio. Alt+Enter
// switches between the two. Call before the primary surface exists to set
// the starting layout.
void surface_set_windowed(int windowed);

// Cursor and window coordinates as the game sees them.
//
// The window may be scaled, so the game works in a virtual screen: its
// coordinates are game pixels, and the game window's client area starts at
// its origin. Mouse messages are translated before the game sees them; the
// game's direct cursor and window-rectangle calls go through these
// functions.
BOOL surface_get_cursor_pos(POINT* point);
BOOL surface_set_cursor_pos(int x, int y);
BOOL surface_client_to_screen(HWND wnd, POINT* point);
BOOL surface_screen_to_client(HWND wnd, POINT* point);
BOOL surface_get_window_rect(HWND wnd, RECT* rect);
// Place a child window (an edit box) given in game pixels.
BOOL surface_move_child_window(HWND child, int x, int y, int width, int height, BOOL repaint);

#endif // SURFACE_H
