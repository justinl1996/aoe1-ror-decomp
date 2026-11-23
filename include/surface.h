#ifndef SURFACE_H
#define SURFACE_H

// SDL2 window, display and sound layer (workflow/docs/sdl_port_tasks.md), in
// port/sdl/. SDL.h is included only by port/sdl/*.cpp, never by headers.
// Everything here has C linkage, so Dsutil.c can use it too.

#include <windows.h>
#include <mmsystem.h>
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
// The clipboard, as single-byte (Windows-1252) text. get returns the
// length copied into buffer (always 0-terminated).
int surface_get_clipboard_text(char* buffer, int size);
void surface_set_clipboard_text(const char* text, int length);
// The game window as a whole: show and raise it, its title, its client
// rectangle (game pixels), and DefWindowProcA for its messages.
void surface_show_main_window(void);
void surface_set_window_title(const char* title);
BOOL surface_get_client_rect(HWND wnd, RECT* rect);
LRESULT surface_def_window_proc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam);
// The desktop's size in pixels.
void surface_get_display_size(long* width, long* height);
// While mouse input is off, the game gets no mouse messages (it used to give
// the mouse capture to a hidden window while loading).
void surface_set_mouse_input(int enabled);
// In-game viewport mouse-wheel zoom (port/sdl/surface.cpp).
void surface_set_zoom_viewport(int enabled, long x, long y, long w, long h);
void surface_set_zoom_suspended(int suspended);
// A message box (MessageBoxA's MB_ICON* flags choose its look), and the
// default beep (nothing where there is none).
void surface_message_box(const char* text, const char* title, UINT type);
void surface_message_beep(void);
// One copy of the game at a time: 0 if another is running (it is brought
// to the front).
int surface_claim_single_instance(const char* name, const char* title);
void surface_release_single_instance(void);
// timeGetTime.
DWORD surface_time_get_time(void);
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

// The volume lines of the SDL mixer (port/sdl/dsound_memory.cpp), standing
// in for the system mixer's wave-out line (every DirectSound buffer) and
// its CD or synthesizer line (music played by surface_mci_send_command).
// Volumes run from -10000 (silent) to 0 (full), linear in amplitude, as the
// game set them on the system mixer.
enum { SURFACE_SOUND_WAVE = 0, SURFACE_SOUND_MUSIC = 1 };
long surface_sound_get_volume(int line);
void surface_sound_set_volume(int line, long volume);

// Music (port/sdl/mci.cpp): mciSendCommandA for the cdaudio and sequencer
// devices, playing WAV files through the SDL mixer: CD tracks ripped to
// music/trackNN.wav, and WAV renderings of MIDI files. Anything else goes
// to the system's MCI.
MCIERROR WINAPI surface_mci_send_command(MCIDEVICEID id, UINT msg, DWORD flags, DWORD params);
BOOL surface_mci_get_error_string(MCIERROR error, LPSTR text, UINT length);

// WINMM stand-ins for the WAV stream (port/sdl/winmm.cpp): timeSetEvent on
// SDL's timer thread, and the mmio RIFF file functions over stdio. They
// take and return what their WINMM namesakes do.
UINT surface_time_set_event(UINT delay, UINT resolution, LPTIMECALLBACK proc, DWORD user, UINT flags);
MMRESULT surface_time_kill_event(UINT id);
HMMIO surface_mmio_open(LPSTR name, LPMMIOINFO info, DWORD flags);
MMRESULT surface_mmio_close(HMMIO mmio, UINT flags);
LONG surface_mmio_read(HMMIO mmio, HPSTR data, LONG length);
LONG surface_mmio_write(HMMIO mmio, const char* data, LONG length);
LONG surface_mmio_seek(HMMIO mmio, LONG offset, int origin);
MMRESULT surface_mmio_get_info(HMMIO mmio, LPMMIOINFO info, UINT flags);
MMRESULT surface_mmio_set_info(HMMIO mmio, const MMIOINFO* info, UINT flags);
MMRESULT surface_mmio_advance(HMMIO mmio, LPMMIOINFO info, UINT flags);
MMRESULT surface_mmio_descend(HMMIO mmio, LPMMCKINFO chunk, const MMCKINFO* parent, UINT flags);
MMRESULT surface_mmio_ascend(HMMIO mmio, LPMMCKINFO chunk, UINT flags);
MMRESULT surface_mmio_create_chunk(HMMIO mmio, LPMMCKINFO chunk, UINT flags);

// Settings (port/sdl/settings.cpp): registry-style values kept in
// settings.ini next to the executable, under a key, for the current user
// (user != 0) or the machine. They return what RegQueryValueExA and
// RegSetValueExA return (ERROR_SUCCESS, ERROR_FILE_NOT_FOUND,
// ERROR_MORE_DATA with the size needed).
long surface_setting_get(int user, const char* key, const char* name, unsigned long* type, unsigned char* data,
                         unsigned long* size);
long surface_setting_set(int user, const char* key, const char* name, unsigned long type, const unsigned char* data,
                         unsigned long size);

// String tables (port/sdl/strings.cpp): the RT_STRING resources of a
// language DLL, read from the file. open takes the DLL's name, as
// LoadLibraryA did; load is LoadStringA (Windows-1252 text, 0 when the id
// has no string).
void* surface_string_table_open(const char* dll_name);
void surface_string_table_close(void* table);
int surface_string_table_load(void* table, unsigned int id, char* buffer, int max_length);

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

// GDI for the game's 8-bit surfaces (port/sdl/gdi.cpp), drawn with SDL_ttf.
//
// A device context from a surface (TDrawArea::GetDc) is one of gdi.cpp's;
// on it these functions draw into the surface's pixels. On any other device
// context they are the Win32 calls of the same name. Every GDI object the
// game creates comes from here; gdi_win32_object gives the real GDI object
// standing for one (for a device context that is not a surface's).
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
// Palettes: colour tables with GDI's calls. A palette handle may be passed
// as void* (the draw system keeps its palette as one). Selecting and
// realizing a palette does nothing, and there is no system palette (as on
// a true-colour display).
HPALETTE gdi_create_palette(const LOGPALETTE* logpalette);
HPALETTE gdi_create_halftone_palette(HDC dc);
UINT gdi_get_palette_entries(void* palette, UINT start, UINT count, PALETTEENTRY* entries);
UINT gdi_set_palette_entries(void* palette, UINT start, UINT count, const PALETTEENTRY* entries);
BOOL gdi_resize_palette(void* palette, UINT count);
UINT gdi_get_nearest_palette_index(void* palette, COLORREF color);
int gdi_get_object(void* object, int size, void* out);
HPALETTE gdi_select_palette(HDC dc, void* palette, BOOL background);
UINT gdi_realize_palette(HDC dc);
UINT gdi_get_system_palette_entries(HDC dc, UINT start, UINT count, PALETTEENTRY* entries);
// Surface device contexts, for port/sdl/ddraw_memory.cpp.
HDC gdi_create_surface_dc(void);
void gdi_set_surface_dc_target(HDC dc, unsigned char* bits, long pitch, long width, long height,
                               const PALETTEENTRY* palette);
void gdi_delete_surface_dc(HDC dc);

#ifdef __cplusplus
}
#endif

#endif // SURFACE_H
