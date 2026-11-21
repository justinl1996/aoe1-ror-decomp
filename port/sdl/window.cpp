// The game window and its message loop, owned by SDL
// (workflow/docs/sdl_port_tasks.md, "SDL owns the window and the event loop").
//
// SDL creates the game window. The game's window procedure is unchanged, but
// it is fed from one message queue here instead of from the operating system:
//
// - SDL input and window events become the messages Windows would have sent
//   (WM_KEYDOWN with virtual-key codes, WM_CHAR, mouse messages in game
//   pixels with MK_* flags, double-clicks, activation, WM_SIZE, WM_CLOSE);
// - messages the game posts to its window, from any thread, and timers;
// - WM_PAINT, generated from an update region kept here, as Windows does.
//
// RGE_Base_Game::run takes messages from surface_peek_message and
// surface_get_message and hands them to surface_dispatch_message. The game's
// direct calls on its main window (posting and sending messages, timers,
// invalidation, cursor, capture, key state) go through the surface_* functions
// below; for any other window they fall through to Windows.
//
// On Windows the window is still a real window, and Windows components send
// it messages SDL knows nothing about: notifications and colour requests
// from the game's EDIT boxes, MCI notifications, palette messages. A
// procedure on top of SDL's passes those straight to the game ("the Windows
// bridge" below). Everything else goes to SDL, which turns it into events.

#include "../../include/surface.h"
#include "sdl_internal.h"

#include <SDL_syswm.h>
#include <string.h>

SDL_Window* sdl_window = 0;
HWND sdl_hwnd = 0;

static WNDPROC window_game_proc = 0;  // the game's window procedure
static WNDPROC window_sdl_proc = 0;   // SDL's, under the Windows bridge
static Uint32 window_wake_event = 0;  // wakes surface_get_message from other threads
static int window_app_active = 0;
static int window_quit = 0;
static int window_quit_code = 0;

// ---------------------------------------------------------------------------
// Message queues, in Windows' order of retrieval: posted messages, then
// input, then WM_QUIT, then WM_PAINT, then WM_TIMER.

enum { QUEUE_SIZE = 1024, MAX_TIMERS = 8 };

struct MessageQueue {
    MSG items[QUEUE_SIZE];
    int head;
    int count;
};

static MessageQueue queue_posted;  // guarded by queue_lock: any thread posts
static MessageQueue queue_input;   // main thread only
static SDL_mutex* queue_lock = 0;

static int queue_push(MessageQueue* queue, HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (queue->count == QUEUE_SIZE)
        return 0;
    MSG* item = &queue->items[(queue->head + queue->count) % QUEUE_SIZE];
    memset(item, 0, sizeof(*item));
    item->hwnd = wnd;
    item->message = msg;
    item->wParam = wparam;
    item->lParam = lparam;
    item->time = SDL_GetTicks();
    ++queue->count;
    return 1;
}

static int queue_pop(MessageQueue* queue, MSG* out) {
    if (queue->count == 0)
        return 0;
    *out = queue->items[queue->head];
    queue->head = (queue->head + 1) % QUEUE_SIZE;
    --queue->count;
    return 1;
}

static void input_post(UINT msg, WPARAM wparam, LPARAM lparam) {
    queue_push(&queue_input, sdl_hwnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Timers. Like Windows timers they never pile up: at most one WM_TIMER per
// timer is pending.

struct WindowTimer {
    UINT id;
    SDL_TimerID sdl_id;
    volatile int due;
};

static WindowTimer window_timers[MAX_TIMERS];

static void wake_main_thread(void) {
    if (window_wake_event != (Uint32) -1 && window_wake_event != 0) {
        SDL_Event event;
        memset(&event, 0, sizeof(event));
        event.type = window_wake_event;
        SDL_PushEvent(&event);
    }
}

static Uint32 SDLCALL timer_callback(Uint32 interval, void* param) {
    WindowTimer* timer = (WindowTimer*) param;
    timer->due = 1;
    wake_main_thread();
    return interval;
}

UINT surface_set_timer(HWND wnd, UINT id, UINT elapse) {
    if (wnd != sdl_hwnd || sdl_hwnd == 0)
        return SetTimer(wnd, id, elapse, 0);
    for (int i = 0; i < MAX_TIMERS; ++i) {
        if (window_timers[i].sdl_id != 0 && window_timers[i].id == id) {
            SDL_RemoveTimer(window_timers[i].sdl_id);
            window_timers[i].sdl_id = 0;
        }
    }
    for (int i = 0; i < MAX_TIMERS; ++i) {
        if (window_timers[i].sdl_id == 0) {
            window_timers[i].id = id;
            window_timers[i].due = 0;
            window_timers[i].sdl_id = SDL_AddTimer(elapse > 0 ? elapse : 1, timer_callback, &window_timers[i]);
            return window_timers[i].sdl_id != 0 ? id : 0;
        }
    }
    return 0;
}

BOOL surface_kill_timer(HWND wnd, UINT id) {
    if (wnd != sdl_hwnd || sdl_hwnd == 0)
        return KillTimer(wnd, id);
    for (int i = 0; i < MAX_TIMERS; ++i) {
        if (window_timers[i].sdl_id != 0 && window_timers[i].id == id) {
            SDL_RemoveTimer(window_timers[i].sdl_id);
            window_timers[i].sdl_id = 0;
            window_timers[i].due = 0;
            return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// The update region. One rectangle, as a bounding box; validating part of it
// shrinks it only when what is left is still a rectangle, so it may ask for
// a little more painting than Windows would, never less.

static int paint_invalid = 0;
static RECT paint_rect;

static void paint_whole(RECT* rect) {
    rect->left = 0;
    rect->top = 0;
    rect->right = map_game_width() > 0 ? map_game_width() : 1;
    rect->bottom = map_game_height() > 0 ? map_game_height() : 1;
}

void window_invalidate_all(void) {
    RECT whole;
    paint_whole(&whole);
    surface_invalidate_rect(sdl_hwnd, &whole, FALSE);
}

BOOL surface_invalidate_rect(HWND wnd, const RECT* rect, BOOL erase) {
    if (wnd != sdl_hwnd || sdl_hwnd == 0)
        return InvalidateRect(wnd, rect, erase);
    RECT add;
    if (rect != 0)
        add = *rect;
    else
        paint_whole(&add);
    if (add.left >= add.right || add.top >= add.bottom)
        return TRUE;
    if (paint_invalid == 0) {
        paint_rect = add;
        paint_invalid = 1;
    } else {
        if (add.left < paint_rect.left)
            paint_rect.left = add.left;
        if (add.top < paint_rect.top)
            paint_rect.top = add.top;
        if (add.right > paint_rect.right)
            paint_rect.right = add.right;
        if (add.bottom > paint_rect.bottom)
            paint_rect.bottom = add.bottom;
    }
    return TRUE;
}

BOOL surface_validate_rect(HWND wnd, const RECT* rect) {
    if (wnd != sdl_hwnd || sdl_hwnd == 0)
        return ValidateRect(wnd, rect);
    if (paint_invalid == 0)
        return TRUE;
    if (rect == 0) {
        paint_invalid = 0;
        return TRUE;
    }
    RECT r = *rect;
    int covers_x = r.left <= paint_rect.left && r.right >= paint_rect.right;
    int covers_y = r.top <= paint_rect.top && r.bottom >= paint_rect.bottom;
    if (covers_x && covers_y) {
        paint_invalid = 0;
    } else if (covers_x && r.top <= paint_rect.top && r.bottom > paint_rect.top) {
        paint_rect.top = r.bottom;
    } else if (covers_x && r.bottom >= paint_rect.bottom && r.top < paint_rect.bottom) {
        paint_rect.bottom = r.top;
    } else if (covers_y && r.left <= paint_rect.left && r.right > paint_rect.left) {
        paint_rect.left = r.right;
    } else if (covers_y && r.right >= paint_rect.right && r.left < paint_rect.right) {
        paint_rect.right = r.left;
    }
    if (paint_rect.left >= paint_rect.right || paint_rect.top >= paint_rect.bottom)
        paint_invalid = 0;
    return TRUE;
}

BOOL surface_get_update_rect(HWND wnd, RECT* rect, BOOL erase) {
    if (wnd != sdl_hwnd || sdl_hwnd == 0)
        return GetUpdateRect(wnd, rect, erase);
    if (paint_invalid == 0) {
        if (rect != 0)
            SetRectEmpty(rect);
        return FALSE;
    }
    if (rect != 0)
        *rect = paint_rect;
    return TRUE;
}

BOOL surface_update_window(HWND wnd) {
    if (wnd != sdl_hwnd || sdl_hwnd == 0)
        return UpdateWindow(wnd);
    if (paint_invalid != 0 && window_game_proc != 0)
        CallWindowProcA((FARPROC) window_game_proc, sdl_hwnd, WM_PAINT, 0, 0);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Keyboard and mouse state, as GetKeyState sees it: kept in step with the
// key messages the game receives.

static unsigned char key_state[256];  // 0x80 down, 0x01 toggled

static int vk_from_key(const SDL_Keysym* key, int* extended) {
    SDL_Keycode code = key->sym;
    *extended = 0;
    if (code >= SDLK_a && code <= SDLK_z)
        return 'A' + (code - SDLK_a);
    if (code >= SDLK_0 && code <= SDLK_9)
        return '0' + (code - SDLK_0);
    if (code >= SDLK_F1 && code <= SDLK_F12)
        return VK_F1 + (code - SDLK_F1);
    if (code >= SDLK_KP_1 && code <= SDLK_KP_9)
        return VK_NUMPAD1 + (code - SDLK_KP_1);
    switch (code) {
        case SDLK_KP_0: return VK_NUMPAD0;
        case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
        case SDLK_KP_PLUS: return VK_ADD;
        case SDLK_KP_MINUS: return VK_SUBTRACT;
        case SDLK_KP_PERIOD: return VK_DECIMAL;
        case SDLK_KP_DIVIDE: *extended = 1; return VK_DIVIDE;
        case SDLK_KP_ENTER: *extended = 1; return VK_RETURN;
        case SDLK_RETURN: return VK_RETURN;
        case SDLK_ESCAPE: return VK_ESCAPE;
        case SDLK_BACKSPACE: return VK_BACK;
        case SDLK_TAB: return VK_TAB;
        case SDLK_SPACE: return VK_SPACE;
        case SDLK_PAUSE: return VK_PAUSE;
        case SDLK_CAPSLOCK: return VK_CAPITAL;
        case SDLK_NUMLOCKCLEAR: *extended = 1; return VK_NUMLOCK;
        case SDLK_SCROLLLOCK: return VK_SCROLL;
        case SDLK_PRINTSCREEN: *extended = 1; return VK_SNAPSHOT;
        case SDLK_PAGEUP: *extended = 1; return VK_PRIOR;
        case SDLK_PAGEDOWN: *extended = 1; return VK_NEXT;
        case SDLK_END: *extended = 1; return VK_END;
        case SDLK_HOME: *extended = 1; return VK_HOME;
        case SDLK_LEFT: *extended = 1; return VK_LEFT;
        case SDLK_UP: *extended = 1; return VK_UP;
        case SDLK_RIGHT: *extended = 1; return VK_RIGHT;
        case SDLK_DOWN: *extended = 1; return VK_DOWN;
        case SDLK_INSERT: *extended = 1; return VK_INSERT;
        case SDLK_DELETE: *extended = 1; return VK_DELETE;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return VK_SHIFT;
        case SDLK_LCTRL: return VK_CONTROL;
        case SDLK_RCTRL: *extended = 1; return VK_CONTROL;
        case SDLK_LALT: return VK_MENU;
        case SDLK_RALT: *extended = 1; return VK_MENU;
        case SDLK_LGUI: *extended = 1; return 0x5B;  // VK_LWIN
        case SDLK_RGUI: *extended = 1; return 0x5C;  // VK_RWIN
        case SDLK_APPLICATION: *extended = 1; return 0x5D;  // VK_APPS
        // US-layout punctuation, as Windows' VK_OEM_* codes.
        case SDLK_SEMICOLON: return 0xBA;
        case SDLK_EQUALS: return 0xBB;
        case SDLK_COMMA: return 0xBC;
        case SDLK_MINUS: return 0xBD;
        case SDLK_PERIOD: return 0xBE;
        case SDLK_SLASH: return 0xBF;
        case SDLK_BACKQUOTE: return 0xC0;
        case SDLK_LEFTBRACKET: return 0xDB;
        case SDLK_BACKSLASH: return 0xDC;
        case SDLK_RIGHTBRACKET: return 0xDD;
        case SDLK_QUOTE: return 0xDE;
    }
    return 0;
}

// The left/right variants Windows also tracks for the generic modifiers.
static int vk_side(SDL_Keycode code) {
    switch (code) {
        case SDLK_LSHIFT: return VK_LSHIFT;
        case SDLK_RSHIFT: return VK_RSHIFT;
        case SDLK_LCTRL: return VK_LCONTROL;
        case SDLK_RCTRL: return VK_RCONTROL;
        case SDLK_LALT: return VK_LMENU;
        case SDLK_RALT: return VK_RMENU;
    }
    return 0;
}

static void key_state_set(int vk, int side, int down) {
    if (down != 0) {
        if ((key_state[vk] & 0x80) == 0)
            key_state[vk] ^= 0x01;
        key_state[vk] |= 0x80;
        if (side != 0)
            key_state[side] |= 0x80;
    } else {
        if (side != 0)
            key_state[side] &= ~0x80;
        // A generic modifier stays down while either side is.
        if (vk == VK_SHIFT && ((key_state[VK_LSHIFT] | key_state[VK_RSHIFT]) & 0x80) != 0)
            return;
        if (vk == VK_CONTROL && ((key_state[VK_LCONTROL] | key_state[VK_RCONTROL]) & 0x80) != 0)
            return;
        if (vk == VK_MENU && ((key_state[VK_LMENU] | key_state[VK_RMENU]) & 0x80) != 0)
            return;
        key_state[vk] &= ~0x80;
    }
}

static unsigned char mouse_button_state(int vk) {
    Uint32 buttons = SDL_GetMouseState(0, 0);
    if (vk == VK_LBUTTON)
        return (buttons & SDL_BUTTON_LMASK) != 0 ? 0x80 : 0;
    if (vk == VK_RBUTTON)
        return (buttons & SDL_BUTTON_RMASK) != 0 ? 0x80 : 0;
    if (vk == VK_MBUTTON)
        return (buttons & SDL_BUTTON_MMASK) != 0 ? 0x80 : 0;
    return 0;
}

SHORT surface_get_key_state(int vk) {
    if (sdl_window == 0)
        return GetKeyState(vk);
    vk &= 0xFF;
    unsigned char state = key_state[vk] | mouse_button_state(vk);
    return (SHORT) (((state & 0x80) != 0 ? 0x8000 : 0) | (state & 0x01));
}

SHORT surface_get_async_key_state(int vk) {
    if (sdl_window == 0)
        return GetAsyncKeyState(vk);
    vk &= 0xFF;
    unsigned char state = key_state[vk] | mouse_button_state(vk);
    return (SHORT) ((state & 0x80) != 0 ? 0x8000 : 0);
}

BOOL surface_get_keyboard_state(BYTE* state) {
    if (sdl_window == 0)
        return GetKeyboardState(state);
    memcpy(state, key_state, sizeof(key_state));
    state[VK_LBUTTON] |= mouse_button_state(VK_LBUTTON);
    state[VK_RBUTTON] |= mouse_button_state(VK_RBUTTON);
    state[VK_MBUTTON] |= mouse_button_state(VK_MBUTTON);
    return TRUE;
}

static WPARAM mouse_key_flags(void) {
    Uint32 buttons = SDL_GetMouseState(0, 0);
    SDL_Keymod mods = SDL_GetModState();
    WPARAM flags = 0;
    if ((buttons & SDL_BUTTON_LMASK) != 0)
        flags |= MK_LBUTTON;
    if ((buttons & SDL_BUTTON_RMASK) != 0)
        flags |= MK_RBUTTON;
    if ((buttons & SDL_BUTTON_MMASK) != 0)
        flags |= MK_MBUTTON;
    if ((mods & KMOD_SHIFT) != 0)
        flags |= MK_SHIFT;
    if ((mods & KMOD_CTRL) != 0)
        flags |= MK_CONTROL;
    return flags;
}

// SDL already reports mouse events in the renderer's logical coordinates
// (game pixels) once SDL_RenderSetLogicalSize is set; they only need
// clamping to the game area, so the letterbox bars count as its edges.
static LPARAM mouse_point(int x, int y) {
    if (map_active()) {
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x > map_game_width() - 1)
            x = map_game_width() - 1;
        if (y > map_game_height() - 1)
            y = map_game_height() - 1;
    }
    return MAKELPARAM(x, y);
}

// ---------------------------------------------------------------------------
// SDL events to window messages

static void translate_key(const SDL_KeyboardEvent* key, int down) {
    int extended;
    int vk = vk_from_key(&key->keysym, &extended);
    SDL_Keymod mods = (SDL_Keymod) key->keysym.mod;

    // Alt+Enter switches between fullscreen and a window. The game never
    // sees it.
    if (vk == VK_RETURN && (mods & KMOD_ALT) != 0) {
        if (down != 0 && key->repeat == 0)
            layout_toggle();
        return;
    }
    if (vk == 0)
        return;

    int previous = down != 0 ? key->repeat != 0 : 1;
    key_state_set(vk, vk_side(key->keysym.sym), down);
    int alt = (mods & KMOD_ALT) != 0 || vk == VK_MENU;
    LPARAM lparam = 1 | (extended != 0 ? 1 << 24 : 0) | (alt != 0 ? 1 << 29 : 0) | (previous != 0 ? 1 << 30 : 0)
        | (down == 0 ? (LPARAM) 1 << 31 : 0);
    // Windows reports Alt combinations and F10 as system keys.
    UINT msg;
    if (alt != 0 || vk == VK_F10)
        msg = down != 0 ? WM_SYSKEYDOWN : WM_SYSKEYUP;
    else
        msg = down != 0 ? WM_KEYDOWN : WM_KEYUP;
    input_post(msg, vk, lparam);

    // Characters SDL's text input leaves out, as TranslateMessage makes them.
    if (down != 0 && alt == 0) {
        int ch = 0;
        if (vk == VK_RETURN)
            ch = '\r';
        else if (vk == VK_BACK)
            ch = '\b';
        else if (vk == VK_TAB)
            ch = '\t';
        else if (vk == VK_ESCAPE)
            ch = 0x1B;
        else if ((mods & KMOD_CTRL) != 0 && vk >= 'A' && vk <= 'Z')
            ch = vk - 'A' + 1;
        if (ch != 0)
            input_post(WM_CHAR, ch, lparam & 0x01FFFFFF);
    }
}

// SDL text input is UTF-8; the game reads single-byte (Windows-1252) WM_CHARs.
static void translate_text(const char* text) {
    const unsigned char* p = (const unsigned char*) text;
    while (*p != 0) {
        unsigned long code;
        int extra;
        if (*p < 0x80) {
            code = *p;
            extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            code = *p & 0x1F;
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            code = *p & 0x0F;
            extra = 2;
        } else {
            code = *p & 0x07;
            extra = 3;
        }
        ++p;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i, ++p)
            code = (code << 6) | (*p & 0x3F);
        if (code >= 0x20 && code < 0x100 && code != 0x7F)
            input_post(WM_CHAR, code, 1);
    }
}

static void activate(int active) {
    if (active == window_app_active)
        return;
    window_app_active = active;
    if (active != 0) {
        input_post(WM_ACTIVATEAPP, TRUE, 0);
        input_post(WM_ACTIVATE, WA_ACTIVE, 0);
    } else {
        memset(key_state, 0, sizeof(key_state));
        input_post(WM_ACTIVATE, WA_INACTIVE, 0);
        input_post(WM_ACTIVATEAPP, FALSE, 0);
    }
}

static void translate_event(const SDL_Event* event) {
    switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            translate_key(&event->key, event->type == SDL_KEYDOWN);
            break;
        case SDL_TEXTINPUT:
            translate_text(event->text.text);
            break;
        case SDL_MOUSEMOTION:
            input_post(WM_MOUSEMOVE, mouse_key_flags(), mouse_point(event->motion.x, event->motion.y));
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int down = event->type == SDL_MOUSEBUTTONDOWN;
            int twice = down != 0 && event->button.clicks >= 2;
            UINT msg;
            if (event->button.button == SDL_BUTTON_LEFT)
                msg = twice ? WM_LBUTTONDBLCLK : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
            else if (event->button.button == SDL_BUTTON_RIGHT)
                msg = twice ? WM_RBUTTONDBLCLK : (down ? WM_RBUTTONDOWN : WM_RBUTTONUP);
            else if (event->button.button == SDL_BUTTON_MIDDLE)
                msg = twice ? WM_MBUTTONDBLCLK : (down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
            else
                break;
            input_post(msg, mouse_key_flags(), mouse_point(event->button.x, event->button.y));
            break;
        }
        case SDL_WINDOWEVENT:
            switch (event->window.event) {
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    activate(1);
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    // On Windows a focused EDIT box takes the keyboard focus
                    // from SDL's window, but the game is still the active
                    // application.
                    if (GetForegroundWindow() != sdl_hwnd)
                        activate(0);
                    break;
                case SDL_WINDOWEVENT_EXPOSED:
                    window_invalidate_all();
                    present_repaint();
                    break;
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    window_invalidate_all();
                    input_post(WM_SIZE, SIZE_RESTORED, MAKELPARAM(map_game_width(), map_game_height()));
                    break;
                case SDL_WINDOWEVENT_MINIMIZED:
                    input_post(WM_SIZE, SIZE_MINIMIZED, 0);
                    break;
                case SDL_WINDOWEVENT_RESTORED:
                    window_invalidate_all();
                    input_post(WM_SIZE, SIZE_RESTORED, MAKELPARAM(map_game_width(), map_game_height()));
                    break;
                case SDL_WINDOWEVENT_CLOSE:
                    input_post(WM_CLOSE, 0, 0);
                    break;
            }
            break;
    }
}

static void pump_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0)
        translate_event(&event);
}

// ---------------------------------------------------------------------------
// The message loop

static int take_message(MSG* msg) {
    int got = 0;
    if (queue_lock != 0) {
        SDL_LockMutex(queue_lock);
        got = queue_pop(&queue_posted, msg);
        SDL_UnlockMutex(queue_lock);
    }
    if (got != 0)
        return 1;
    if (queue_pop(&queue_input, msg) != 0)
        return 1;
    memset(msg, 0, sizeof(*msg));
    msg->hwnd = sdl_hwnd;
    msg->time = SDL_GetTicks();
    if (window_quit != 0) {
        msg->hwnd = 0;
        msg->message = WM_QUIT;
        msg->wParam = window_quit_code;
        return 1;
    }
    // WM_PAINT stays until the game validates the window, as on Windows.
    if (paint_invalid != 0) {
        msg->message = WM_PAINT;
        return 1;
    }
    for (int i = 0; i < MAX_TIMERS; ++i) {
        if (window_timers[i].sdl_id != 0 && window_timers[i].due != 0) {
            window_timers[i].due = 0;
            msg->message = WM_TIMER;
            msg->wParam = window_timers[i].id;
            return 1;
        }
    }
    return 0;
}

BOOL surface_peek_message(MSG* msg) {
    pump_events();
    return take_message(msg);
}

BOOL surface_get_message(MSG* msg) {
    for (;;) {
        pump_events();
        if (take_message(msg) != 0)
            return msg->message != WM_QUIT;
        SDL_Event event;
        if (SDL_WaitEvent(&event) != 0)
            translate_event(&event);
    }
}

LRESULT surface_dispatch_message(const MSG* msg) {
    if (msg->hwnd == sdl_hwnd && window_game_proc != 0)
        return CallWindowProcA((FARPROC) window_game_proc, msg->hwnd, msg->message, msg->wParam, msg->lParam);
    if (msg->hwnd != 0)
        return DispatchMessageA(msg);
    return 0;
}

BOOL surface_post_message(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (wnd != sdl_hwnd || sdl_hwnd == 0 || queue_lock == 0)
        return PostMessageA(wnd, msg, wparam, lparam);
    SDL_LockMutex(queue_lock);
    int posted = queue_push(&queue_posted, wnd, msg, wparam, lparam);
    SDL_UnlockMutex(queue_lock);
    wake_main_thread();
    return posted;
}

LRESULT surface_send_message(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (wnd != sdl_hwnd || sdl_hwnd == 0 || window_game_proc == 0)
        return SendMessageA(wnd, msg, wparam, lparam);
    return CallWindowProcA((FARPROC) window_game_proc, wnd, msg, wparam, lparam);
}

void surface_post_quit_message(int exit_code) {
    window_quit = 1;
    window_quit_code = exit_code;
    wake_main_thread();
}

// ---------------------------------------------------------------------------
// Cursor, capture, focus

static SDL_Cursor* cursor_arrow = 0;
static SDL_Cursor* cursor_wait = 0;
static HWND capture_wnd = 0;

// The game hides the system cursor while it draws its own, and shows the
// arrow or the hourglass otherwise.
void surface_set_cursor(HCURSOR cursor) {
    if (sdl_window == 0) {
        SetCursor(cursor);
        return;
    }
    if (cursor == 0) {
        SDL_ShowCursor(SDL_DISABLE);
        return;
    }
    if (cursor_arrow == 0)
        cursor_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    if (cursor_wait == 0)
        cursor_wait = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
    SDL_SetCursor(cursor == LoadCursorA(0, IDC_WAIT) ? cursor_wait : cursor_arrow);
    SDL_ShowCursor(SDL_ENABLE);
}

HWND surface_get_capture(void) {
    if (capture_wnd != 0)
        return capture_wnd;
    return GetCapture();
}

HWND surface_set_capture(HWND wnd) {
    HWND previous = surface_get_capture();
    if (wnd == sdl_hwnd && sdl_hwnd != 0) {
        SDL_CaptureMouse(SDL_TRUE);
        capture_wnd = wnd;
        return previous;
    }
    SetCapture(wnd);
    return previous;
}

BOOL surface_release_capture(void) {
    if (capture_wnd != 0) {
        SDL_CaptureMouse(SDL_FALSE);
        capture_wnd = 0;
    }
    return ReleaseCapture();
}

HWND surface_get_foreground_window(void) {
    if (sdl_window == 0)
        return GetForegroundWindow();
    return window_app_active != 0 ? sdl_hwnd : 0;
}

BOOL surface_is_iconic(HWND wnd) {
    if (wnd != sdl_hwnd || sdl_window == 0)
        return IsIconic(wnd);
    return (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_MINIMIZED) != 0;
}

// ---------------------------------------------------------------------------
// The Windows bridge

static LRESULT CALLBACK bridge_wnd_proc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_PAINT:
            // Windows wants the window painted (it was covered or shown).
            // Show the last frame again, and let the game repaint through
            // its own WM_PAINT.
            ValidateRect(wnd, 0);
            window_invalidate_all();
            present_repaint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_COMMAND:
        case WM_CTLCOLORMSGBOX:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSCROLLBAR:
        case WM_CTLCOLORSTATIC:
        case WM_TIMER:
        case WM_SYSCOMMAND:
        case WM_QUERYNEWPALETTE:
        case WM_PALETTECHANGED:
        case WM_POWERBROADCAST:
        case MM_MCINOTIFY:
            return CallWindowProcA((FARPROC) window_game_proc, wnd, msg, wparam, lparam);
        case WM_DESTROY:
            CallWindowProcA((FARPROC) window_game_proc, wnd, msg, wparam, lparam);
            return CallWindowProcA((FARPROC) window_sdl_proc, wnd, msg, wparam, lparam);
    }
    if (msg >= WM_USER)
        return CallWindowProcA((FARPROC) window_game_proc, wnd, msg, wparam, lparam);
    return CallWindowProcA((FARPROC) window_sdl_proc, wnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Creating and destroying the window

HWND surface_create_main_window(const char* title, int width, int height, WNDPROC game_proc) {
    if (sdl_window != 0)
        return sdl_hwnd;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
        return 0;

    Uint32 flags = SDL_WINDOW_SHOWN;
    if (layout_is_windowed() != 0)
        flags |= SDL_WINDOW_RESIZABLE;
    else
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    sdl_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (sdl_window == 0) {
        SDL_Quit();
        return 0;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(sdl_window, &info) == SDL_FALSE) {
        SDL_DestroyWindow(sdl_window);
        sdl_window = 0;
        SDL_Quit();
        return 0;
    }
    sdl_hwnd = info.info.win.window;

    queue_lock = SDL_CreateMutex();
    window_wake_event = SDL_RegisterEvents(1);
    window_game_proc = game_proc;
    window_sdl_proc = (WNDPROC) SetWindowLongA(sdl_hwnd, GWL_WNDPROC, (LONG) bridge_wnd_proc);

    if ((SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_INPUT_FOCUS) != 0)
        activate(1);
    window_invalidate_all();
    return sdl_hwnd;
}

void surface_destroy_main_window(void) {
    present_destroy_target();
    for (int i = 0; i < MAX_TIMERS; ++i) {
        if (window_timers[i].sdl_id != 0) {
            SDL_RemoveTimer(window_timers[i].sdl_id);
            window_timers[i].sdl_id = 0;
        }
    }
    if (sdl_window != 0) {
        if (IsWindow(sdl_hwnd))
            SetWindowLongA(sdl_hwnd, GWL_WNDPROC, (LONG) window_sdl_proc);
        SDL_DestroyWindow(sdl_window);
        sdl_window = 0;
    }
    sdl_hwnd = 0;
    window_game_proc = 0;
    if (queue_lock != 0) {
        SDL_DestroyMutex(queue_lock);
        queue_lock = 0;
    }
    SDL_Quit();
}
