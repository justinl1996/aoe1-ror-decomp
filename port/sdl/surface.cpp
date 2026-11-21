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
// and swallows WM_ERASEBKGND. This procedure sits on top of SDL's and sends
// those two straight to the game. SDL only turns them into events, which
// surface_drop_events discards anyway.
static LRESULT CALLBACK surface_wnd_proc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_PAINT || msg == WM_ERASEBKGND)
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

void surface_detach_window(void) {
    if (surface_window != 0) {
        // SDL restores the game's procedure only if its own is on top.
        SetWindowLongA(surface_hwnd, GWL_WNDPROC, (LONG) surface_sdl_proc);
        SDL_DestroyWindow(surface_window);
        surface_window = 0;
    }
    SDL_Quit();
}

#endif // AOE_SDL
