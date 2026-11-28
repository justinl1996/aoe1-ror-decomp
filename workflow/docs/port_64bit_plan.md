# 64-Bit Cross-Platform Porting Plan

## Goal
Port **Age of Empires: The Rise of Rome** (`aoe1-ror-decomp`) to native **64-bit** across all platforms:
- **Linux** (`x86_64`, `aarch64`)
- **Windows** (`x64`, `arm64`)
- **macOS** (`arm64` Apple Silicon, `x86_64`)
- **WebAssembly** (`wasm32` preserved + optional `wasm64` / `-sMEMORY64=1`)

The port must preserve **100% binary compatibility** with retail 32-bit little-endian game data (`empires.dat`, `.drs`, `.slp`, `.shp`, `.scx`, `.cpx`, `.gam` savegames, `language.dll`).

---

## Codebase Audit: Quantified 64-Bit Blockers

Direct scan of `src/`, `include/`, and `port/` on branch `hd`:

| Hazard Category | Scope | Why It Breaks on 64-Bit |
|---|---|---|
| **LP64 vs LLP64 `long` mismatch** | **1,113** `rge_read`/`rge_write` 4-byte sites + `Res_file.h`, `Shape.h`, `bitmap_info.h`, `port/asm/` | `sizeof(long)` is **8 bytes** on Linux/macOS 64-bit (LP64) vs **4 bytes** on Windows x64 (LLP64). Reading 4 bytes into an 8-byte `long` leaves the upper 32 bits garbage, and memory-mapped `.drs`/`.slp`/`.bmp` headers shift every field offset. |
| **Hardcoded 4-byte pointer allocations** | **85** `calloc(n, 4)` / `malloc(n * 4)` sites in `src/` | Allocating `T**` arrays with element size `4` instead of `sizeof(T*)` immediately causes heap buffer overflows on 64-bit (`visible.cpp`, `mst_play.cpp`, `task_lst.cpp`, `Panel_ez.cpp`, `campaign.cpp`, `vis_unit.cpp`, `world.cpp`, etc.). |
| **Pointer ↔ 32-bit integer truncation** | **~1,145** casts + `TPanel::action(..., unsigned long, unsigned long)`, `Res_file.cpp` `int fHandle = (int)CreateFileA(...)`, `pathsys.cpp` `(int)distance < (int)(...)` | On Windows x64 (LLP64), `long`/`unsigned long` is 32-bit; on all 64-bit platforms, `int` is 32-bit. Casting pointers or `HANDLE`s to `int`/`long`/`unsigned long` truncates addresses above 4 GB and crashes. |
| **32-bit struct size assertions** | **383** `typedef char ...[(sizeof(T) == 0x...) ? 1 : -1]` checks in `include/` | Every class with vtables or pointers grows on 64-bit; these 32-bit MSVC layout checks block compilation. |
| **`port/asm/` 32-bit typedefs & state sizes** | `port/asm/Asm_draw.cpp`, `Inf32.cpp`, `Drawarea_asm.cpp` | `typedef unsigned long u32;` becomes 64-bit on LP64; pointer alignment casts use `(unsigned long)dest & 3`; `Inf32.cpp` checks `sizeof(State) <= kStateSize`. |
| **32-bit-only toolchains & headers** | `config/linux.json`, `tools/build_linux.py`, `tools/build_clang.py` | Linux build uses `-m32` + `libsdl2:i386` + 32-bit Wine headers; Windows `clang` build links against 32-bit VC6 `lib/` (no x64 libs in VC6). |
| **ARM64 / Apple Silicon specifics** | All `src/` | `char` is **unsigned** by default on ARM64 (must pass `-fsigned-char`); audit any unaligned struct reads or x86-specific assumptions. |

---

## Phased Implementation Plan

### Phase 1: Portable Fixed-Width Types & Compile-Time Guardrails
1. **Create `include/aoe_types.h`**
   - Include `<stdint.h>` and `<stddef.h>`.
   - Define explicit fixed-width aliases (`i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `isize`, `usize`, `iptr`, `uptr`) and a helper macro `AOE_STATIC_ASSERT(cond, name)`.
   - Define `AOE_32BIT_LAYOUT_ASSERT(expr)` that is active **only** on 32-bit x86 MSVC/clang-cl (`#if defined(_M_IX86) || defined(__i386__)`) — or opt-in via `-DAOE_CHECK_32BIT_LAYOUT` — while keeping `AOE_BINARY_LAYOUT_ASSERT(expr)` **always active** on 64-bit for on-disk binary formats.
2. **Gate the 383 32-bit class-size assertions** (`include/*.h`)
   - Convert in-memory C++ class size checks (vtable/pointer-bearing classes) to `AOE_32BIT_LAYOUT_ASSERT(...)`.
   - Keep **always-on** exact size checks for on-disk binary structs: `resfile_header`, `resfile_type_dir_node`, `resfile_item_dir_node`, `Shape_Header`, `Shape_Info`, `BITMAPINFO256`, `Production_Queue_Record`, etc.

### Phase 2: Binary Format & Serialization Safety (LP64 + LLP64)
3. **Fix memory-mapped & on-disk binary headers**
   - `include/Res_file.h`: change `long`/`unsigned long` in `resfile_header`, `resfile_type_dir_node`, `resfile_item_dir_node` to `int32_t`/`uint32_t`.
   - `include/Shape.h`: change `long`/`unsigned long` in `Shape_Header`, `Shape_Info`, `SHP_*` headers to `int32_t`/`uint32_t`.
   - `include/bitmap_info.h`: ensure `BITMAPINFOHEADER` uses `int32_t`/`uint32_t` (not `long` on LP64).
   - `src/Deflate.c`: verify `uint32_t`/`int32_t` widths for zlib/deflate state.
4. **Fix `rge_read` / `rge_write` LP64 safety (1,113 sites)**
   - Introduce typed inline helpers in `include/rge_fio.h`:
     - `rge_read_i32(int fd, int32_t* out)` / `rge_write_i32(int fd, int32_t val)`
     - `rge_read_u32(int fd, uint32_t* out)` / `rge_write_u32(int fd, uint32_t val)`
     - `rge_read_i16`, `rge_read_u8`, `rge_read_f32`
   - Convert serialized `long`/`unsigned long` fields in `include/*.h` (`RGE_Master_*`, `RGE_Static_Object`, `RGE_Player`, `RGE_World`, `RGE_Scenario`, `TRIBE_*`, `AI*`, `RGE_Map`, `RGE_Sprite`, `RGE_Sound`, `RGE_Campaign`) to `int32_t`/`uint32_t`.
   - Add a compile-time trap (C++ overload `rge_read(int, long*, int) = delete` on LP64) so it is **impossible** for any remaining `rge_read(fd, &long_var, 4)` to compile silently!
   - Audit every `rge_read`/`rge_write` that reads/writes a whole struct (`sizeof(SomeStruct)`) to ensure none contain pointers or `long`s.

### Phase 3: Pointer Size, Allocations & Casts
5. **Fix all 85 hardcoded 4-byte allocations** (`src/*.cpp`)
   - Replace every `calloc(n, 4)` / `malloc(n * 4)` / `realloc(p, n * 4)` on pointer arrays with `sizeof(*ptr)` (e.g., `calloc(n, sizeof(RGE_Sprite*))`).
   - Also audit `memset`/`memcpy` calls using `count * 4` on pointer arrays (e.g., `src/view.cpp`, `src/world.cpp`, `src/visible.cpp`, `src/Panel.cpp`).
6. **Fix pointer-to-integer truncation**
   - Change `TPanel::action(TPanel*, long, unsigned long, unsigned long)` → `TPanel::action(TPanel*, long, uintptr_t, uintptr_t)` across `include/panel.h` and all subclasses (~40 overrides).
   - Fix `Res_file.cpp`: store `HANDLE` (or `void*`) instead of `int fHandle = (int)CreateFileA(...)`; keep `_open()` POSIX fd separate from Win32 `HANDLE`.
   - Fix pointer comparisons/hashes cast to `int`/`long` (e.g., `src/pathsys.cpp:65` `while (distance < DiagionalDistance + 256)` — compare pointers directly!).
   - Audit `WPARAM`, `LPARAM`, `LRESULT`, `HWND`, `HDC`, `HFONT`, `HPALETTE`, `HBITMAP`, `HCURSOR`, `HINSTANCE` across `src/` and `port/` so all casts use `intptr_t`/`uintptr_t`.
   - Audit `TTextPanel` / `TListPanel` / `TButtonPanel` / `ManagedArray` where user data or IDs carry pointers.
7. **Fix `port/asm/` C++ ports**
   - `port/asm/Asm_draw.cpp`, `port/asm/Inf32.cpp`, `port/asm/Drawarea_asm.cpp`:
     - Replace `typedef unsigned long u32;` with `typedef uint32_t u32;`.
     - Replace `(unsigned long)dest & 3` with `(uintptr_t)dest & 3`.
     - Update `kStateSize` in `port/asm/Inf32.cpp` (or make `State` allocation dynamic / sized with `sizeof(void*)`).
   - Re-verify `port/asm/` against the original assembly oracle via `make harness` (32-bit reference) + add a 64-bit unit test for `Asm_draw`, `Inf32`, and `Drawarea_asm`.

### Phase 4: Platform Abstraction Layer (`port/`)
8. **Decouple from 32-bit Wine headers (Portable Win32 Shim)**
   - Currently `port/linux/` pulls in 32-bit Wine Win32 headers (`config/linux.json`), which ties Linux to `i386` packages and makes macOS/ARM64 awkward.
   - Either:
     - **Option A (Recommended — cleanest for Linux x86_64/arm64 + macOS + BSD + Web):** Replace the Wine header dependency with a small, self-contained `port/win32_compat/` header set (~500–800 lines covering the exact subset of `windows.h`, `ddraw.h`, `dsound.h`, `mmsystem.h`, `winsock.h` the game actually uses, built strictly on `<stdint.h>` where `DWORD = uint32_t`, `LONG = int32_t`, `WPARAM = uintptr_t`, `LPARAM = intptr_t`, `HANDLE = void*`).
     - **Option B:** Use 64-bit Wine headers on Linux/macOS and Windows SDK x64 on Windows.
9. **Audit `port/sdl/` and `port/linux/` for 64-bit**
   - `port/sdl/ddraw_memory.cpp`, `dsound_memory.cpp`, `gdi.cpp`, `mci.cpp`, `surface.cpp`, `window.cpp`, `settings.cpp`, `strings.cpp`, `winmm.cpp`:
     - Verify `DDSURFACEDESC`, `WAVEFORMATEX`, `BITMAPINFOHEADER`, `PALETTEENTRY`, `RECT`, `POINT`, `MSG` field types use `int32_t`/`uint32_t` (not LP64 `long`!).
     - Verify `HANDLE` / `HFONT` / `HDC` / `HPALETTE` / `HRGN` casting uses `uintptr_t`.
   - `port/linux/crt.cpp`, `win32.cpp`, `winsock.cpp`:
     - Verify `_finddata_t` / `_stat` / `time_t` / `off_t` / `SOCKET` types on 64-bit Linux & macOS.
     - Add macOS path handling (`port_base_path`, case-insensitive file lookup already in `crt.cpp` works on both Linux and case-sensitive APFS).

### Phase 5: Cross-Platform 64-Bit Build Systems (`tools/` & `config/`)
10. **Linux 64-bit (`make linux64` / default `make linux`)**
    - Drop `-m32`; compile native `x86_64` or `aarch64` with `-fsigned-char`, `-fno-strict-aliasing`, `-fwrapv`.
    - Link against native 64-bit system `libsdl2-dev` and `libsdl2-ttf-dev` (no `:i386` multiarch packages required!).
11. **Windows 64-bit (`make win64`)**
    - VC6 only ships 32-bit `.lib`s; switch the Windows 64-bit build to **x64** using either:
      - `clang-cl -m64` + `lld-link` against modern Windows SDK x64 + SDL2/SDL2_ttf x64 binaries, **or**
      - `llvm-mingw` / `x86_64-w64-mingw32-clang++` (cross-compiles Windows x64 `.exe` directly from Linux with zero MSVC/Wine dependency!).
    - Drop `dplayx` (DirectPlay is deprecated/unavailable on modern 64-bit Windows) or stub it cleanly just like the Linux/Web builds.
12. **macOS 64-bit (`make macos`)**
    - Compile with Apple Clang (`arm64` / `x86_64` universal) with `-fsigned-char`, linking Homebrew/framework `SDL2` and `SDL2_ttf`.
13. **WebAssembly (`make web` & optional `make web64`)**
    - Keep `wasm32` as the default browser build (best performance & widest browser support); all fixed-width and `sizeof(*ptr)` fixes apply cleanly to it.
    - Add an optional `wasm64` target (`-sMEMORY64=1`) to validate 64-bit Emscripten compilation as well.

---

## Verification Strategy

1. **Compiler-enforced safety (zero silent truncation):**
   - Build all 64-bit targets with `-Werror=pointer-to-int-cast -Werror=int-to-pointer-cast -Werror=shorten-64-to-32` (incrementally per module) plus the deleted `rge_read(int, long*, int)` trap on LP64.
2. **Sanitizers on Linux x86_64 (massive advantage of 64-bit!):**
   - Run **AddressSanitizer + UndefinedBehaviorSanitizer** (`-fsanitize=address,undefined`) on a Linux x86_64 build under Xvfb! ASan immediately catches any remaining 4-byte pointer allocation, out-of-bounds read, or struct mismatch.
3. **Cross-architecture binary compatibility tests:**
   - Load retail `empires.dat`, `Interfac.drs`, `inter_up.drs`, `graphics.drs`, `sounds.drs`, and retail `.scx` scenarios on 64-bit Linux/Windows/macOS.
   - Save a game (`.gam`) on **32-bit** and load it on **64-bit** (and vice versa) — verifying byte-identical savegame round-tripping.
4. **Automated Xvfb smoke + visual suite:**
   - Boot to Main Menu → Single Player → Random Map → play 30s (train units, rally points, pathfinding around obstacles, save & reload, quit) across `720p`/`900p`/`1080p` under ASan with zero errors.

---

## Recommended Execution Order
1. **Phase 1 + Phase 2** (`aoe_types.h`, binary structs, `rge_read`/`rge_write` LP64 safety) — can be landed and tested on existing 32-bit builds first with zero breakage.
2. **Phase 3** (85 `calloc(..., 4)` fixes, `TPanel::action` `uintptr_t`, `Res_file.cpp` `HANDLE`, `port/asm/` `uint32_t`/`uintptr_t`) — still testable on 32-bit builds at every step!
3. **Phase 4 + Phase 5 (Linux x86_64 first)** — bring up `make linux64` with ASan/UBSan and verify full gameplay + save/load under Xvfb.
4. **Windows x64 (`llvm-mingw` / `clang-cl -m64`), macOS (`arm64`), and Web (`wasm32`/`wasm64`)** — once Linux LP64 + ASan is green, Windows LLP64 and macOS ARM64 require minimal platform-specific polish.

## Audit after the port (2026-11-28)

The places the port touched were checked again on the linux64 build, with
the 32-bit build as the reference.

How:

- `sizeof` of all 459 types in `include/` on i686 and x86_64 (347 differ);
- every whole-struct file read and write found by wrapping `rge_read`,
  `rge_write` and `read` in templates that warn on a type whose size
  differs;
- clang's pointer and integer cast warnings over every source;
- a sweep for byte-pointer strides and offsets (`(char*) p + i * 0x..`,
  `p += 0x..`) and `(long*)` casts;
- ASan linux64 runs: a random-map game with selection drags, moves,
  training, scrolling, zoom, the menu and a minute of play; then a save
  of a played game and loading it back.

Fixed (each a raw 32-bit size or `long` width):

- **view.cpp**: the dirty-region comparison of drawing nodes compared
  0x24/0x34 bytes; on 64-bit that stopped before the selection box's far
  corner, so a growing box was not redrawn. Now computed with `offsetof`.
- **zone_map.cpp** (`do_zone_map`) and **map.cpp**
  (`clear_map_view_info`): tiles stepped by 0x18, `RGE_Tile`'s 32-bit
  size. The zone map read past the terrain rules (the crash). Typed now.
- **Deflate.c** (`mem_copy`): 16-byte blocks copied as four
  `unsigned long`, which is 32 bytes on LP64. Every save over-read its
  input and overwrote the compression window. Now `unsigned int`.
- **rmm_obj.cpp** (`to_close`): random-map lands stepped by 0x10, so after
  the first land, object placement measured distances to the wrong
  points. By member now.
- **t_histry.cpp**: history events were saved as the raw struct (16
  bytes on 32-bit, with a pointer at 12). Now written and read as the
  32-bit record.
- **tactical_unit_data.h**: `UnitData::lastTaskTime` is `uint32_t`, so the
  AI's gatherer records keep their 24-byte saved size.
- **taitacmd.cpp**: `placementStateValue.buildItem` (a pointer) is cleared
  after loading; the 304 saved bytes stop short of it on 64-bit.
- **Dsutil.c** (`SndObjDestroy`): `iAlloc` by `offsetof`, not 8.
- **view.cpp**: the drawing-node pools (`DClipInfo_List`) were created
  with an element size of 0x40, `DClipInfo_Node`'s 32-bit size; the node
  is 88 bytes on 64-bit. With many objects on screen, nodes went past the
  end of each block: heap corruption and a crash some minutes into a game.
- **Dib.cpp** (`DibCheckTrans`): line records allocated at 16 bytes each
  (`h << 4`), but a `LINEINFO` is 24 on 64-bit. Starting a campaign
  scenario (its briefing picture) overran the buffer.
- **port/sdl/settings.cpp**: DWORD settings went through an 8-byte
  `unsigned long` with only 4 bytes set, so values written back to
  settings.ini could be wrong. Now 4-byte `unsigned int`.

A second sweep (after the campaign crash) covered every allocation and
every `memset`/`memcpy`/`memmove`/`memcmp` whose size is not written with
`sizeof`, matched against the types whose size differs, and every size or
count passed as a literal to a constructor, `qsort` or `fread`. The rest
are byte buffers, `int` arrays, or types of the same size on both
(`Waypoint`, `BuildingLot`, `OrderEvent`, `NotifyEvent`, `List_Info`,
`VISIBLE_*_REC`).

Checked and left as they are: struct reads of file formats (same layout
on both); `rge_read`/`rge_write` of `long`, `unsigned long` and pointer
arrays (the LP64 overloads in rge_fio.h); the 65 pointer-array
allocations (all sized by `sizeof`); `Shape_Info`, `Blit_Queue_Entry`,
`AIPlayPhase`, `ObjectMemory`, `AttackMemory`, `BuildingLot` strides
(same size on both).

Not fixed, outside the port's scope: flics.cpp (`FrameHead` read whole;
no callers) and com_hand.cpp (multiplayer messages read as `unsigned
long`).

Checked since: a campaign scenario started and played for two
minutes; a random map left running for six. Not checked on 64-bit: the
scenario editor, long AI games, music. Saved games from 64-bit and 32-bit builds were not compared byte
for byte.

### WebAssembly 64 (web64)

All 459 types have the same size on wasm64 as on x86_64 Linux (and on
wasm32 as on i686), and `long`, `DWORD`, `LONG` and pointers have the
same widths; the layout fixes above cover web64 unchanged.

WebAssembly adds one hazard: a function declared with one signature and
defined with another links to a stub that traps (`wasm-ld: warning:
function signature mismatch`). On wasm32 `long` and `DWORD` are both
i32, so such declarations matched; on wasm64 `long` is i64. Three local
`extern` declarations disagreed with their definitions: `ICInfo` and
`MCIWndCreateA` (tribegam.cpp: web64 trapped at startup) and
`ds_stream_volume` (Sounddrv.cpp). Each web64 link log should have no
signature-mismatch warnings.

Checked (2026-11-28): web64 in headless Chromium at 1280x720, a random
map with drags, zoom, scrolling, two minutes of play and a save.
