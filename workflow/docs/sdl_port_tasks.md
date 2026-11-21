# SDL2 port on Windows: task breakdown

Status: written 2026-11-21. Done: Tasks 0 and 1 (see "Progress" at the end).
The rest is still a plan.

Scope: **Windows only for now.** The target is the clang-cl PE build
(`workflow/docs/clang_build.md`). SDL2 replaces DirectDraw, and later
DirectSound. Everything else stays on Win32. Linux is a later step; the
staged plan for it is `workflow/docs/linux_port.md` on the `linux_port`
branch.

## Why it is small on Windows

The game is a Win32 program. Its window, message loop, keyboard, mouse,
files, registry, string tables, GDI text, edit boxes, timers and CD audio
all work on current Windows. What does not work well is DirectDraw:

- 8-bit exclusive fullscreen;
- palette changes;
- a windowed mode that blits to desktop coordinates.

DirectSound still works but is legacy. So the port replaces only the
DirectX layer, and leaves the rest of `src/` alone.

## Design

1. **The game keeps its own window and message loop.**
   `RGE_Base_Game::setup_main_window` still creates the window with
   `CreateWindowExA`. SDL attaches to that window with
   `SDL_CreateWindowFrom(hwnd)` and only renders into it. Input, `WM_PAINT`,
   timers, edit boxes and the parity harness's injected messages stay as
   they are.
2. **DirectDraw is emulated in memory.** `port/sdl/ddraw_memory.cpp`
   implements `IDirectDraw`, `IDirectDrawSurface`, `IDirectDrawPalette` and
   `IDirectDrawClipper`, as the VC98 `ddraw.h` declares them, over plain
   memory. Only `DirectDrawCreate` changes (to `surface_DirectDrawCreate`);
   every other call site and member type stays as it is. Nothing links
   `ddraw.lib`. (The first draft of this plan proposed a new `TSurface`
   class with changed member types; emulating the interfaces needs no
   changes to the callers, and the Linux plan wants the same emulation.)
3. **The game always runs as if fullscreen** (`ScreenMode` 2). The
   "primary surface" is a memory buffer of game resolution, not the
   desktop. Whether it shows in a window or fills the screen is SDL's
   business, so the windowed-DirectDraw code path (`ScreenMode` 1, blits at
   `ClientToScreen` coordinates) is not used.
4. **GDI text keeps working.** Each surface's pixels live in an 8-bit DIB
   section, so `GetDC` returns a memory DC that draws straight into them.
   The 19 files that draw text through `TDrawArea::GetDc` don't change.
5. **A build switch during the transition.** `AOE_SDL` (set by a
   `build_clang.py --sdl` flag) selects the SDL path, and the DirectDraw
   path stays buildable until the SDL build passes. Then the DirectDraw path
   is deleted (Task 6), leaving one path.

Files this plan adds:

| File | Holds |
| --- | --- |
| `include/surface.h`, `port/sdl/surface.cpp` | attaching SDL to the window, presenting the primary surface (Tasks 0, 2–4) |
| `port/sdl/ddraw_memory.cpp` | the in-memory DirectDraw (Task 1) |
| `include/sdl_sound.h`, `port/sdl/sdl_sound.cpp` | `TSoundBuffer` and the SDL mixer (Task 5) |
| `config/sdl.json` | SDL2 package pin, defines, sources and libraries for `--sdl` |
| `tools/fetch_sdl2.py` | downloads the pinned SDL2 VC package into `build/third_party/` (never committed) |

The SDL sources live in `port/sdl/`, not `src/`: the matching-build tools
(`tools/build_executable.py`) require a registry route for every file in
`src/`, and these files are not part of the matching build.

Every task checks the same three things:

- the build compiles with `--sdl` and without it;
- a play test on Windows (Wine is fine for a first look);
- the replay parity cases (`behavior_replay.md`, `human_replay.md`) still
  pass. Rendering must not change game state.

---

## Task 0: Put SDL2 into the build and attach it to the game window

- Fetch the pinned SDL2 VC development package (`tools/fetch_sdl2.py`).
- `tools/build_clang.py`:
  - a `--sdl` flag that adds `-DAOE_SDL=1` and `the SDL2 include directory`;
  - link `SDL2.lib` and drop `ddraw.lib` (and later `dsound.lib`);
  - copy `SDL2.dll` next to `empiresx.exe`.
- Add `make sdl`.
- Check that the SDL headers compile against the VC98 headers under
  clang-cl with `_MSC_VER` 1200. `SDL_stdinc.h` must find `stdint.h`;
  clang supplies it. Include `SDL.h` only from `port/sdl/surface.cpp` and
  `port/sdl/sdl_sound.cpp`, never from shared headers, so the rest of the tree
  never sees it.
- `WinMain` (`src/main.cpp`) calls nothing from SDL. Call `SDL_Init` from
  `TDrawSystem::Init` (Task 2). Set `SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP`
  to `"0"` so SDL never pumps messages itself, and install an
  `SDL_SetEventFilter` that drops all events. The game reads input from
  its own messages.

**Smoke test (do this first, it decides the design).** In
`setup_main_window`, call `SDL_CreateWindowFrom(prog_window)` and
`SDL_CreateRenderer`. Log every message in `RGE_Base_Game::wnd_proc` and
confirm that each one still reaches it: keys, mouse, `WM_TIMER`,
`WM_ACTIVATEAPP`, and above all `WM_PAINT`.

SDL subclasses a foreign window and forwards to the original procedure,
but its `WM_PAINT` handler calls `ValidateRect` first. That can empty
`GetUpdateRect` in `RGE_Base_Game::check_paint`. If that breaks painting,
the fixes, in order of preference:

1. have `check_paint` stop relying on the update rect;
2. re-subclass after SDL so the game sees messages first;
3. drop SDL for presenting and use `StretchDIBits` from the DIB section,
   which needs no dependency at all.

**Done when:** the `--sdl` build starts and plays exactly as before (still
on DirectDraw), with SDL attached, and the message log matches a build
without SDL.

## Task 1: Replace DirectDraw with in-memory surfaces

`port/sdl/ddraw_memory.cpp` implements the four DirectDraw interfaces the
game uses. `src/drawarea.cpp` maps `DirectDrawCreate` to
`surface_DirectDrawCreate` under `AOE_SDL`; nothing else in `src/`
changes. `GetDXVersion` (getdxver.cpp) is untouched: it probes the system's
own `DDRAW.DLL` and DirectPlay through `LoadLibraryA`, which still exist on
Windows.

How the objects behave:

- **Pixels.** Each surface is an 8-bit top-down DIB section, with the pitch
  rounded up to 4 bytes and selected into its own memory DC.
- **`Lock` / `GetSurfaceDesc`.** These fill `DDSURFACEDESC`: `lpSurface`,
  `lPitch`, size, 8-bit palettized `DDPIXELFORMAT`, and caps. Surfaces
  created without `DDSCAPS_SYSTEMMEMORY` report video memory, as on real
  hardware; the game's surfaces are system memory unless it runs with
  `VIDEOMEMORY`.
- **Blits.** `Blt` and `BltFast` copy, fill (`DDBLT_COLORFILL`) and honor
  source color keys (`DDBLT_KEYSRC`, `DDBLTFAST_SRCCOLORKEY`). Copies
  within one surface may overlap (the scrolling view does that in video
  memory). Stretching is nearest-neighbour; no call site is known to
  stretch.
- **Clipping**, as in DirectDraw:
  - `Blt` clips to the surface when a clipper is attached, and fails with
    `DDERR_INVALIDRECT` without one;
  - `BltFast` never clips, and fails with `DDERR_BLTFASTCANTCLIP` on a
    surface with a clipper (`TMousePointer` already detaches it first).
- **`GetDC`.** Returns the surface's memory DC, after setting its color table
  from the surface's palette, or from the primary's palette when the
  surface has none (as DirectDraw does). `Lock` and `ReleaseDC` call
  `GdiFlush`.
- **Display modes.** The display mode is never changed:
  - `SetDisplayMode` only records an 8-bit mode;
  - `EnumDisplayModes` reports 640x480, 800x600, 1024x768 and 1280x1024 in 8
    and 16 bits;
  - `GetDisplayMode` reports the recorded mode as 8 bits, so windowed
    `ScreenMode` 1 no longer needs an 8-bit desktop.
- **The primary surface.** It has the recorded mode's size in exclusive
  mode, and the desktop's size otherwise (the game blits to it in screen
  coordinates then).
  - Every write to it (blit, `Unlock`, `ReleaseDC`, palette change) calls
    `surface_present_primary`, which for now copies the changed rectangle
    to the window with GDI `BitBlt`. That is what a real primary surface
    shows. Task 2 replaces this with SDL.
- **Never lost.** `IsLost`, `Restore` and `GetBltStatus` always succeed, so
  the restore paths stay compiled but never run.

**Done when:** the import table has no `DDRAW.dll`, and a game plays.

## Task 2: Present the frame with SDL

In `port/sdl/surface.cpp`:

- **The present call.** `TDrawSystem::Present()` runs only when the frame
  is dirty. It converts the primary surface through the current palette
  into an `SDL_PIXELFORMAT_ARGB8888` streaming texture of game resolution
  (`SDL_LockTexture`, then a 256-entry lookup table). Then it calls
  `SDL_RenderClear`, `SDL_RenderCopy` and `SDL_RenderPresent`. Use
  `SDL_RenderSetLogicalSize(w, h)` for letterboxing, and set
  `SDL_HINT_RENDER_SCALE_QUALITY` to `"0"` (nearest).
- **When to present.** At the end of `TDrawSystem::Paint`, after
  `TMousePointer::Poll` and after `TMousePointer::draw`/`erase` when they
  touch the primary. As a backstop, once per `RGE_Base_Game::run`
  iteration. At most once per game frame, and the pointer poll may add
  extra presents. Turn vsync off at first, so presenting never blocks the
  game loop.
- **Palette sources.** They all reach the emulated `IDirectDrawPalette` (`surface_present_palette`):
  - `TDrawSystem::Init`, `ModifyPalette`, `SetPalette`;
  - `TDrawArea::SetPalette`, through the palette's `SetEntries`;
  - GDI `SetPaletteEntries` on `prog_palette` in `TRIBE_Game::setup` and
    `setup_palette`. Check that each of these then reaches
    `TDrawSystem::SetPalette`; if one doesn't, add the call.

  A palette change marks the frame dirty, so fades (`TRIBE_Credits_Screen::handle_idle`,
  `ModifyPalette`) animate.
- **Fullscreen-only behavior.** The game runs with `full_screen` = 1, but
  the window is ordinary. Review these branches so none of them changes the
  desktop mode or covers the screen without being asked:
  - `basegame.cpp`: lines 1433, 1493, 1536 and 2369, `setup_main_window`,
    and `set_screen_size` (`SetWindowLongA`, `SetWindowPos`,
    `GetDeviceCaps`);
  - `Panel_ez.cpp:93` and `drawarea.cpp:104`.

  In `set_screen_size`, size the window so its client area equals the game
  resolution.
- **Window clearing.** `RGE_Base_Game::clear_window` (`GetDC`,
  `FillRect`, `ReleaseDC` on the window) clears the primary instead, so
  GDI never draws on the SDL-owned window. `RGE_Base_Game::handle_query_new_palette`
  and `TDrawSystem::HandleQueryNewPalette`/`HandlePaletteChanged` keep their
  GDI palette calls and do nothing visible. Make them call `Present()`.
- **Color depth check.** `RGE_Base_Game::setup_graphics_system` refuses
  to run when the desktop is below 8 bpp. Keep it; it never fires now.

**Done when:** in a window the size of the game resolution, all of these
render correctly:

- the main menu, single-player setup, an in-game map, the scenario editor
  and the credits;
- the custom mouse pointer, with no trails;
- palette fades.

Parity cases pass.

## Task 3: GDI text on surfaces

`TDrawArea::GetDc` gives an HDC to the text code in 19 files:

> Pnl_msg, Pnl_inp, Pnl_txt, basegame, pnl_btn, pnl_edit, pnl_lst,
> scr_cred, rpnl_tim, spnl_obj, stat_obj, tpnl_obj, tpnl_tim, tpnl_txl,
> tpnl_btn, tpnl_inv, tpnl_pop, tpnl_tml, drawarea.

They call `SelectObject`, `SetTextColor`, `SetBkMode`, `SetBkColor`,
`TextOutA`, `DrawTextA`, `GetTextExtentPoint32A`, `SelectClipRgn`,
`FillRect`, `MoveToEx` and `LineTo`. All of these work on a DIB-section
memory DC. What to get right:

- **Color table sync.** On every palette change, update the DIB color
  table of every surface with `SetDIBColorTable` (or only the ones with a
  DC currently out). GDI matches `RGB()` text colors against that table.
- **`PALETTEINDEX` colors.** If the game passes these (grep
  `PALETTEINDEX` and `0x01000000`), select the game's `HPALETTE`
  (`TDrawSystem::Pal`, `prog_palette`) into the memory DC and call
  `RealizePalette` on it in `MemSurface::GetDC`.
- **No drawing while locked.** `GetDC` while the surface is locked fails,
  as DirectDraw did (`TDrawArea::GetDc` already checks `Bits`).
  `ReleaseDC` calls `GdiFlush()` before the game locks the pixels again.

**Done when:** text on the help screen, scenario editor, chat, achievements,
timeline, unit panels and credits has the right color and position, and
matches the DirectDraw build.

## Task 4: Window modes and scaling

- **Resizable window.** The window is resizable, the frame scales with
  letterboxing, and integer scaling is an option. Fullscreen-desktop is a
  borderless window of monitor size: `SetWindowLongA(GWL_STYLE, WS_POPUP)`
  and `SetWindowPos` in `set_screen_size`. Toggle it with a command-line
  switch (`WINDOWED`, `FULLSCREEN` already exist in
  `RGE_Base_Game::setup_cmd_options`) and with Alt+Enter in
  `RGE_Base_Game::wnd_proc`.
- **Mouse coordinates.** Once the client area differs from the game
  resolution, convert window coordinates to game coordinates in one helper,
  `surface_window_to_game(POINT*)`, and back in
  `surface_game_to_window(POINT*)`. Apply them at:
  - the top of `RGE_Base_Game::wnd_proc`, for every mouse message's
    `lParam`;
  - after `GetCursorPos`/`ScreenToClient` in
    `TMousePointer::update_mouse_position`, `TPanel::get_mouse_pos`,
    `RGE_Base_Game::get_mouse_pos`, `RGE_View::Get_Cursor_Position` and
    `RGE_Main_View::do_auto_scroll`;
  - before `SetCursorPos` in `RGE_View::handle_scroll_view`,
    `end_scroll_view` and `TMousePointer::center`;
  - in `RGE_Main_View::mouse_left_down_action`, which uses
    `ClientToScreen` and `ScreenToClient`.
- **Edge scrolling.** `RGE_Main_View::do_auto_scroll` uses
  `GetWindowRect` and the cursor position. Clip the cursor to the window in
  fullscreen-desktop mode (`ClipCursor`, a new import) so edge scrolling
  works.
- **Edit boxes.** `TEditPanel::set_rect` (`MoveWindow`) places a real
  child `EDIT` window in game coordinates. Scale its rectangle with
  `surface_game_to_window`. Its font won't scale, which is acceptable.
  Or keep it unscaled and accept a small offset; decide when you see it.

**Done when:** at 1x, 2x and fullscreen-desktop, clicks, drag selection,
edge scrolling, minimap clicks and edit boxes hit the right place.

## Task 5: Sound through SDL (do last)

DirectSound works on current Windows, so this task is only needed to
remove DirectX entirely or to prepare for Linux.

- **`TSoundBuffer`** (`port/sdl/sdl_sound.cpp`): PCM bytes and format, play
  cursor, loop, volume (hundredths of a dB, gain = 10^(v/2000)), pan
  (attenuates the opposite channel), frequency, status. Duplicates share
  the same bytes. One `SDL_AudioCallback` mixes at 22050 Hz, 16-bit,
  stereo, with linear resampling. Lock buffer state against the audio
  thread.
- **`Dsutil.c` is C.** Provide `LPDIRECTSOUND`, `LPDIRECTSOUNDBUFFER`
  and the `IDirectSound_*` / `IDirectSoundBuffer_*` macros it uses from
  `include/sdl_sound.h` under `AOE_SDL`, as plain C functions:
  - `CreateSoundBuffer`, `Lock`, `Unlock`, `Play`, `Stop`, `GetStatus`;
  - `GetCurrentPosition`, `SetCurrentPosition`, `SetVolume`, `Restore`,
    `Release`;
  - `DuplicateSoundBuffer`, through `lpVtbl` in `SndObjCreate`; change
    that one call to the macro form.
- **Call sites:**

| Function | Calls |
| --- | --- |
| `TSound_Driver::init` | `DirectSoundCreate`, `GetCaps`, `SetCooperativeLevel`, `CreateSoundBuffer` (primary), `SetFormat`, `Play`, `Release` |
| `TSound_Driver::exit` | `Stop`, `Release` |
| `TDigital::load`, `unload` | `CreateSoundBuffer`, `Lock`, `Unlock`, `Release` |
| `TDigital::play`, `stop`, `set_volume`, `set_pitch`, `set_pan`, `is_playing` | `GetStatus`, `SetVolume`, `SetFrequency`, `SetPan`, `SetCurrentPosition`, `Play`, `Stop` |
| `SndObjCreate`, `SndObjGetFreeBuffer`, `SndObjPlay`, `SndObjStop`, `DSLoadSoundBuffer`, `DSReloadSoundBuffer`, `DSFillSoundBuffer` (Dsutil.c) | as listed above |
| `ds_stream_init`, `ds_stream_file`, `ds_stream_time_func`, `ds_stream_stop`, `ds_stream_volume`, `ds_stream_pause`, `ds_stream_resume` (Dsutil.c) | looping stream buffer. `GetCurrentPosition` must advance in real time for the half-buffer refill. `timeSetEvent` stays (WINMM). |

- **Types.** Change `include/dsutil.h`, `include/sounddrv.h`,
  `include/game_types.h` and `src/game_globals.cpp` (the buffer globals).
- **Leave on WINMM:** `mmio*` WAV reading (Dsutil.c), `mixer*` volume
  (`TSound_Driver::open_mixer`, `set_volume`, `TMusic_System::open_mixer`,
  `set_volume`), and MCI CD audio (Cdaudio.cpp, music.cpp). They work on
  Windows. Ripped-track music is a separate, optional change.

**Done when:** unit, interface and ambient sounds and streamed WAV music
play with correct volume and pan. The volume sliders work. Parity cases
pass.

## Task 6: Remove the DirectDraw path

- Delete the `#ifndef AOE_SDL` branches, the `IDirectDraw*` forward
  declarations, and `ddraw.lib` (and `dsound.lib` after Task 5). Make
  `--sdl` the default and drop the flag.
- **Vestigial draw path.** `TDrawSystem::DrawType` 1 (the `WING` switch in
  `setup_cmd_options`) has no surface code left in the retail game
  (`TDrawArea::Init` returns 0 for it). Remove the switch, or make it an
  alias for windowed.
- Update `workflow/docs/clang_build.md` (it should mention SDL2 and
  `SDL2.dll`) and `README.md`.

**Done when:** the import table has no `DDRAW.dll` (and no `DSOUND.dll`),
and the full play test and parity cases pass.

---

## Order

```
0 -> 1 -> 2 -> 3 -> 4 -> 6
          \-> 5 (independent of 3 and 4) -/
```

Task 0's smoke test decides how the window is attached; do it before
anything else. Tasks 3 and 5 can run in parallel once Task 2 renders.

## Not needed on Windows

These stay on Win32 untouched, and they are what a later Linux port still
has to supply:

- the window and message loop (basegame.cpp);
- `GetKeyState`, `GetAsyncKeyState` and `GetCursorPos`;
- `EDIT` child windows (pnl_edit.cpp) and the clipboard;
- GDI text rendering (FreeType on Linux);
- the registry (registry.cpp) and `LoadStringA` on `language.dll`;
- file mapping (Res_file.cpp) and `_findfirst`;
- MCI CD audio, `mmio`, `mixer` and `timeSetEvent`;
- the DirectPlay multiplayer layer.

## Open questions

- Does `WM_PAINT` still reach the game after `SDL_CreateWindowFrom`, with
  its update region intact? (Task 0)
- Does any `Blt` call stretch? (Task 1)
- Does the text code use `PALETTEINDEX` colors? (Task 3)
- Does `TRIBE_Game::setup_palette`'s `SetPaletteEntries` always reach
  `TDrawSystem::SetPalette`, or does the game rely on GDI realizing the
  palette for DirectDraw? (Task 2)
- Do the AVI intro videos (`MCIWndCreateA` child window in
  `TRIBE_Game::start_video`) still show over the SDL-rendered window? If
  not, stop presenting while `start_video` is active.

## Progress

### Task 0 — done 2026-11-21

- `make sdl` builds `build/sdl/<time>/empiresx.exe` with `SDL2.dll` beside
  it; `make play-sdl` runs it under Wine. SDL2 2.30.9 headers compile
  against the VC98 headers once `surface.cpp` defines the
  `SDL_DISABLE_*INTRIN_H` switches (clang's intrinsic headers need
  `_aligned_malloc`, which VC6's runtime lacks).
- `surface_attach_window` (called from `RGE_Base_Game::setup_main_window`)
  attaches SDL with `SDL_CreateWindowFrom`, with SDL's message loop off and
  all SDL events dropped. `surface_detach_window` runs in
  `~RGE_Base_Game` before `DestroyWindow`.
- Smoke test result: SDL forwards every message, but it validated the
  window before forwarding `WM_PAINT` (all 11 paints in the test reached
  the game with an empty update region, against 10 of 11 non-empty without
  SDL) and swallowed `WM_ERASEBKGND`. Fixed by a window procedure on top
  of SDL's that sends those two messages straight to the game. With it,
  the paint sequence matches the plain build. The only extra messages are
  SDL's own `WM_GETTEXT`/`WM_GETTEXTLENGTH` and input-language messages.
- Checked under Wine 10 on Xvfb with the retail data at 1024x768
  (`NOSTARTUP GAMECD NODXCHECK`; the install's `langua_1.dll` copied to
  `languagex.dll`): main menu, single-player menu and back again are
  pixel-identical to the plain clang build. That was also the first run of
  the clang build; it reaches the main menu like the retail executable.
- Not run: the parity suite (`game.cmd test-parity`) needs Windows.

### Task 1 — done 2026-11-21

- `DDRAW.dll` is no longer imported by the `--sdl` build.
- Checked under Wine (1024x768, see "Testing under Wine" below):
  - main menu, single-player menu and back again are pixel-identical to
    the plain clang build;
  - the scenario editor renders correctly: generated random map,
    scrolling, the Units tab's text list, and a minimap click. The editor's
    random-map seed does not come from `RANDOMMAP`, so these screens were
    inspected rather than compared pixel by pixel.
- A Task 0 bug showed up at 800x600: SDL answers `WM_NCCALCSIZE` itself for
  a borderless window, which left the game's popup with a 0x0 client area
  whenever it did not cover the whole screen. `surface_wnd_proc` now sends
  `WM_NCCALCSIZE` straight to the game too.
- Not checked: a running game. See the next section.

### Testing under Wine

- The test setup (Xvfb without root, a separate Wine prefix with a fake CD
  drive, screenshot and comparison scripts) lives under `build/sdl_test/`
  and is not committed.
- The plain clang build, the VC6 build and the `--sdl` build all stop on
  "Starting a new game..." when a random-map game starts under Wine 10.
  The main thread sits in the game's idle loop (sampled with `ptrace`: it
  is in libc, not in game code). The retail executable cannot be compared
  because it stops at its CD check. This predates the SDL work and is not
  yet explained.
- At 800x600 (the default `Screen Size`), Wine scales the plain build's
  emulated display mode up to the whole screen, while the SDL build draws
  an 800x600 window. Pixel comparisons therefore use 1024x768.
