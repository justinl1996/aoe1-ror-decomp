#ifndef SDL_INTERNAL_H
#define SDL_INTERNAL_H

// Shared between the files in port/sdl/; not for the game's code.

#include <windows.h>

// The VC6 C runtime lacks _aligned_malloc, which clang's intrinsic headers
// use. Nothing here needs SDL's intrinsics.
#define SDL_DISABLE_IMMINTRIN_H
#define SDL_DISABLE_MMINTRIN_H
#define SDL_DISABLE_XMMINTRIN_H
#define SDL_DISABLE_EMMINTRIN_H
#define SDL_DISABLE_PMMINTRIN_H
#include <SDL.h>

// window.cpp: the game window SDL owns, and the handle the game knows it by.
extern SDL_Window* sdl_window;
extern HWND sdl_hwnd;
// Mark the whole game window as needing a WM_PAINT.
void window_invalidate_all(void);

// surface.cpp: presenting, layout and coordinate mapping.
void present_repaint(void);
void present_destroy_target(void);
int map_active(void);
void map_client_to_game(POINT* point);
void map_game_to_client(POINT* point, int center);
long map_game_width(void);
long map_game_height(void);
void layout_toggle(void);
int layout_is_windowed(void);

#endif // SDL_INTERNAL_H
