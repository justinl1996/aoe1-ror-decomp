// Presenting the primary surface, the window layout, and coordinates
// (workflow/docs/sdl_port_tasks.md). The window itself and its message loop
// are in window.cpp.

#include "../../include/surface.h"
#include "sdl_internal.h"

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
static int present_exclusive = 0;
static long present_width = 0;
static long present_height = 0;
static Uint32 present_lut[256];
static int present_pending = 0;
static Uint32 present_last_ticks = 0;

// In-game mouse-wheel viewport zoom (zooms only the map viewport; HUD stays 1:1).
static int zoom_enabled = 0;
static int zoom_suspended = 0;
static long zoom_view_x = 0;
static long zoom_view_y = 0;
static long zoom_view_w = 0;
static long zoom_view_h = 0;
static int zoom_level = 0;
static const float zoom_levels[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
static const int ZOOM_LEVEL_COUNT = 5;

static int zoom_is_active(void) {
    return zoom_enabled != 0 && zoom_suspended == 0 && zoom_level > 0 && zoom_view_w > 0 && zoom_view_h > 0;
}

static void zoom_src_rect(SDL_Rect* out) {
    float z = zoom_levels[zoom_level];
    int sw = (int) (zoom_view_w / z + 0.5f);
    int sh = (int) (zoom_view_h / z + 0.5f);
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    out->x = (int) zoom_view_x + ((int) zoom_view_w - sw) / 2;
    out->y = (int) zoom_view_y + ((int) zoom_view_h - sh) / 2;
    out->w = sw;
    out->h = sh;
}

void surface_set_zoom_viewport(int enabled, long x, long y, long w, long h) {
    zoom_enabled = enabled != 0;
    zoom_view_x = x;
    zoom_view_y = y;
    zoom_view_w = w;
    zoom_view_h = h;
    if (!zoom_enabled) {
        zoom_level = 0;
        zoom_suspended = 0;
    }
}

void surface_set_zoom_suspended(int suspended) {
    int next = suspended != 0;
    if (zoom_suspended != next) {
        zoom_suspended = next;
        if (zoom_enabled && zoom_level > 0)
            present_repaint();
    }
}

void surface_zoom_step(int delta) {
    if (!zoom_enabled || zoom_suspended || delta == 0)
        return;
    int next = zoom_level + (delta > 0 ? 1 : -1);
    if (next < 0)
        next = 0;
    if (next >= ZOOM_LEVEL_COUNT)
        next = ZOOM_LEVEL_COUNT - 1;
    if (next != zoom_level) {
        zoom_level = next;
        present_repaint();
    }
}

static void layout_apply(void);

void present_destroy_target(void) {
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
    present_exclusive = exclusive;
    if (sdl_window == 0 || wnd != sdl_hwnd)
        return;

    if (present_renderer == 0) {
        // Nearest-neighbour scaling keeps the pixel art sharp. No vsync: a
        // present must never stall the game loop.
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        present_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
        if (present_renderer == 0)
            present_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
        if (present_renderer == 0)
            return;
    }
    // Outside exclusive mode the primary surface is the desktop; the texture
    // holds only the window's client area.
    if (exclusive == 0) {
        int client_width;
        int client_height;
        SDL_GetWindowSize(sdl_window, &client_width, &client_height);
        width = client_width;
        height = client_height;
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
    if (zoom_is_active()) {
        if (zoom_view_y > 0) {
            SDL_Rect top = { 0, 0, (int) present_width, (int) zoom_view_y };
            SDL_RenderCopy(present_renderer, present_texture, &top, &top);
        }
        SDL_Rect src;
        zoom_src_rect(&src);
        SDL_Rect dst = { (int) zoom_view_x, (int) zoom_view_y, (int) zoom_view_w, (int) zoom_view_h };
        SDL_RenderCopy(present_renderer, present_texture, &src, &dst);
        int bot_y = (int) (zoom_view_y + zoom_view_h);
        if (bot_y < present_height) {
            SDL_Rect bot = { 0, bot_y, (int) present_width, (int) (present_height - bot_y) };
            SDL_RenderCopy(present_renderer, present_texture, &bot, &bot);
        }
    } else {
        SDL_RenderCopy(present_renderer, present_texture, 0, 0);
    }
    SDL_RenderPresent(present_renderer);
    present_pending = 0;
    present_last_ticks = SDL_GetTicks();
}

void surface_present_primary(const unsigned char* bits, long pitch, long width, long height, const RECT* rect) {
    if (present_texture == 0)
        return;

    // Outside exclusive mode the primary surface is the desktop; only the
    // part under the window's client area is shown.
    RECT r = *rect;
    POINT origin;
    origin.x = 0;
    origin.y = 0;
    if (present_exclusive == 0) {
        int x;
        int y;
        SDL_GetWindowPosition(sdl_window, &x, &y);
        origin.x = x;
        origin.y = y;
    }
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

void present_repaint(void) {
    if (present_renderer != 0 && present_texture != 0)
        present_now();
}

void surface_present_flush(void) {
    if (present_pending != 0 && present_renderer != 0)
        present_now();
}

// ---------------------------------------------------------------------------
// Window layout and coordinates.
//
// Fullscreen is SDL's desktop fullscreen; windowed is an ordinary resizable
// window, sized to the largest whole multiple of the game resolution that
// fits the usable display area. Either way SDL scales the frame to the
// window with letterboxing (SDL_RenderSetLogicalSize), and the game works in
// game pixels: window.cpp translates mouse positions, and the surface_*
// coordinate functions below stand in for the game's direct cursor and
// window-rectangle calls.

#ifdef __EMSCRIPTEN__
// In a browser, fullscreen needs a click or key first; start in the page.
static int layout_windowed = 1;
#else
static int layout_windowed = 0;
#endif

void surface_set_windowed(int windowed) {
    layout_windowed = windowed != 0;
}

int layout_is_windowed(void) {
    return layout_windowed;
}

static void layout_apply(void) {
    if (sdl_window == 0 || present_width <= 0 || present_height <= 0)
        return;

    if (layout_windowed == 0) {
        // Cover the display before asking for fullscreen. A window manager
        // makes the window fullscreen; without one (bare X11, kiosk setups)
        // SDL only believes it is, and a window left at the size and
        // position it had for the game's size (1280x1024, say) would show
        // part of a frame drawn for the whole display. SDL ignores moves of
        // a window it already holds fullscreen, so this happens first.
        if ((SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_FULLSCREEN) == 0) {
            int display = SDL_GetWindowDisplayIndex(sdl_window);
            SDL_Rect bounds;
            if (SDL_GetDisplayBounds(display < 0 ? 0 : display, &bounds) == 0) {
                SDL_SetWindowBordered(sdl_window, SDL_FALSE);
                SDL_SetWindowPosition(sdl_window, bounds.x, bounds.y);
                SDL_SetWindowSize(sdl_window, bounds.w, bounds.h);
            }
        }
        SDL_SetWindowFullscreen(sdl_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(sdl_window, 0);
        SDL_SetWindowBordered(sdl_window, SDL_TRUE);
        SDL_SetWindowResizable(sdl_window, SDL_TRUE);
        int display = SDL_GetWindowDisplayIndex(sdl_window);
        if (display < 0)
            display = 0;
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(display, &usable) != 0) {
            usable.w = present_width;
            usable.h = present_height;
        }
        int top = 0;
        int left = 0;
        int bottom = 0;
        int right = 0;
        SDL_GetWindowBordersSize(sdl_window, &top, &left, &bottom, &right);
        long scale = 1;
        while (present_width * (scale + 1) + left + right <= usable.w
               && present_height * (scale + 1) + top + bottom <= usable.h)
            ++scale;
        SDL_SetWindowSize(sdl_window, present_width * scale, present_height * scale);
        SDL_SetWindowPosition(sdl_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    }
    window_invalidate_all();
}

void layout_toggle(void) {
    layout_windowed = !layout_windowed;
    layout_apply();
}

int map_active(void) {
    return present_renderer != 0 && present_width > 0 && present_height > 0;
}

long map_game_width(void) {
    return present_width;
}

long map_game_height(void) {
    return present_height;
}

void map_zoom_to_game(long* x, long* y) {
    long gx = *x;
    long gy = *y;
    if (zoom_is_active() && gx >= zoom_view_x && gx < zoom_view_x + zoom_view_w
        && gy >= zoom_view_y && gy < zoom_view_y + zoom_view_h) {
        // Preserve screen-edge pixels so edge-scrolling still works while zoomed in.
        int on_left_edge = gx < 4;
        int on_right_edge = gx >= present_width - 4;
        SDL_Rect src;
        zoom_src_rect(&src);
        if (!on_left_edge && !on_right_edge)
            gx = src.x + (long) ((gx - zoom_view_x) * src.w / zoom_view_w);
        gy = src.y + (long) ((gy - zoom_view_y) * src.h / zoom_view_h);
        *x = gx;
        *y = gy;
    }
}

// Client pixel to game pixel, clamped to the game area (the letterbox bars
// count as its nearest edge, so edge scrolling still works there).
void map_client_to_game(POINT* point) {
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
    map_zoom_to_game(&gx, &gy);
    point->x = gx;
    point->y = gy;
}

// Game coordinate to client pixel. center: map a pixel's center (for the
// cursor) rather than its top-left corner (for rectangle edges).
void map_game_to_client(POINT* point, int center) {
    int x;
    int y;
    float px = (float) point->x;
    float py = (float) point->y;
    if (zoom_is_active() && point->x >= zoom_view_x && point->x < zoom_view_x + zoom_view_w
        && point->y >= zoom_view_y && point->y < zoom_view_y + zoom_view_h) {
        SDL_Rect src;
        zoom_src_rect(&src);
        px = (float) zoom_view_x + ((px - (float) src.x) * (float) zoom_view_w) / (float) src.w;
        py = (float) zoom_view_y + ((py - (float) src.y) * (float) zoom_view_h) / (float) src.h;
    }
    float offset = center != 0 ? 0.5f : 0.0f;
    SDL_RenderLogicalToWindow(present_renderer, px + offset, py + offset, &x, &y);
    point->x = x;
    point->y = y;
}

BOOL surface_get_cursor_pos(POINT* point) {
    if (sdl_window == 0)
#ifdef _WIN32
        return GetCursorPos(point);
#else
        return FALSE;
#endif
#ifdef __EMSCRIPTEN__
    // A page has no screen position to subtract: the pointer's position in
    // the canvas.
    int x;
    int y;
    SDL_GetMouseState(&x, &y);
    point->x = x;
    point->y = y;
#else
    int global_x;
    int global_y;
    int window_x;
    int window_y;
    SDL_GetGlobalMouseState(&global_x, &global_y);
    SDL_GetWindowPosition(sdl_window, &window_x, &window_y);
    point->x = global_x - window_x;
    point->y = global_y - window_y;
#endif
    if (map_active())
        map_client_to_game(point);
    return TRUE;
}

BOOL surface_set_cursor_pos(int x, int y) {
    if (sdl_window == 0)
#ifdef _WIN32
        return SetCursorPos(x, y);
#else
        return FALSE;
#endif
    POINT point;
    point.x = x;
    point.y = y;
    if (map_active())
        map_game_to_client(&point, 1);
    SDL_WarpMouseInWindow(sdl_window, point.x, point.y);
    return TRUE;
}

BOOL surface_client_to_screen(HWND wnd, POINT* point) {
    if (map_active() && wnd == sdl_hwnd)
        return TRUE;
#ifdef _WIN32
    return ClientToScreen(wnd, point);
#else
    return FALSE;
#endif
}

BOOL surface_screen_to_client(HWND wnd, POINT* point) {
    if (map_active() && wnd == sdl_hwnd)
        return TRUE;
#ifdef _WIN32
    return ScreenToClient(wnd, point);
#else
    return FALSE;
#endif
}

BOOL surface_get_window_rect(HWND wnd, RECT* rect) {
    if (map_active() && wnd == sdl_hwnd) {
        rect->left = 0;
        rect->top = 0;
        rect->right = present_width;
        rect->bottom = present_height;
        return TRUE;
    }
#ifdef _WIN32
    return GetWindowRect(wnd, rect);
#else
    return FALSE;
#endif
}
