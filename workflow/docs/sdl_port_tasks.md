# SDL2 port on Windows: task breakdown

Status: written 2026-11-21. Done: Tasks 0 to 6 (see "Progress" at the end).
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
| `port/sdl/dsound_memory.cpp` | the in-memory DirectSound and its SDL mixer (Task 5) |
| `config/sdl.json` | SDL2 package pin, defines, sources and libraries for the clang build |
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

### Task 2 — done 2026-11-21

- `port/sdl/surface.cpp` creates an SDL renderer on the attached window
  (accelerated, falling back to software, no vsync, nearest scaling) and a
  streaming ARGB texture the size of the primary surface (of the client
  area outside exclusive mode). Each primary-surface change converts only
  the changed rectangle through a 256-entry palette table.
- Presenting is throttled: a change presents at once unless the last
  present was under 4 ms ago; otherwise it is left pending, and
  `surface_present_flush` (called each iteration of `RGE_Base_Game::run`)
  shows it. `WM_PAINT` presents the last frame again before the game
  handles it, because a swap chain does not survive being covered or
  remapped the way a real primary surface did.
- `RGE_Base_Game::clear_window` clears the primary surface instead of
  filling the window with GDI.
- Done as planned without touching the fullscreen branches: the game
  already sizes its popup window so the client area equals the game
  resolution, at the top-left of the screen. Making that a proper window or
  a scaled fullscreen is Task 4.
- Checked under Wine: at 1024x768 the menu screens are pixel-identical to
  the plain clang build in three of four runs; the fourth, the first run in
  a fresh prefix, showed a black main menu at the 15 s mark, before the
  `WM_PAINT` re-present existed. The scenario editor renders correctly, and
  its first screen is pixel-identical. At 800x600 the menus render 1:1 in
  the top-left corner and clicks land where they should.
- Not checked: palette fades on the credits screen, and which SDL renderer
  Wine ends up using.

### Task 3 — done 2026-11-21

- No new code: Task 1 already covers it. `MemSurface::GetDC` loads the
  DIB color table from the surface's palette (or the primary's) before
  handing out the DC, and the text code only passes plain `RGB()` colors
  (no `PALETTEINDEX`; checked by grep), which GDI matches against that
  table.
- Checked under Wine at 1024x768, against the plain clang build: the
  "help not updated" dialog and the scenario editor's Players, Diplomacy,
  Individual Victory, Options and Cinematics tabs are pixel-identical,
  including all their text.
- The Messages tab differs, and the SDL build is the more faithful one.
  Its instructions box is a child `EDIT` window. The SDL build shows it (a
  black field); the plain build under Wine shows the previous tab's pixels
  there, because Wine's exclusive-mode DirectDraw hides child windows. On a
  real 8-bit display GDI draws child windows over the primary surface.
- Clicking OK on the help dialog opens Wine's WinHelp viewer in both
  builds; behind it the SDL build keeps showing the game, the plain build
  shows black.

### Task 4 — done 2026-11-21

How it was built, where it differs from the plan above:

- **Window layout** (`layout_apply` in `port/sdl/surface.cpp`), applied
  whenever a primary surface is created (game start, resolution change):
  - fullscreen (the default) is a borderless popup covering the screen;
  - windowed is an ordinary resizable window, sized to the largest whole
    multiple of the game resolution that fits the work area and centred.
    The size is measured after `SetWindowPos` and corrected once, because
    the real frame can differ from `AdjustWindowRect`'s.
  - The game's `WINDOW` switch now selects the windowed layout
    (`surface_set_windowed`) and leaves `full_screen` at 1, so the game
    always takes its fullscreen (`ScreenMode` 2) paths.
  - Alt+Enter toggles the two layouts; the game never sees the key.
  - SDL scales the frame with `SDL_RenderSetLogicalSize` (letterboxed,
    nearest neighbour). Integer-only scaling was not added.
- **A virtual screen in game pixels** instead of per-site mapping helpers:
  - `surface_wnd_proc` rewrites the coordinates of mouse messages
    (`WM_MOUSEMOVE` to `WM_MBUTTONDBLCLK`) from client pixels to game
    pixels, clamped to the game area, so the letterbox bars act as its
    edges.
  - The game's direct calls go through `surface_get_cursor_pos`,
    `surface_set_cursor_pos`, `surface_client_to_screen`,
    `surface_screen_to_client` and `surface_get_window_rect` (declared in
    `include/surface.h`; plain Win32 calls in non-SDL builds). For the game
    window, screen and client coordinates are the same game pixels, and its
    window rectangle is the game area.
  - Changed call sites: `TMousePointer::update_mouse_position`, `center`;
    `RGE_Base_Game::get_mouse_pos`; `RGE_Main_View::do_auto_scroll`,
    `mouse_left_down_action`; `TPanel::get_mouse_pos`;
    `RGE_View::Get_Cursor_Position`, `handle_scroll_view`,
    `end_scroll_view` and the other `SetCursorPos` sites in view.cpp;
    `color_log`. `RGE_Base_Game`'s other `GetWindowRect` calls size the real
    window and are unchanged.
  - Edit boxes: `TEditPanel::set_rect` places its `EDIT` window through
    `surface_move_child_window`, which scales the rectangle. Its font is
    not scaled, and a box is not moved again when the layout changes while
    it is shown.
- SDL had to stop handling `WM_GETMINMAXINFO`: for a window it considers
  not resizable, SDL pins the size it last recorded, which undid every
  layout change. It now goes straight to the game, like `WM_NCCALCSIZE`.
- The SDL event filter lets window events through (the renderer's event
  watcher, which follows the window size, only sees events that pass the
  filter); `surface_present_flush` discards them.

Checked under Wine at 800x600 on a 1024x768 screen, with the Direct3D
renderer:

- fullscreen scales the menus to the whole screen, and clicks land on the
  right buttons;
- with `WINDOW`, an 800x600 client area is centred, and a click inside it
  works;
- Alt+Enter switches to fullscreen and back, keeping the right sizes;
- in the scenario editor, tab clicks land, the Messages instructions box is
  placed at its scaled position, and pushing the cursor to the screen edge
  scrolls the map.
- The plain clang build, now calling the pass-through wrappers, is still
  pixel-identical to before on the menu screens.

Not checked: text typed into an edit box (it did not show in the scaled
instructions box; the plain build hides that box under Wine, so there is no
reference), high-DPI displays, and multiple monitors (the layout uses the
primary screen).

### Task 5 — done 2026-11-21

- Built the same way as Task 1, not with a new `TSoundBuffer` type:
  `port/sdl/dsound_memory.cpp` implements `IDirectSound` and
  `IDirectSoundBuffer` as the VC98 `dsound.h` declares them, and
  `src/Sounddrv.cpp` maps `DirectSoundCreate` to
  `surface_DirectSoundCreate` under `AOE_SDL`. `Dsutil.c`'s
  `IDirectSound*_` C macros call through the same vtables, so neither file
  changed otherwise. `dsound.lib` is no longer linked; `DSOUND.dll` is gone
  from the imports.
- Mixer: one SDL audio device, 44100 Hz, 16-bit stereo, 512-frame
  callbacks. Every playing buffer is resampled with linear interpolation (8
  or 16 bit, mono or stereo, any rate, `SetFrequency` honored). Volume and
  pan follow DirectSound's hundredths-of-a-decibel curves, and pan
  attenuates only the opposite channel. Duplicates share sample memory.
  `GetStatus` reports `PLAYING | LOOPING` for looping buffers, as
  DirectSound does (`TDigital::is_playing` compares with `==`, so it is
  false for them, as before).
- The play cursor advances in the audio callback. The write cursor leads
  it by 15 ms, so `Dsutil.c`'s timer thread refills the stream buffer as it
  did. Playback state changes under `SDL_LockAudioDevice`.
- With no SDL audio device, `surface_DirectSoundCreate` fails and the game
  runs silent, as it does without DirectSound.
- Kept on WINMM, as planned: `mmio*` WAV reading, `mixer*` volume and MCI
  CD audio.
- Checked under Wine with SDL's `disk` audio driver (which writes the mix to
  a file): each menu button press produced a click (six presses, six
  sounds). A streamed 5-second 440 Hz test WAV came out as 440 Hz for 5
  seconds at the expected -50 dB peak (the default `Music Volume`), across
  several refills of its 1.2-second ring buffer. The menus are still
  pixel-identical to the plain clang build.
- Found while testing, not changed: with `STREAM_MUSIC`, the music system
  passes a full path and `TSound_Driver::stream_file` prepends the sound
  folder again, so `WaveOpenFile` fails and no music plays. The streaming
  test above bypassed this in a throwaway build. The retail game ships
  only MIDI music, which goes through MCI.

### Task 6 — done 2026-11-21

- The clang build is the SDL build: `tools/build_clang.py` always applies
  `config/sdl.json` (the `--sdl` flag is gone), `make clang` writes
  `build/clang/<time>/` with `SDL2.dll`, and `make play` copies both files.
  The `make sdl` and `make play-sdl` targets and `build/sdl/` are gone.
  The import table has neither `DDRAW.dll` nor `DSOUND.dll`.
- The `#ifdef AOE_SDL` branches are gone (the user decided on 2026-11-21
  that only the clang build matters): `src/` calls `port/sdl/`
  unconditionally, `surface.h` lost its plain-Win32 fallbacks, and
  `config/sdl.json` defines nothing. The VC5 matching build and the VC6/Wine
  build (`tools/build_executable.py`, used by `game.cmd`) compile `src/`
  without `port/sdl/` and so no longer link.
- The `WING` switch selected the software draw path, which has no surface
  code left and could only fail to start. In the SDL build it now means
  `WINDOW`.
- `workflow/docs/clang_build.md` describes the SDL build and how to play it.
- Checked: the tool tests pass; the default clang build's menus are
  pixel-identical to the reference under Wine; `WING` opens the windowed
  layout; a random-map game from a retail install plays (selection,
  movement, training, scrolling, game menu, sounds). Not run: the parity
  suite (Windows only, and built on the VC5 route).

### SDL owns the window and the event loop — done 2026-11-21

The first step towards Linux (item 1 of "What a Linux port still needs"):
SDL creates the game window and drives the game's message loop, so input
no longer reaches the game from Windows' message queue.

- `port/sdl/window.cpp` (new):
  - `surface_create_main_window` replaces `CreateWindowExA` in
    `RGE_Base_Game::setup_main_window`: `SDL_CreateWindow` in the starting
    layout, and the game knows the window by its handle (from
    `SDL_GetWindowWMInfo` on Windows). `surface_destroy_main_window`
    replaces `DestroyWindow` in `~RGE_Base_Game`.
  - `RGE_Base_Game::run` takes messages from `surface_peek_message` and
    `surface_get_message` and hands them to `surface_dispatch_message`,
    which calls the game's unchanged `wnd_proc`. There is no
    `TranslateMessage`: `WM_CHAR` comes from the translator.
  - The queue keeps Windows' order: posted messages (thread-safe; the
    stream timer thread posts), then input, then `WM_QUIT`, then
    `WM_PAINT` (from an emulated update region, kept until validated),
    then `WM_TIMER` (SDL timers, never piling up).
  - SDL events become Windows messages: keys with virtual-key codes and
    `lParam` flags (system keys for Alt and F10), `WM_CHAR` from SDL text
    input plus Enter, Backspace, Tab, Escape and Ctrl+letter, mouse
    messages in game pixels with `MK_*` flags, double-clicks from SDL's
    click count, activation (`WM_ACTIVATEAPP`, `WM_ACTIVATE`), `WM_SIZE`
    and `WM_CLOSE`. Alt+Enter is taken before the game sees it.
  - Wrappers for the game's calls on its window, which fall through to
    Windows for any other window: `surface_post_message`,
    `surface_send_message`, `surface_post_quit_message`,
    `surface_set_timer`, `surface_kill_timer`,
    `surface_invalidate_rect`, `surface_validate_rect`,
    `surface_get_update_rect`, `surface_update_window`,
    `surface_is_iconic`, `surface_get_foreground_window`,
    `surface_set_cursor` (for `SetCursor` and the class cursor),
    `surface_set_capture`, `surface_release_capture`,
    `surface_get_capture`, `surface_get_key_state`,
    `surface_get_async_key_state` and `surface_get_keyboard_state`.
    About 110 call sites in 15 files now use them.
  - The Windows bridge: a procedure on top of SDL's sends the messages
    Windows components send the game window (edit-box `WM_COMMAND` and
    `WM_CTLCOLOR*`, `MM_MCINOTIFY`, palette and power messages,
    `WM_SYSCOMMAND`, `WM_USER` and up) straight to the game, and turns
    Windows' own `WM_PAINT` into a repaint. Everything else goes to SDL.
- `port/sdl/surface.cpp`: the layout uses SDL (desktop fullscreen, or a
  resizable window sized from `SDL_GetDisplayUsableBounds` and centred), and
  the cursor wrappers use `SDL_GetGlobalMouseState` and
  `SDL_WarpMouseInWindow`. The `SDL_CreateWindowFrom` attachment and its
  message routing are gone.
- SDL already reports mouse events in the renderer's logical coordinates
  once `SDL_RenderSetLogicalSize` is set, so the translator only clamps
  them to the game area. (Mapping them again worked only at 1:1 scale.)
- Still Windows on Windows, for Linux to replace: the game's `EDIT` boxes
  (which take keyboard focus as real child windows; their own key handling
  uses Win32 `GetKeyState`), the video window, `ShowWindow`/`SetFocus` on
  the game window during setup, and the input-disabled child window that
  captures the mouse while loading.
- Checked under Wine: the menu screens and six scenario-editor tabs at
  1024x768 are pixel-identical to the earlier references; at 800x600,
  fullscreen scaling, the `WINDOW` switch, Alt+Enter, editor clicks, map
  generation and edge scrolling work; a random-map game plays (selection,
  movement, training, the `C` hotkey, arrow scrolling, F10), and Enter
  opens chat, where typed text is sent and shown. The chat box itself
  shows no text while typing: it is a Windows `EDIT` control whose colours
  come out black on black (also before this step).

### Text without GDI — done 2026-11-21

Item 2 of "What a Linux port still needs": the game's surfaces no longer
use GDI, and text is drawn with SDL_ttf.

- `port/sdl/gdi.cpp` (new) implements the GDI calls the game makes on a
  surface's device context: fonts (`CreateFontIndirectA`), `TextOutA`,
  `DrawTextA`/`DrawTextExA` (left, centre, right, vertical centre and
  bottom for single lines, word wrap, `DT_CALCRECT`),
  `GetTextExtentPoint32A`, `GetTextMetricsA`, text and background colours
  and `OPAQUE`/`TRANSPARENT`, rectangular clip regions, pens with
  `MoveToEx`/`LineTo`, brushes with `FillRect`, stock pens and brushes, and
  `DeleteObject`. About 425 call sites in 25 files call the `gdi_*`
  versions, which fall through to Win32 for other device contexts.
- Drawing follows GDI on a palettized surface: SDL_ttf's non-anti-aliased
  "solid" rendering with mono hinting and no kerning, colours matched to the
  nearest palette entry (lowest index on a tie; `PALETTEINDEX` honoured),
  `OPAQUE` filling the text cell, `LineTo` leaving out the end point.
- Every GDI object the game creates is one of gdi.cpp's. On Windows it can
  produce a real GDI twin (`gdi_win32_object`), which the EDIT boxes get
  (their font, and the brush answering `WM_CTLCOLOREDIT`), as do real device
  contexts handed one of these objects.
- Fonts: the game asks for "Copperplate Gothic Light" (bold) and Arial (from
  the language DLL). The real Windows font file is used when present
  (`arial.ttf`, `COPRGTL.TTF`, ...); otherwise Liberation Sans or Serif,
  shipped in `fonts/` next to the executable. A bold request without a bold
  file uses SDL_ttf's synthetic bold. Font sizes follow `lfHeight` (the em
  height in pixels for the negative heights the game uses).
- Surfaces (`ddraw_memory.cpp`) are plain `calloc` memory now; the DIB
  sections, `GdiFlush` and the colour-table updates are gone, and the
  desktop size comes from `SDL_GetDesktopDisplayMode`.
- Build: `config/sdl.json` (schema 2) pins three packages, fetched by
  `tools/fetch_sdl2.py`: SDL2 2.30.9, SDL2_ttf 2.22.0 (VC package, with
  FreeType and HarfBuzz inside `SDL2_ttf.dll`) and Liberation fonts 2.1.5
  (SIL Open Font License). `make clang` copies `SDL2.dll`, `SDL2_ttf.dll` and
  `fonts/` next to `empiresx.exe`; `make play` copies them too.
- Still GDI, for Linux to replace: `Dib.cpp`'s palette and bitmap helpers
  (`CreatePalette`, `GetPaletteEntries`, `GetNearestPaletteIndex`, ...), the
  window device contexts in `clear_window`, `setup_graphics_system` and the
  palette messages, `CreateICA`/`GetDeviceCaps` for the desktop size, and
  the EDIT boxes' own drawing.
- Checked under Wine (no Arial there, so Liberation Sans): the menus differ
  from the GDI references only in their text pixels (about 1% of the
  screen); text renders cleanly on the menus, the six scenario-editor tabs,
  the in-game panels, resource bar and game menu; the selection outline
  drawn with `LineTo` shows. Not checked: text on real Windows with the
  real fonts.

### Edit boxes drawn by the game — done 2026-11-21

- `TEditPanel` (src/pnl_edit.cpp) no longer wraps a Windows EDIT control.
  The control was a real child window, painted by Windows over SDL's frame:
  the chat box flickered between white and black and its text never
  showed. The box now keeps its own text, caret and selection, gets keys
  from the panel system (`key_down_action`, `char_action`), and draws with
  the GDI layer. Its interface is unchanged, so the screens that use it
  (chat, scenario editor, save, player-name and campaign dialogs) did not
  change.
- Kept from the control: the length limit and `verify_char`'s character
  checks per format; Enter and Escape to the parent panel, with Escape
  undoing the edit when the parent does not take it; Tab to the next panel;
  select-all on focus for `auto_sel` boxes; word wrap and Enter for
  `FormatMultiLine`; arrows (Ctrl for words), Home/End, Shift to select,
  Delete, Backspace, Ctrl+A/C/X/V/Z and Shift+Insert/Ctrl+Insert/Shift+Delete;
  a blinking caret; a click places the caret. Not kept: IME (the
  `imc` field is gone; `enable_ime` is still stored).
- The clipboard is SDL's (`surface_get_clipboard_text`,
  `surface_set_clipboard_text` in window.cpp), converted between the
  game's Windows-1252 text and SDL's UTF-8.
- `TEditPanel` is larger than the original 0x16C bytes now; nothing
  depended on that size. `surface_move_child_window` is gone with its last
  caller.
- Checked under Wine: in-game chat shows its text as typed, with no flicker;
  Backspace, Home, typing at the caret, End, Shift+Home selection and Enter
  (the line appears on screen) work. In the scenario editor, the Messages
  box takes several lines, wraps a long one and edits with Up/Home; the
  Save Scenario name box takes text and rejects `*`. Not checked: the
  clipboard keys, and the multiplayer and campaign dialogs.
- Found while testing: an original off-by-one in
  `RGE_Random_Map_Module::randomize_stack` (bucket index 100 of 100)
  overwrote the return address in the clang build, an intermittent crash
  while generating a map. Fixed in src/rmm_base.cpp.

### Sound without WINMM — done 2026-11-21

- Timer: Dsutil.c's stream refill timer (`timeSetEvent`, periodic, 50 ms)
  runs on SDL's timer thread (`surface_time_set_event`,
  `surface_time_kill_event` in port/sdl/winmm.cpp). `timeBeginPeriod` and
  `timeEndPeriod` are gone; SDL's timer needs no resolution request.
- WAV files: the `mmio*` calls in Dsutil.c go to `surface_mmio_*`
  (port/sdl/winmm.cpp), a small buffered RIFF reader and writer over
  stdio: open, read, write, seek, chunk descend (plain, `FINDCHUNK`,
  `FINDRIFF`, `FINDLIST`), ascend (fixing the size of a chunk being
  written), create, and the direct buffer access of
  `mmioGetInfo`/`mmioAdvance`/`mmioSetInfo` that `WaveReadFile` uses.
- Volume: the game set the system mixer's wave-out line (sound volume) and
  its CD or synthesizer line (music volume). These are two volume lines in
  the SDL mixer now (`surface_sound_set_volume`, dsound_memory.cpp): wave
  scales every DirectSound buffer, music scales the music player. The
  game's -10000..0 maps linearly to amplitude, as the mixer control did.
  The game no longer changes the system's volume, so it has nothing to
  restore on exit. The `mixer*` members of `TSound_Driver` and
  `TMusic_System` are gone, with their size checks.
- Music: `mciSendCommandA` in music.cpp and Cdaudio.cpp goes to
  `surface_mci_send_command` (port/sdl/mci.cpp), which plays WAV files
  through the SDL mixer:
  - the `cdaudio` device plays CD tracks ripped to
    `music/trackNN.wav` next to the executable (`track02.wav` is the
    first music track). It opens when one exists. Without any, it fails,
    and the game falls back to MIDI as it did without a music CD;
  - the `sequencer` device plays a WAV rendering of the MIDI file, with
    the same name and `.wav`, beside it or in `music/`. Without one, the
    command goes to the system's MCI, so MIDI music works on Windows as
    before;
  - play (from/to, TMSF or milliseconds), seek, pause, resume, stop,
    close, set time format, and status (track count, position, mode,
    current track) behave as MCI's do. `MM_MCINOTIFY` is posted when a
    play finishes, or (superseded or aborted) when a later command
    interrupts it, so the game's count of pending notifications holds.
  - WAV formats: PCM 8, 16 or 32-bit, and 32-bit float, any rate and
    channel count (converted by `SDL_AudioStream`).
- Fixed: with `STREAM_MUSIC`, music.cpp passed a full path that
  `TSound_Driver::stream_file` prefixed with the sound folder again, so no
  music played. It passes the name only now.
- Remaining WINMM imports: `mciSendCommandA` and `mciGetErrorStringA` (the
  system sequencer fallback, Windows only) and `timeGetTime` (Task 3 of
  the Linux list).
- Checked under Wine with SDL's `disk` audio driver, using test tones
  for the music: with `music/track02.wav` and `track03.wav`, the menu
  plays track 2 once at the music volume, and stops. With no CD tracks
  but `sound/OPEN.wav`, `MUSIC1.wav` and `MUSIC2.wav` beside the MIDI
  files, the menu plays OPEN's rendering. In a game, MUSIC1 and then
  MUSIC2 play, moved on by the notification. MUSIC3, with no rendering,
  goes to Wine's sequencer and stays silent. With `STREAM_MUSIC`, the
  menu streams `open.wav` in full through the Dsutil stream (SDL timer and
  mmio). Game sounds play as before. Not checked: on Windows, the
  sequencer fallback with real MIDI; volume changes from the options
  screen; saved-game music positions.

### Testing under Wine

- The test setup (Xvfb without root, a separate Wine prefix with a fake CD
  drive, screenshot and comparison scripts) lives under `build/sdl_test/`
  and is not committed.
- Games start and play under Wine 10 from a complete retail install
  (checked 2026-11-21). An earlier install copy, taken from installer
  files, let every build reach the menus but stalled on "Starting a new
  game..." (main thread idle, no error); that copy was the cause, not Wine
  or the code. The notes above that say a running game could not be
  checked predate this.
- In-game check of the SDL build (after Task 5), on a random map at
  1024x768: units and the town centre select, a right-click moves a
  villager, the `C` hotkey trains a villager (food and the progress
  percentage update in the panel), arrow keys scroll, and F10 opens the
  game menu. The plain clang build does the same. Maps and civilizations
  differ between runs despite `RANDOMGAME`/`RANDOMMAP`, so these screens
  were compared by eye. With SDL's `disk` audio driver, the recording holds
  the menu clicks and the in-game unit and building sounds.
- At 800x600 (the default `Screen Size`), Wine scales the plain build's
  emulated display mode up to the whole screen, while the SDL build draws
  an 800x600 window. Pixel comparisons therefore use 1024x768.
