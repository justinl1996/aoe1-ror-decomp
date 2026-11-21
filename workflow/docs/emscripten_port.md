# Emscripten (browser) port plan

A plan for running the game in a web browser, built with Emscripten from
the same sources as the clang Windows build and the native Linux build.
It is written to be followed task by task by a person (or an agent): each
task names the files and functions involved, what to do, how to check it,
and when it is done.

Like the other substitute builds, the web build is for playing and
porting, never evidence for config/functions/ status.

## Starting point

The web port builds on two finished pieces:

- **The SDL layer** (`port/sdl/`, workflow/docs/sdl_port_tasks.md). Window,
  input, presenting, GDI text (SDL_ttf), DirectDraw and DirectSound in
  memory, the mixer, music (MCI stand-in), settings (`settings.ini`),
  string tables read from `language.dll`. Emscripten has SDL2 and
  SDL2_ttf as built-in ports (`-sUSE_SDL=2 -sUSE_SDL_TTF=2`).
- **The Linux build** (workflow/docs/linux_build.md). Wine's headers for the
  Win32 declarations, `port/linux/` for the Microsoft C runtime's extras
  (Windows paths resolved case-insensitively), the remaining Win32 calls,
  and `main()`. Everything there is POSIX and carries over.

Facts that make the web port smaller than it could be (checked
2026-11-21):

- **WebAssembly in browsers is 32-bit** (`wasm32`): pointers and `long` are
  4 bytes, as in the 32-bit Windows and Linux builds. None of the 64-bit
  work applies; file formats are read as they are.
- **One message loop.** `RGE_Base_Game::run` (src/basegame.cpp) is the
  game's only loop over messages; the panels have no modal loops of their
  own, and no code busy-waits on the clock.
- **Few explicit function-pointer casts** (five `(WNDPROC)`/`(FARPROC)`
  casts), the main source of WebAssembly's signature traps.
- **Wine's headers almost compile for wasm32**: only the CPU-specific
  parts of `winnt.h` fail (`CONTEXT` and `NtCurrentTeb()`).
- **Data**: a retail install is 254 MB, of which 127 MB is the AVI videos,
  which the port does not play. About 127 MB is needed.

## Ground rules

- **No game data is built in or hosted.** The page loads the player's own
  retail install from their disk. Only the Liberation fonts (SIL OFL) are
  packaged, as in the other builds.
- **Scope** as for the Linux port: no multiplayer, no intro videos, no
  WinHelp, English only. MIDI music needs the software synthesizer (see
  "Related work"); until then the web build plays CD tracks ripped to WAV
  or WAV renderings of the MIDI files, if the player has them.
- **Keep the other builds working.** Every source change must still build
  and run with `make clang` (Windows) and `make linux`. Web-only code goes
  under `#ifdef __EMSCRIPTEN__`, in `port/web/` or the port layer.
- **Commit after each task**, with the task's checks recorded in this
  file's Progress section.
- Before running a tool, read its `--help` (AGENTS.md).

## Design decisions

1. **Main loop: a step function, not Asyncify.** `run()` becomes "do one
   iteration" (`run_step()`), called by `emscripten_set_main_loop` once per
   browser frame. The loop is small and single, so this is cheaper and
   faster than Asyncify (which instruments every function that may block).
   Asyncify stays the fallback if a hidden blocking wait turns up.
2. **No threads at first.** Timers (`WM_TIMER`, the WAV stream's refill)
   are polled from the main loop step instead of SDL's timer thread. This
   avoids SharedArrayBuffer, which browsers only allow on pages served
   with cross-origin isolation headers.
3. **File system:** the game folder is `/game` in Emscripten's virtual file
   system, backed by IndexedDB (IDBFS) so the data, settings and saves
   survive reloads. The first visit copies the chosen folder in.
4. **Display:** the canvas fills the page; the game frame is scaled into
   it with the existing letterboxing. "Fullscreen" is the browser's
   fullscreen, entered from a user gesture.

## Tasks

Each task: **Files**, **Steps**, **Check**, **Done when**.

### Task 0: Toolchain and build tool

**Files:** new `config/web.json`, new `tools/build_web.py`, `Makefile`.

**Steps:**

1. Pin an Emscripten SDK version in `config/web.json` (version, where
   `emsdk` is expected or how to install it). Do not vendor the SDK.
2. Write `tools/build_web.py` modelled on `tools/build_linux.py`: the same
   source tree (`build_clang.case_insensitive_view`, `add_include_aliases`,
   `apply_patches`), the same source plan plus `port/linux/*.cpp` and the
   new `port/web/*.cpp`, compiled with `em++`/`emcc`.
3. Flags: `-sUSE_SDL=2 -sUSE_SDL_TTF=2`, `-fdeclspec`, `-DNO_STRICT`,
   `-fwrapv`, `-fno-strict-aliasing`, the forced include
   `port/linux/include/aoe_linux.h`, and `-D AOE_WEB`. Leave out
   `-malign-double` and `-mno-sse` (they are x86 options; wasm32 already
   aligns doubles to 8 as MSVC does).
4. Output to `build/web/<time>/`: `index.html`, `empiresx.js`,
   `empiresx.wasm`, and the fonts as a preloaded package (`--preload-file
   fonts@/fonts`).
5. `make web` target; `--debug` option for `-O0 -g -sASSERTIONS=2
   -sSAFE_HEAP=1 -sSTACK_OVERFLOW_CHECK=2`.

**Check:** the tool runs and reports per-source compile results, as
`build_linux.py` does.

**Done when:** `make web` runs end to end (failures expected until Task 1).

### Task 1: Compile every source for wasm32

**Files:** new `port/web/include/` (header shims), `port/linux/include/aoe_linux.h`,
possibly a few `src/` files.

**Steps:**

1. **Wine's CPU-specific parts.** `winnt.h` stops with "You need to define
   a CONTEXT for your CPU" and "You must define NtCurrentTeb() for your
   architecture". Add a patch for Wine's headers, applied to a copy in the
   build tree (the way `port/clang/*.patch` is applied): an
   `#elif defined(__wasm32__)` branch with a minimal `CONTEXT` struct and an
   `NtCurrentTeb()` that returns 0. Nothing in the game uses either.
2. **C library.** Emscripten uses musl, not glibc. Go through
   `port/linux/include/msvcrt.h` and `port/linux/crt.cpp`: `fnmatch`
   (`FNM_CASEFOLD` is a GNU extension; implement a case-insensitive
   wildcard match), `st_mtim` (present in musl), `sysconf(_SC_PHYS_PAGES)`
   (may be unavailable: report a fixed 1 GB in `GlobalMemoryStatus`).
3. **Win32 stand-ins.** `port/linux/win32.cpp` uses `mmap` (supported on
   Emscripten's file systems, as a copy) and `clock_gettime`; keep them.
4. Compile everything; fix what fails, keeping the Windows and Linux builds
   green. Record each fix and why.

**Check:** all ~243 sources compile; `make clang` and `make linux` still
build.

**Done when:** `build_web.py` compiles every source.

### Task 2: Link, and find signature mismatches

**Files:** `port/web/main.cpp` (new), `port/linux/main.cpp`, any source with a
mismatched function-pointer call.

**Steps:**

1. Link. Undefined symbols should be few (the port layer and
   `port/linux/` already cover the Win32 calls). Stub what remains in
   `port/web/` with the same "not supported" message style as
   `port/linux/win32.cpp`.
2. **Function-pointer signatures.** WebAssembly traps ("function signature
   mismatch") when a function is called through a pointer of another type;
   x86 did not care. Compile with `-Wcast-function-type-strict` and review
   each warning. Known places:
   - window procedures: `CallWindowProcA`'s `FARPROC` overload in
     `aoe_linux.h`, `TRIBE_Game::video_wnd_proc` (video only), the casts
     in `port/sdl/window.cpp` (Windows only);
   - callbacks stored in tables or as `void*` (search `(void*)` casts of
     function names and `FARPROC`/`WNDPROC` casts);
   - `qsort` comparators declared with non-`const void*` parameters.
   Fix each by giving the function the right signature, or by a small
   adapter function, never by casting.
3. For diagnosis only, a build with `-sEMULATE_FUNCTION_POINTER_CASTS=1`
   tells whether a crash is a signature trap.

**Check:** the debug build links; starting it in a browser reaches
`WinMain` (log a line) before anything else fails.

**Done when:** the program links and starts.

### Task 3: The main loop

**Files:** `src/basegame.cpp` (`RGE_Base_Game::run`), `port/sdl/window.cpp`
(`surface_get_message`, `surface_peek_message`), `src/main.cpp` (`WinMain`),
`port/web/main.cpp`.

**Steps:**

1. Split `RGE_Base_Game::run()` into `run_step()`, one iteration of the
   current `while (1)` body, returning whether to go on. `run()` on Windows
   and Linux becomes `while (run_step()) {}`, so those builds behave as
   before.
2. On the web, `run_step()` must not block: where `run()` calls
   `surface_get_message` (the game inactive or not in a game or menu), use
   `surface_peek_message` and return if there is nothing. Within one step,
   handle all queued messages, then `handle_idle()` once, then
   `surface_present_flush()`.
3. `WinMain` (src/main.cpp) calls `run()` and then cleans up. On the web,
   start the loop with `emscripten_set_main_loop(step, 0, 0)` (0 = the
   browser's frame rate) and return; do the cleanup when `WM_QUIT` arrives
   (cancel the loop, run the code that followed `run()`).
4. Startup (`TRIBE_Game::setup`, data loading) stays synchronous: the tab
   is busy for a few seconds, as the game was. Show "Loading…" in the page
   before calling into the game (Task 6).

**Check:** the main menu shows in the browser and responds to clicks;
the browser reports no "page unresponsive".

**Done when:** the menus work, and a game can be started.

### Task 4: Timers and sound without threads

**Files:** `port/sdl/window.cpp` (`surface_set_timer`, `timer_callback`),
`port/sdl/winmm.cpp` (`surface_time_set_event`), `port/sdl/dsound_memory.cpp`.

**Steps:**

1. Check what Emscripten's SDL2 does for `SDL_AddTimer` without pthreads.
   If timers do not fire, or fire only while the main loop yields (likely),
   poll instead: add `surface_poll_timers()` that fires every due timer
   (both `WM_TIMER` timers and `surface_time_set_event` callbacks), and
   call it from `run_step()`. Keep SDL timers on Windows and Linux.
2. The WAV stream's refill (Dsutil.c, every 50 ms) then runs at frame
   rate, which is often enough (its buffer holds 1.2 s).
3. Audio: SDL's Emscripten audio runs the mixer callback on the main
   thread through Web Audio. Browsers start audio only after a user
   gesture: resume the audio context on the first click or key (check
   whether SDL already does; if not, a small `EM_JS` hook in
   `port/web/`).
4. `mix_lock`/`mix_unlock` (SDL mutexes) are no-ops without threads;
   nothing to change.

**Check:** menu clicks make sounds; in a game, unit sounds play; a
streamed WAV (`STREAM_MUSIC` with a test `open.wav`) plays through.

**Done when:** sound works and `WM_TIMER`-driven behaviour (the game's
50 ms timer) works.

### Task 5: The game folder in the browser

**Files:** `port/web/fs.cpp` and `port/web/shell.html` (new), the port
layer's base-path users (`port/sdl/settings.cpp`, `strings.cpp`, `mci.cpp`,
`gdi.cpp`).

**Steps:**

1. **Choosing the install.** The page asks for the game folder
   (`<input type="file" webkitdirectory>`, or drag-and-drop). JavaScript
   copies its files into `/game` (skipping `avi/` and `*.exe`, `*.dll`
   other than `language.dll` and `languagex.dll`), keeping relative paths.
   Check that `data2/empires.dat` and `language.dll` exist before
   starting; otherwise explain what folder is needed.
2. **Persistence.** Mount `/game` as IDBFS. On later visits, `FS.syncfs`
   loads it and the picker is skipped (offer "choose another folder").
   After the game writes settings, saved games or scenarios, call
   `FS.syncfs(false, …)`: hook the writes (`linux_fopen` with a write mode,
   `linux_open` with `O_CREAT`, `CreateFileA` for writing) to schedule a
   sync, plus a sync on `beforeunload`.
3. **Working folder and base path.** `chdir("/game")` before `WinMain`.
   The port layer finds `settings.ini`, the language DLLs and `music/` next
   to the executable through `SDL_GetBasePath()`; on the web add
   `surface_base_path()` that returns `/game/`, and the fonts come from
   `/fonts/`.
4. **The startup check** `_findfirst("empires.exe")`
   (`RGE_Base_Game::setup`): the copied folder includes `EMPIRES.EXE` only
   if step 1 keeps it. Either keep that one file, or create an empty
   placeholder named `empires.exe` in `/game`.
5. **Memory.** `-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB
   -sMAXIMUM_MEMORY=2GB`. The DRS archives are mapped with `MapViewOfFile`,
   which on Emscripten copies the file into memory: about 60 MB on top of
   the file system's own copy. Measure; if needed, read the DRS files
   once and free them after `RESFILE` has its pointers.

**Check:** first visit: choose the folder, the menu appears; reload: the
menu appears without choosing again; settings changed in the game survive
a reload; a saved game survives a reload and loads.

**Done when:** all four checks pass in Chrome and Firefox.

### Task 6: The page, input and display

**Files:** `port/web/shell.html`, `port/sdl/window.cpp`, `port/sdl/surface.cpp`.

**Steps:**

1. **Page.** A canvas filling the window, a status line (loading, errors),
   the folder picker, a fullscreen button. Errors from `report_crash`-style
   handlers and `surface_message_box` (which becomes `alert()` through
   SDL) are shown in the page.
2. **Layout.** Start in the windowed layout (`surface_set_windowed(1)` on
   the web); the canvas size follows the page, and SDL's letterboxing
   scales the frame. Alt+Enter and the fullscreen button call the
   browser's fullscreen (must come from a click or key event).
3. **Keyboard.** The game uses F-keys (F10 menu, F1 help, F3 pause),
   Escape, Tab and Alt combinations. Make sure SDL prevents the browser's
   defaults for them (F5 reloads, F11 fullscreen, Ctrl+W closes the tab:
   leave Ctrl+W to the browser). Typing in the edit boxes uses
   `SDL_TEXTINPUT`, which works.
4. **Mouse.** Disable the context menu on the canvas (right-click is a
   game command). The game draws its own cursor and hides the system one;
   check the canvas cursor is hidden. Edge scrolling needs the pointer at
   the canvas edge: in a window it can leave the canvas; offer pointer lock
   in fullscreen if edge scrolling proves awkward.
5. **Clipboard.** `SDL_GetClipboardText` on the web is limited (the browser
   clipboard is asynchronous and permission-gated). Accept paste from the
   browser's paste event into a buffer the port layer reads, or leave paste
   unsupported and say so.

**Check:** play the scripted random-map game by hand: select, move, train,
scroll by edge and keys, chat, F10 menu, Escape, scenario editor text
entry.

**Done when:** all of the above work in Chrome and Firefox, fullscreen and
windowed.

### Task 7: Automated checks

**Files:** new `tools/test_web.py` (or a Playwright script under `tests/`),
workflow/docs/testing.md.

**Steps:**

1. Serve `build/web/<time>/` with a local static server.
2. For automation only (never published), a dev option that fills `/game`
   from a local install over HTTP instead of the folder picker, e.g.
   `?devdata=/install/` served by the same local server.
3. Headless Chrome (Playwright): start, wait for the menu, screenshot;
   compare with the Linux build's menu at the same size (the frame is
   rendered by the same code; differences should be only scaling filters
   and glyphs).
4. Script the same flows as `build/sdl_test/linux_play.sh` and
   `linux_editor.sh`: random map, select, train, chat, F10; editor text,
   save scenario, reload, reopen; save game, reload page, load.
5. Collect the console (crashes, "not supported" lines).

**Done when:** the script runs the flows and reports pass/fail with
screenshots.

### Task 8: Size, speed and release build

**Steps:**

1. Release flags: `-O3` (or `-Os` if size matters more), `-flto`, no
   assertions. Measure `.wasm` size and startup time.
2. Profile a large random-map game late in the game in the browser's
   profiler (frame time, `handle_idle`, drawing in `port/asm/Asm_draw.cpp`).
3. Write `workflow/docs/web_build.md` (requirements, building, serving,
   what was checked, known gaps), like `linux_build.md`.

**Done when:** a late-game frame is within the game's 50 ms tick on a
typical laptop, and the doc is written.

## Order and rough size

| Task | Depends on | Size |
| --- | --- | --- |
| 0 Toolchain | – | small |
| 1 Compile | 0 | small–medium |
| 2 Link, signatures | 1 | medium (unknown: signature traps) |
| 3 Main loop | 2 | medium |
| 4 Timers, sound | 3 | small–medium |
| 5 File system | 3 | medium |
| 6 Page, input, display | 3, 5 | medium |
| 7 Automated checks | 5, 6 | medium |
| 8 Size, speed, docs | all | small |

Tasks 4 and 5 can go in parallel after Task 3. Overall, roughly the size of
the Linux port.

## Risks and open questions

- **Signature traps** may hide in rarely used paths and show only at run
  time. The automated flows (Task 7) and the debug build's assertions are
  the main defence.
- **Long synchronous work** (data loading at startup, random-map
  generation) blocks the tab for seconds. Acceptable at first; if the
  browser warns, move those behind a "Loading…" frame, or use Asyncify for
  just those calls.
- **Storage quota.** About 130 MB in IndexedDB is within Chrome's and
  Firefox's limits; Safari may ask or evict. Persistent storage
  (`navigator.storage.persist()`) helps.
- **Memory.** File system copy plus mapped DRS copies plus the game's
  heap: measure early (Task 5).
- **Timing.** `surface_time_get_time` uses SDL's clock, which follows
  `performance.now()`; background tabs throttle timers and frames, which
  pauses the game. That is acceptable for single player.
- **Original memory bugs.** The Linux port exposed several (see
  linux_build.md). Wasm's memory layout differs again; run the debug
  build's `-sSAFE_HEAP=1` and AddressSanitizer (Emscripten supports
  `-fsanitize=address`) over the Task 7 flows.

## Related work

- **MIDI music.** The retail music is MIDI. A software synthesizer in the
  port layer (e.g. TinySoundFont with a free General MIDI SoundFont), mixed
  by the SDL mixer, would give all three builds music. It is independent
  of this plan and can be done before or after it.
- **Higher resolutions** beyond 1280x1024 are a separate plan.

## Progress

(Record each task's checks here as it is done.)
