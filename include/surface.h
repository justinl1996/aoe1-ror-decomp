#ifndef SURFACE_H
#define SURFACE_H

// SDL2 window, display and sound layer (workflow/docs/sdl_port_tasks.md), in
// port/sdl/. SDL.h is included only by port/sdl/*.cpp, never by headers.
// Everything here has C linkage, so Dsutil.c can use it too.

#include <windows.h>
#include <ddraw.h>

#ifdef __cplusplus
extern "C" {
#endif

// The game window (port/sdl/window.cpp). SDL creates and owns it; the game
// knows it by the returned handle, and its window procedure gets messages
// from surface_peek_message / surface_get_message / surface_dispatch_message
// instead of from Windows. Returns 0 on failure.
HWND surface_create_main_window(const char* title, int width, int height, WNDPROC game_proc);
// Destroy the game window and shut SDL down.
void surface_destroy_main_window(void);

// The game's message loop. These follow PeekMessageA (with PM_REMOVE),
// GetMessageA and DispatchMessageA. Messages come in Windows' order: posted,
// then input, then WM_QUIT, then WM_PAINT, then WM_TIMER. Key messages are
// already translated (WM_CHAR included), so there is no TranslateMessage.
BOOL surface_peek_message(MSG* msg);
BOOL surface_get_message(MSG* msg);
LRESULT surface_dispatch_message(const MSG* msg);

// The game's calls on its window. For the game window they work on the
// queue and state kept in window.cpp; for any other window they are the
// Win32 calls of the same name. surface_post_message may be called from any
// thread.
BOOL surface_post_message(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
LRESULT surface_send_message(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
void surface_post_quit_message(int exit_code);
UINT surface_set_timer(HWND wnd, UINT id, UINT elapse);
BOOL surface_kill_timer(HWND wnd, UINT id);
BOOL surface_invalidate_rect(HWND wnd, const RECT* rect, BOOL erase);
BOOL surface_validate_rect(HWND wnd, const RECT* rect);
BOOL surface_get_update_rect(HWND wnd, RECT* rect, BOOL erase);
BOOL surface_update_window(HWND wnd);
BOOL surface_is_iconic(HWND wnd);
HWND surface_get_foreground_window(void);
// The system cursor over the game window: hidden for 0 (the game draws its
// own), the hourglass for LoadCursorA(0, IDC_WAIT), otherwise the arrow.
// Stands in for both SetCursor and SetClassLongA(..., GCL_HCURSOR, ...).
void surface_set_cursor(HCURSOR cursor);
HWND surface_set_capture(HWND wnd);
BOOL surface_release_capture(void);
HWND surface_get_capture(void);
// Key state as of the key messages the game has received.
SHORT surface_get_key_state(int vk);
SHORT surface_get_async_key_state(int vk);
BOOL surface_get_keyboard_state(BYTE* state);

// In-memory DirectDraw (port/sdl/ddraw_memory.cpp). Takes the place of
// DirectDrawCreate: the game's DirectDraw calls then draw into memory
// surfaces, and writes to the primary surface reach the window through the
// surface_present_* functions below.
HRESULT WINAPI surface_DirectDrawCreate(GUID FAR* guid, LPDIRECTDRAW FAR* out, IUnknown FAR* outer);

#ifdef __DSOUND_INCLUDED__
// In-memory DirectSound mixed by SDL (port/sdl/dsound_memory.cpp). Takes the
// place of DirectSoundCreate. Fails, so the game runs silent, when SDL has
// no audio device.
HRESULT WINAPI surface_DirectSoundCreate(LPGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer);
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

// Window layout. The game window is SDL's desktop fullscreen (the default)
// or an ordinary resizable window; either way the game's frame is scaled to
// fit, keeping its aspect ratio. Alt+Enter switches between the two. Call
// before the window exists to set the starting layout.
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

#ifdef __cplusplus
}
#endif

#endif // SURFACE_H
