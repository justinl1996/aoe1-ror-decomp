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
// The primary surface's pixels inside rect changed.
void surface_present_primary(const unsigned char* bits, long pitch, long width, long height, const RECT* rect);
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

// GDI for the game's 8-bit surfaces (port/sdl/gdi.cpp), drawn with SDL_ttf.
//
// A device context from a surface (TDrawArea::GetDc) is one of gdi.cpp's;
// on it these functions draw into the surface's pixels. On any other device
// context they are the Win32 calls of the same name. Every GDI object the
// game creates comes from here; gdi_win32_object gives the real GDI object
// standing for one (for the EDIT boxes, which are real controls).
HFONT gdi_create_font_indirect(const LOGFONTA* logfont);
HPEN gdi_create_pen(int style, int width, COLORREF color);
HRGN gdi_create_rect_rgn(int left, int top, int right, int bottom);
HGDIOBJ gdi_get_stock_object(int index);
HGDIOBJ gdi_win32_object(HGDIOBJ object);
BOOL gdi_delete_object(HGDIOBJ object);
HGDIOBJ gdi_select_object(HDC dc, HGDIOBJ object);
int gdi_select_clip_rgn(HDC dc, HRGN region);
COLORREF gdi_set_text_color(HDC dc, COLORREF color);
COLORREF gdi_set_bk_color(HDC dc, COLORREF color);
int gdi_set_bk_mode(HDC dc, int mode);
BOOL gdi_text_out(HDC dc, int x, int y, LPCSTR text, int length);
BOOL gdi_get_text_extent_point32(HDC dc, LPCSTR text, int length, SIZE* size);
BOOL gdi_get_text_metrics(HDC dc, TEXTMETRICA* metrics);
int gdi_draw_text(HDC dc, LPCSTR text, int length, RECT* rect, UINT format);
int gdi_draw_text_ex(HDC dc, LPSTR text, int length, RECT* rect, UINT format, DRAWTEXTPARAMS* params);
BOOL gdi_move_to_ex(HDC dc, int x, int y, POINT* previous);
BOOL gdi_line_to(HDC dc, int x, int y);
int gdi_fill_rect(HDC dc, const RECT* rect, HBRUSH brush);
// Surface device contexts, for port/sdl/ddraw_memory.cpp.
HDC gdi_create_surface_dc(void);
void gdi_set_surface_dc_target(HDC dc, unsigned char* bits, long pitch, long width, long height,
                               const PALETTEENTRY* palette);
void gdi_delete_surface_dc(HDC dc);

#ifdef __cplusplus
}
#endif

#endif // SURFACE_H
