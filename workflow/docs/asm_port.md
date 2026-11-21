# Assembly ports and their differential harness

The three hand-written assembly modules have C++ ports in `port/asm/`:

| Assembly | Port | Entry points |
| --- | --- | --- |
| `src/Asm_draw.asm` | `port/asm/Asm_draw.cpp` | `ASMSet_Shadowing`, `ASMSet_ShadowingEx`, `ASMSet_Surface_Info`, `ASMSet_Color_Xform`, `ASMGet_Color_Xform`, `ASMSet_Xlate_Table`, `ASMGet_Xlate_Table`, `ASMDraw_HLine`, `ASMDraw_Sprite` |
| `src/Drawarea_asm.asm` | `port/asm/Drawarea_asm.cpp` | `TDrawArea::PtrClear`, `TDrawArea::PtrSpanCopy`, `TDrawArea::OverlayMemCopy` |
| `src/Inf32.asm` | `port/asm/Inf32.cpp` | `Inf32BufSize`, `Inf32Decode` |

The ports are behavior ports, not matching source; they never feed
`config/functions/` status. The executable build compiles them
(`port_sources` in `config/executable.json`) and no longer assembles
anything. The assembly in `src/` stays as recovered source, tied to the
original bytes by `tests/test_drawing_engine.py` and
`tests/test_compression_library.py`, and as the reference the harness below
checks the ports against.

## The harness

`tools/asm_port_harness.py` checks each port against the assembly:

1. It copies each `src/*.asm` under `build/` with the public symbols renamed
   to `REF_*` and assembles it with the configured assembler. The
   `__thiscall` `TDrawArea` methods become `__fastcall` functions
   (`@REF_PtrClear@16`, ...): `this` in ECX, an unused EDX, and the same
   callee-popped stack arguments.
2. It compiles the harness program (`tests/asm_port/`) and the ports with
   the configured compiler, links them with the renamed reference objects,
   and runs the program (under Wine off Windows).
3. Each case allocates every buffer the call may touch from one arena,
   runs the reference on it, saves the result, restores the arena, runs the
   port, and compares every arena byte (stray writes included), every
   recorded return value and out-parameter, and the `Lock`/`Unlock` calls.
   A fault on either side is caught and compared too.

Run `python tools/asm_port_harness.py --help` for options (`--only`,
`--cases`, `--seed`). It writes a report under `build/asm_port/`. The
program prints one `SUMMARY` line per function. A case is *skipped* only
when its reference behavior depends on code addresses (below).

The reference objects come from this repository's assembly, which
`tests/test_drawing_engine.py` and `tests/test_compression_library.py` tie
to `orig/empiresx.exe`. The harness itself needs no original executable.

### Inputs

- Drawing engine: random surfaces and span lists (random, one full-width
  span per line, many narrow spans), clip limits, HLine batches, and sprite
  frames generated from every supported command, at random positions,
  normal and mirrored, normal and shadow mode, with the game's own shadow
  masks and arbitrary ones.
- `TDrawArea` methods: random areas, rectangles in and out of range, missing
  draw systems, refused locks, and overlapping source and destination.
- `Inf32Decode`: raw DEFLATE streams the driver writes with Python's zlib
  (stored, fixed, dynamic, sync and full flushes, sizes around the 32 KB
  window), 400 hand-built dynamic blocks with complete, single-symbol,
  incomplete, over-subscribed and deep (9-15 bit) codes, and corrupted and
  truncated copies. Each stream is decoded through a seeded sequence of
  calls with random input and output chunk sizes, both input styles
  (pointer and `rge_fio.cpp`'s offset), and both `final` flag values.

### Checking the checker

On 2026-11-21, deliberately broken ports were caught: each `Inf32` table,
bit-reader, flush and error-count change; each `TDrawArea` clip, lock,
span-row and copy-direction change; and each drawing quirk listed below
when removed from the port. Removing a quirk that the inputs cannot reach
would not be caught, so add inputs before relying on a new quirk.

## Codelet data

The sprite engine draws each span piece with an unrolled codelet chosen by
length and destination alignment. In shadow mode each pixel is combined
with one byte of the mask and value words: the byte's position in the
register the codelet built, which is not always the pixel address modulo 4.

`tools/asm_draw_codelets.py` runs every codelet on symbolic bytes, for every
table, length row and alignment, and checks that each codelet writes only
its pixels, ends where expected, and computes each pixel by the port's
formula. It writes the exceptions to `port/asm/Asm_draw_codelets.inc`
(`--emit`); `tests/test_asm_port_harness.py` fails when the file is stale.

## Assembly behavior the ports keep

Drawing engine (`src/Asm_draw.asm`):

- A clipped piece that runs past a span's right edge (left edge when
  mirrored) is restarted against the span after the remembered one. A
  piece that began beyond the remembered span is then drawn into its own
  span twice; shadow translation (`0B`) applies twice there.
- Shadow-mode byte positions follow the codelets (generated table).
- The mirrored shadow translation masks then adds the value; the normal one
  adds then masks.
- The 7-pixel mirrored player-color codelet at alignment 3 (`L567770`)
  draws source pixel 4 where pixel 6 belongs.
- Clipping a sprite at a nonzero `min_line` restarts its rows at line 0.
- Unclipped rows: `3E` is a no-op and `4E` skips the next command's pixels.
  Mirrored, the two swap and the skip moves right.
- Outline commands (`1E`, `2E`) only skip: nothing enables outlines.

`TDrawArea` methods (`src/Drawarea_asm.asm`): see the file comment in
`port/asm/Drawarea_asm.cpp` (whole-area clip bound, source-conditioned
lock without unlock, DWORD-step copies).

`Inf32Decode` (`src/Inf32.asm`): see the file comment in
`port/asm/Inf32.cpp` (bit-reader fetch timing, window overrun, table
acceptance, symbols 286/287, the input count reported on invalid data).

## Where the ports differ

These reference paths cannot be reproduced and are not generated as inputs:

- `Inf32Decode`, distance symbols 30 and 31 (fixed-code blocks only): the
  assembly reads code addresses as the distance base. The port reports
  invalid data and sets `Inf32_port_address_dependent`; the harness skips
  such cases.
- `ASMDraw_Sprite`, `3E`/`4E` on a clipped row: the assembly continues with a
  span-relative x as the destination pointer and faults on the next write.
  The port ends the row.
- `ASMDraw_Sprite`, `0E` and `5E`-`FE`: `int 3` in the assembly. The port
  ends the row.
- Empty or inverted rectangles in the `TDrawArea` methods, and empty spans:
  the assembly's row and byte counts run away. The port keeps the same
  loops, which the harness does not exercise.

## Building the game

`tools/build_executable.py` and `tools/build_clang.py` link the ports. The
game has been linked this way with VC6 under Wine and with clang; it has not
been play-tested or run under the replay harness.
