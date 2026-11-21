// SDL2 display layer (workflow/docs/sdl_port_tasks.md).
//
// The game keeps its own Win32 window and message loop. SDL attaches to that
// window with SDL_CreateWindowFrom and never pumps messages itself, so input,
// painting and the parity harness's injected messages reach the game exactly
// as before.

#include <windows.h>

#include "../../include/surface.h"

#ifdef AOE_SDL

// The VC6 C runtime lacks _aligned_malloc, which clang's intrinsic headers
// use. Nothing here needs SDL's intrinsics.
#define SDL_DISABLE_IMMINTRIN_H
#define SDL_DISABLE_MMINTRIN_H
#define SDL_DISABLE_XMMINTRIN_H
#define SDL_DISABLE_EMMINTRIN_H
#define SDL_DISABLE_PMMINTRIN_H
#include <SDL.h>

static SDL_Window* surface_window = 0;
static HWND surface_hwnd = 0;
static WNDPROC surface_game_proc = 0;
static WNDPROC surface_sdl_proc = 0;

// SDL subclasses the window and forwards every message to the game's
// procedure, but it validates the window before forwarding WM_PAINT (so
// GetUpdateRect in RGE_Base_Game::check_paint always saw an empty region)
// and swallows WM_ERASEBKGND. It also answers WM_NCCALCSIZE for a borderless
// window itself, which left the game's 800x600 popup with a 0x0 client area
// whenever it did not cover the whole screen. This procedure sits on top of
// SDL's and sends those three straight to the game. SDL only turns WM_PAINT
// and WM_ERASEBKGND into events, which surface_drop_events discards anyway.
static void present_repaint(void);

static LRESULT CALLBACK surface_wnd_proc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // A real display keeps showing the primary surface; a swap chain does
    // not survive being covered or (re)mapped. Show the last frame again
    // before the game handles the paint.
    if (msg == WM_PAINT)
        present_repaint();
    if (msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_NCCALCSIZE)
        return CallWindowProcA((FARPROC) surface_game_proc, wnd, msg, wparam, lparam);
    return CallWindowProcA((FARPROC) surface_sdl_proc, wnd, msg, wparam, lparam);
}

// The game reads input from its own window messages. SDL still turns the
// messages it sees into events; drop them so its queue never fills.
static int SDLCALL surface_drop_events(void* userdata, SDL_Event* event) {
    return 0;
}

int surface_attach_window(HWND wnd) {
    if (surface_window != 0)
        return 1;

    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "0");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return 0;

    SDL_SetEventFilter(surface_drop_events, 0);
    surface_game_proc = (WNDPROC) GetWindowLongA(wnd, GWL_WNDPROC);
    surface_window = SDL_CreateWindowFrom(wnd);
    if (surface_window == 0) {
        SDL_Quit();
        return 0;
    }
    surface_hwnd = wnd;
    surface_sdl_proc = (WNDPROC) SetWindowLongA(wnd, GWL_WNDPROC, (LONG) surface_wnd_proc);
    return 1;
}

// ---------------------------------------------------------------------------
// Presenting the primary surface.
//
// The primary surface is 8-bit memory (port/sdl/ddraw_memory.cpp). Each
// change converts the changed rectangle through the palette into a
// streaming texture of the same size, and SDL shows the texture in the
// window. Presenting is throttled: a change presents at once unless the last
// present was very recent; then it is left pending and
// surface_present_flush (called from the game's main loop) presents it.

enum { PRESENT_MIN_INTERVAL_MS = 4 };

static SDL_Renderer* present_renderer = 0;
static SDL_Texture* present_texture = 0;
static HWND present_wnd = 0;
static int present_exclusive = 0;
static long present_width = 0;
static long present_height = 0;
static Uint32 present_lut[256];
static int present_pending = 0;
static Uint32 present_last_ticks = 0;

static void present_destroy_target(void) {
    if (present_texture != 0) {
        SDL_DestroyTexture(present_texture);
        present_texture = 0;
    }
    if (present_renderer != 0) {
        SDL_DestroyRenderer(present_renderer);
        present_renderer = 0;
    }
    present_pending = 0;
}

void surface_present_set_target(HWND wnd, int exclusive, long width, long height) {
    present_wnd = wnd;
    present_exclusive = exclusive;
    if (surface_window == 0 || wnd != surface_hwnd)
        return;

    if (present_renderer == 0) {
        // Nearest-neighbour scaling keeps the pixel art sharp. No vsync: a
        // present must never stall the game loop.
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        present_renderer = SDL_CreateRenderer(surface_window, -1, SDL_RENDERER_ACCELERATED);
        if (present_renderer == 0)
            present_renderer = SDL_CreateRenderer(surface_window, -1, SDL_RENDERER_SOFTWARE);
        if (present_renderer == 0)
            return;
    }
    // Outside exclusive mode the primary surface is the desktop; the texture
    // holds only the window's client area.
    if (exclusive == 0) {
        RECT client;
        GetClientRect(wnd, &client);
        width = client.right;
        height = client.bottom;
    }
    if (present_texture == 0 || width != present_width || height != present_height) {
        if (present_texture != 0)
            SDL_DestroyTexture(present_texture);
        present_texture =
            SDL_CreateTexture(present_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        present_width = width;
        present_height = height;
    }
    SDL_RenderSetLogicalSize(present_renderer, width, height);
}

void surface_present_palette(const PALETTEENTRY* entries) {
    for (int i = 0; i < 256; ++i)
        present_lut[i] =
            0xFF000000u | ((Uint32) entries[i].peRed << 16) | ((Uint32) entries[i].peGreen << 8) | entries[i].peBlue;
}

static void present_now(void) {
    SDL_RenderClear(present_renderer);
    SDL_RenderCopy(present_renderer, present_texture, 0, 0);
    SDL_RenderPresent(present_renderer);
    present_pending = 0;
    present_last_ticks = SDL_GetTicks();
}

void surface_present_primary(const unsigned char* bits, long pitch, long width, long height, const RECT* rect, HDC dc) {
    if (present_texture == 0)
        return;

    // Outside exclusive mode the primary surface is the desktop; only the
    // part under the window's client area is shown.
    RECT r = *rect;
    POINT origin;
    origin.x = 0;
    origin.y = 0;
    if (present_exclusive == 0)
        ClientToScreen(present_wnd, &origin);
    if (r.left < origin.x)
        r.left = origin.x;
    if (r.top < origin.y)
        r.top = origin.y;
    if (r.right > origin.x + present_width)
        r.right = origin.x + present_width;
    if (r.bottom > origin.y + present_height)
        r.bottom = origin.y + present_height;
    if (r.right > width)
        r.right = width;
    if (r.bottom > height)
        r.bottom = height;
    if (r.left >= r.right || r.top >= r.bottom)
        return;

    SDL_Rect area;
    area.x = r.left - origin.x;
    area.y = r.top - origin.y;
    area.w = r.right - r.left;
    area.h = r.bottom - r.top;
    void* pixels;
    int texture_pitch;
    if (SDL_LockTexture(present_texture, &area, &pixels, &texture_pitch) != 0)
        return;
    for (int y = 0; y < area.h; ++y) {
        const unsigned char* in = bits + (r.top + y) * pitch + r.left;
        Uint32* out = (Uint32*) ((unsigned char*) pixels + y * texture_pitch);
        for (int x = 0; x < area.w; ++x)
            out[x] = present_lut[in[x]];
    }
    SDL_UnlockTexture(present_texture);

    if (SDL_GetTicks() - present_last_ticks >= PRESENT_MIN_INTERVAL_MS)
        present_now();
    else
        present_pending = 1;
}

static void present_repaint(void) {
    if (present_renderer != 0 && present_texture != 0)
        present_now();
}

void surface_present_flush(void) {
    if (present_pending != 0 && present_renderer != 0)
        present_now();
}

void surface_detach_window(void) {
    present_destroy_target();
    if (surface_window != 0) {
        // SDL restores the game's procedure only if its own is on top.
        SetWindowLongA(surface_hwnd, GWL_WNDPROC, (LONG) surface_sdl_proc);
        SDL_DestroyWindow(surface_window);
        surface_window = 0;
    }
    SDL_Quit();
}

#endif // AOE_SDL
