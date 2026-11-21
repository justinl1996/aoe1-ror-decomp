# The native Linux build

`make linux` (tools/build_linux.py) builds the game as a 32-bit Linux
program, `build/linux/<time>/empiresx`, with the same SDL layer as the
clang Windows build (workflow/docs/sdl_port_tasks.md). Like the clang
build it is a substitute build for playing and porting, never evidence
for config/functions/ status.

## What it needs

- clang and lld;
- 32-bit glibc and libstdc++ development files (Debian and Ubuntu:
  `gcc-multilib g++-multilib`; or a sysroot with `--sysroot` /
  `AOE_LINUX_SYSROOT`);
- the 32-bit SDL2 and SDL2_ttf runtime libraries
  (`libsdl2-2.0-0:i386 libsdl2-ttf-2.0-0:i386`, after
  `dpkg --add-architecture i386`);
- Wine's Win32 headers: a system `libwine-dev`, else the tool downloads the
  package pinned in config/linux.json. Only the headers are used; nothing
  of Wine is linked or needed at run time.

The SDL headers are the ones config/sdl.json fetches for the Windows build.

## How it is put together

- The source tree is the clang build's (a case-insensitive view of src/,
  include/ and port/ with port/clang's patches).
- `port/linux/include/aoe_linux.h` is included first in every source: it
  takes the Win32 declarations from Wine's headers (with `NO_STRICT`, as
  the Windows build) and the Microsoft C runtime's extras from
  `port/linux/include/msvcrt.h`. `_WIN32` is not defined; the port layer's
  Windows-only code is under `#ifdef _WIN32`, Linux-only code under
  `AOE_LINUX`.
- `port/linux/crt.cpp`: the Microsoft C runtime's extras. Every function
  that takes a file name (including `fopen`, `open`, `unlink`, `rename`,
  `access`, `_stat`, `_findfirst`, `_chdir`) resolves Windows paths:
  backslashes, a drive letter, and letter case (each path component
  matches case-insensitively). `rand`/`srand` are Microsoft's generator
  (values below 0x8000; the same seed gives the same numbers as Windows).
  `_mbs*` are single-byte (the game is English).
- `port/linux/win32.cpp`: the Win32 calls the game still makes. Files and
  file mapping (the DRS archives), Win16 file calls, global and local
  memory, `CompareStringA`, `CharUpperA`, `MulDiv`, `GetTickCount`, the
  memory check, cursors as tokens. Multiplayer (DirectPlay), videos,
  WinHelp, the registry (only the lobby uses it), launching programs and
  the CD check fail cleanly; a line on stderr says so once.
- `port/linux/winsock.cpp`: Winsock fails (no multiplayer).
  `aoe_linux.h` renames `gethostname` and `gethostbyname`: glibc has
  functions of those names with another calling convention, and an
  executable defining them hands its versions to libraries (libxcb calls
  `gethostname`).
- `port/linux/main.cpp`: `main` passes the command line to `WinMain` as one
  string. A crash prints the stack as module+offset lines
  (`llvm-symbolizer --obj=empiresx <offsets>`); `AOE_NO_CRASH_HANDLER=1`
  leaves crashes to a debugger or sanitizer.

Compiler choices: `-malign-double` (doubles 8-byte aligned in structs, as
MSVC), `-mno-sse` (x87, as the original), `-fwrapv`,
`-fno-strict-aliasing`.

## Layout differences

The Itanium C++ ABI used on Linux places a derived class's first members in
its base class's tail padding; MSVC does not. Twelve of the headers'
layout checks therefore hold only for MSVC and are left out on Linux
(`#ifndef AOE_LINUX`, with a note). Code that reached members through
MSVC offsets uses the members now (the AI modules' world and owner chains,
`Visible_Unit_Manager::Update_Unit_Info`). Data read from files is read
into plain structs, whose layout is the same.

## Running

Run it from a retail install (the game expects its files in the current
folder, as on Windows), with the fonts folder next to the executable:

    cp build/linux/<time>/empiresx /path/to/install/empiresx-linux
    cp -r build/linux/<time>/fonts /path/to/install/
    cd /path/to/install && ./empiresx-linux

Settings are in `settings.ini` next to the executable. SIGTERM and SIGINT
close the game as its window's close button does.

## Debugging

`tools/build_linux.py --sanitize address --output-dir build/linux-asan`
builds with AddressSanitizer. Run with `AOE_NO_CRASH_HANDLER=1` and
`ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0` (the game frees
`new[]` arrays of plain types with `delete`, which is harmless).

## Status (2026-11-21)

Checked under Xvfb, from a retail install, with the AddressSanitizer
build and the plain one: the menus (pixel-identical to the clang Windows
build under Wine except for glyph rasterization in the copyright lines,
from a different FreeType); a random-map game (town centre selected,
villager trained, scrolling, chat typed and sent, F10 menu, sounds);
the scenario editor (Messages text typed, saved as a new scenario,
listed, reopened with the text); saving a game and loading it back.

Found and fixed on the way (all also in the Windows build's sources):
original over-reads and overflows that glibc's heap and stack layout
expose (the inflate input size in rge_fio, `position_used`'s ninth entry,
the land after the last in `add_object_module`, zone rules past the
terrain table, `get_string`'s terminator past `name + 4`), self-copies
with `strncpy`, and the raw offsets above.

Not checked: campaigns, the single-player scenario games, the AI over a
long game, music, Wayland, and a real (non-Xvfb) desktop. Not ported:
multiplayer, intro videos, WinHelp. After loading a saved random-map
game the interface used the default panel style instead of the
civilization's; not compared with Windows yet.
