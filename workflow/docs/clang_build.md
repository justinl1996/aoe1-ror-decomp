# Building with clang (Linux, native)

`tools/build_clang.py` builds a playable `empiresx.exe` with clang-cl and
lld-link running natively on Linux: no Wine, no VC compiler, no assembler.
Like the VC6 build (`vc6_wine_build.md`), it is a substitute for playing and
porting. It is not byte-matching and never evidence for function status.

Its window, input, display and sound go through SDL2, and its text through
SDL2_ttf, instead of Windows' message loop, GDI, DirectDraw and DirectSound
(`sdl_port_tasks.md`). `SDL2.dll`, `SDL2_ttf.dll` and the `fonts/` folder must
sit next to `empiresx.exe`.

Last checked 2026-11-21 with clang 21.1.6: 230/230 sources and the three
SDL sources compiled, 1 patch applied, linked. Played under Wine from a
retail install: menus, the scenario editor and a random-map game.

## Needs

- `clang`, `lld` and `llvm` packages (`clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc`).
- A Visual C++ 6.0 tree (`VC98/Include`, `VC98/Lib`): only its headers,
  import libraries and C runtime (`LIBC.LIB`) are used. Pass `--vc-dir` or
  set `VC6_DIR`.
- The Python environment from `vc6_wine_build.md`.
- Network access on the first build: `tools/fetch_sdl2.py` downloads the
  pinned SDL2 and SDL2_ttf VC development packages and the Liberation fonts
  (`config/sdl.json`) into `build/third_party/` and checks their SHA-256.

## Build

```sh
make            # or: VC6_DIR=/home/ubuntu/aoe_web/MSVC600 build/linux-venv/bin/python tools/build_clang.py
```

`make help` lists the other targets (VC6 builds, the port harness, tests, play).

Run `tools/build_clang.py --help` for options. Each run writes
`build/clang/<timestamp>/` with `empiresx.exe`, `SDL2.dll`, `SDL2_ttf.dll`, `fonts/`, `report.json`,
`link.log`, the patched source copy (`tree/`) and the objects (`obj/`).

## Play

Copy `empiresx.exe`, the two DLLs and `fonts/` into a retail Age of Empires + Rise of
Rome install (`make play` does this for `runtime/`) and run
`empiresx.exe NOSTARTUP GAMECD`. The game fills the screen, scaled; with
`WINDOW` it starts in a window. Alt+Enter switches between the two.

## What it does

- Compiles a copy of `src/`, `include/`, `port/asm/`, `port/sdl/` and `third_party/`
  with include-name aliases (the sources assume a case-insensitive file
  system) and the patch in `port/clang/` (see its README).
- Uses the C++ ports in `port/asm/` instead of the three assembly files.
- Adds `port/sdl/` (the SDL-owned window and message loop, GDI text with
  SDL_ttf, in-memory DirectDraw and DirectSound); links `SDL2.lib` and
  `SDL2_ttf.lib` instead of `ddraw.lib` and `dsound.lib`. `src/` calls into
  `port/sdl/` unconditionally, so this is the only build that links.
- Targets `i686-pc-windows-msvc` with `_MSC_VER` 1200, x87 floating point
  (`-mno-sse`), C++98 and C89, no exception tables and no stack cookies.
- Links against VC6's `LIBC.LIB`, the game's import libraries, and a
  DirectPlay import library built from `config/dplayx.def`.

## Risks

- Floating point and evaluation order differ from VC5, so a clang build
  may not stay in sync with original-compiled builds in replays or
  multiplayer. Check it with the replay harness before trusting it.
- Undefined behavior that VC5 happened to tolerate may surface under
  clang's optimizer.
- The ports in `port/asm/` pass `tools/asm_port_harness.py` when compiled
  by clang too (checked by hand on 2026-11-21: 40,182 cases, 0 failures).
