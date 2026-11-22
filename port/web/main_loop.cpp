// The browser build's main loop (workflow/docs/emscripten_port.md, Task 3).
//
// A browser page cannot loop forever: it runs a function once per frame
// and returns. RGE_Base_Game::run() is therefore driven here one pass at a
// time (RGE_Base_Game::run_step): each frame handles the waiting messages
// and does the game's idle work once, within a time budget, then returns to
// the browser.

#include "../../include/basegame.h"

#include <emscripten.h>

static RGE_Base_Game* web_game = 0;

enum { FRAME_BUDGET_MS = 25 };

static void web_frame(void) {
    if (web_game == 0)
        return;
    double start = emscripten_get_now();
    for (;;) {
        int exit_code = 0;
        int step = web_game->run_step(&exit_code);
        if (step == RGE_Base_Game::RunStepQuit) {
            emscripten_cancel_main_loop();
            RGE_Base_Game* game = web_game;
            web_game = 0;
            delete game;
            EM_ASM({
                if (Module.onGameExit)
                    Module.onGameExit($0);
            }, exit_code);
            return;
        }
        if (step == RGE_Base_Game::RunStepIdle || emscripten_get_now() - start > FRAME_BUDGET_MS)
            return;
    }
}

void web_run_game(RGE_Base_Game* game) {
    web_game = game;
    // 0: the browser's frame rate; 1: never return (unwinds WinMain).
    emscripten_set_main_loop(web_frame, 0, 1);
}
