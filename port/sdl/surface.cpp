// SDL2 display layer (workflow/docs/sdl_port_tasks.md).
//
// The game keeps its own Win32 window and message loop. SDL attaches to that
// window with SDL_CreateWindowFrom and never pumps messages itself, so input,
// painting and the parity harness's injected messages reach the game exactly
// as before.

#include <windows.h>

#include "../../include/surface.h"


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
// whenever it did not cover the whole screen. And for a window it thinks is
// not resizable (the game's popup has no thick frame), its WM_GETMINMAXINFO
// pins the window to the size it last recorded, which defeated the window
// layout below. This procedure sits on top of SDL's and sends those four
// straight to the game. SDL only turns WM_PAINT and WM_ERASEBKGND into
// events, which surface_drop_events discards anyway.
static void present_repaint(void);
static int map_active(void);
static void map_client_to_game(POINT* point);
static void layout_toggle(void);

static LRESULT CALLBACK surface_wnd_proc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // A real display keeps showing the primary surface; a swap chain does
    // not survive being covered or (re)mapped. Show the last frame again
    // before the game handles the paint.
    if (msg == WM_PAINT)
        present_repaint();

    // The game sees mouse positions in game pixels, whatever the scale.
    if (msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK && map_active()) {
        POINT point;
        point.x = (short) LOWORD(lparam);
        point.y = (short) HIWORD(lparam);
        map_client_to_game(&point);
        lparam = MAKELPARAM(point.x, point.y);
    }

    // Alt+Enter switches between fullscreen and a window. The game never
    // sees it.
    if (msg == WM_SYSKEYDOWN && wparam == VK_RETURN && (lparam & (1 << 29)) != 0) {
        if ((lparam & (1 << 30)) == 0)
            layout_toggle();
        return 0;
    }
    if (msg == WM_SYSCHAR && wparam == '\r')
        return 0;
    if (msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_NCCALCSIZE || msg == WM_GETMINMAXINFO)
        return CallWindowProcA((FARPROC) surface_game_proc, wnd, msg, wparam, lparam);
    return CallWindowProcA((FARPROC) surface_sdl_proc, wnd, msg, wparam, lparam);
}

// The game reads input from its own window messages. SDL still turns the
// messages it sees into events; drop them so its queue never fills. Window
// events must pass: SDL runs its event watchers (the renderer's, which
// tracks the window size) only for events the filter lets through. They are
// discarded in surface_present_flush.
static int SDLCALL surface_drop_events(void* userdata, SDL_Event* event) {
    return event->type == SDL_WINDOWEVENT;
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

static void layout_apply(void);

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
    layout_apply();
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
    SDL_FlushEvent(SDL_WINDOWEVENT);
    if (present_pending != 0 && present_renderer != 0)
        present_now();
}

// ---------------------------------------------------------------------------
// Window layout and coordinates.
//
// Fullscreen is a borderless window covering the screen; windowed is an
// ordinary resizable window, sized to the largest whole multiple of the game
// resolution that fits the work area. Either way SDL scales the frame to the
// client area with letterboxing (SDL_RenderSetLogicalSize), and the game
// works in game pixels: mouse messages are translated in surface_wnd_proc,
// and the surface_* coordinate functions below stand in for the game's
// direct cursor and window-rectangle calls.

static int layout_windowed = 0;

void surface_set_windowed(int windowed) {
    layout_windowed = windowed != 0;
}

static void layout_apply(void) {
    if (surface_hwnd == 0 || present_width <= 0 || present_height <= 0)
        return;

    if (layout_windowed == 0) {
        SetWindowLongA(surface_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_SYSMENU);
        SetWindowPos(
            surface_hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            SWP_FRAMECHANGED | SWP_SHOWWINDOW
        );
    } else {
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN;
        RECT work;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0) == 0) {
            work.left = 0;
            work.top = 0;
            work.right = GetSystemMetrics(SM_CXSCREEN);
            work.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        long scale = 1;
        RECT frame;
        for (;;) {
            frame.left = 0;
            frame.top = 0;
            frame.right = present_width * (scale + 1);
            frame.bottom = present_height * (scale + 1);
            AdjustWindowRect(&frame, style, FALSE);
            if (frame.right - frame.left > work.right - work.left || frame.bottom - frame.top > work.bottom - work.top)
                break;
            ++scale;
        }
        frame.left = 0;
        frame.top = 0;
        frame.right = present_width * scale;
        frame.bottom = present_height * scale;
        AdjustWindowRect(&frame, style, FALSE);
        long width = frame.right - frame.left;
        long height = frame.bottom - frame.top;
        long x = work.left + (work.right - work.left - width) / 2;
        long y = work.top + (work.bottom - work.top - height) / 2;
        if (x < work.left)
            x = work.left;
        if (y < work.top)
            y = work.top;
        SetWindowLongA(surface_hwnd, GWL_STYLE, style);
        SetWindowPos(surface_hwnd, HWND_NOTOPMOST, x, y, width, height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        // The real frame can differ from AdjustWindowRect's (under Wine
        // without a window manager it does), and a window leaving
        // fullscreen may keep its old size. Measure and correct once.
        RECT window_rect;
        RECT client;
        GetWindowRect(surface_hwnd, &window_rect);
        GetClientRect(surface_hwnd, &client);
        long want_width = present_width * scale;
        long want_height = present_height * scale;
        if (client.right != want_width || client.bottom != want_height) {
            width = want_width + (window_rect.right - window_rect.left) - client.right;
            height = want_height + (window_rect.bottom - window_rect.top) - client.bottom;
            x = work.left + (work.right - work.left - width) / 2;
            y = work.top + (work.bottom - work.top - height) / 2;
            if (x < work.left)
                x = work.left;
            if (y < work.top)
                y = work.top;
            SetWindowPos(surface_hwnd, 0, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    InvalidateRect(surface_hwnd, 0, FALSE);
}

static void layout_toggle(void) {
    layout_windowed = !layout_windowed;
    layout_apply();
}

static int map_active(void) {
    return present_renderer != 0 && present_width > 0 && present_height > 0;
}

// Client pixel to game pixel, clamped to the game area (the letterbox bars
// count as its nearest edge, so edge scrolling still works there).
static void map_client_to_game(POINT* point) {
    float x;
    float y;
    SDL_RenderWindowToLogical(present_renderer, point->x, point->y, &x, &y);
    long gx = x < 0.0f ? -1 : (long) x;
    long gy = y < 0.0f ? -1 : (long) y;
    if (gx < 0)
        gx = 0;
    if (gy < 0)
        gy = 0;
    if (gx > present_width - 1)
        gx = present_width - 1;
    if (gy > present_height - 1)
        gy = present_height - 1;
    point->x = gx;
    point->y = gy;
}

// Game coordinate to client pixel. center: map a pixel's center (for the
// cursor) rather than its top-left corner (for rectangle edges).
static void map_game_to_client(POINT* point, int center) {
    int x;
    int y;
    float offset = center != 0 ? 0.5f : 0.0f;
    SDL_RenderLogicalToWindow(present_renderer, point->x + offset, point->y + offset, &x, &y);
    point->x = x;
    point->y = y;
}

BOOL surface_get_cursor_pos(POINT* point) {
    BOOL result = GetCursorPos(point);
    if (result != 0 && map_active()) {
        ScreenToClient(surface_hwnd, point);
        map_client_to_game(point);
    }
    return result;
}

BOOL surface_set_cursor_pos(int x, int y) {
    POINT point;
    point.x = x;
    point.y = y;
    if (map_active()) {
        map_game_to_client(&point, 1);
        ClientToScreen(surface_hwnd, &point);
    }
    return SetCursorPos(point.x, point.y);
}

BOOL surface_client_to_screen(HWND wnd, POINT* point) {
    if (map_active() && wnd == surface_hwnd)
        return TRUE;
    return ClientToScreen(wnd, point);
}

BOOL surface_screen_to_client(HWND wnd, POINT* point) {
    if (map_active() && wnd == surface_hwnd)
        return TRUE;
    return ScreenToClient(wnd, point);
}

BOOL surface_get_window_rect(HWND wnd, RECT* rect) {
    if (map_active() && wnd == surface_hwnd) {
        rect->left = 0;
        rect->top = 0;
        rect->right = present_width;
        rect->bottom = present_height;
        return TRUE;
    }
    return GetWindowRect(wnd, rect);
}

BOOL surface_move_child_window(HWND child, int x, int y, int width, int height, BOOL repaint) {
    if (map_active()) {
        POINT top_left;
        POINT bottom_right;
        top_left.x = x;
        top_left.y = y;
        bottom_right.x = x + width;
        bottom_right.y = y + height;
        map_game_to_client(&top_left, 0);
        map_game_to_client(&bottom_right, 0);
        x = top_left.x;
        y = top_left.y;
        width = bottom_right.x - top_left.x;
        height = bottom_right.y - top_left.y;
    }
    return MoveWindow(child, x, y, width, height, repaint);
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

