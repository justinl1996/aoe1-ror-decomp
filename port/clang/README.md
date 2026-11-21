# Clang build patches

`tools/build_clang.py` compiles a copy of the source tree with clang-cl. It
applies these patches to that copy only, never to `src/`, which must stay
compilable by VC5 to the original bytes.

| Patch | Clang error | Change |
| --- | --- | --- |
| `02-dib-memmove` | file-local `extern "C" static memmove` conflicts with the CRT's | rename it `dib_memmove` (it copies backward; the call keeps using it) |

This one stays a patch because it is not neutral for VC5: the function is
registered as matching `0043AB50` under the symbol `_memmove`
(`config/functions/src/Dib.cpp.json`, `workflow/decomp_notes/dib.md`), and
the rename changes that symbol.

Five earlier patches were moved into `src/` on 2026-11-21, after VC5
compiled each file before and after the change to identical code and data
bytes and identical relocations (VC5's numbered internal labels `$L`, `$T`,
`$S`, `$SG` shift, nothing else): a string-literal cast in `Command.cpp`,
the `for` counter in `taiinfmd.cpp`, the `extern "C"` redeclarations of `L`
in `tcommand.cpp` and `restore_mouse_after_paint` in `tribegam.cpp` (VC
keeps the first linkage), and the `void*` literal casts in `world.cpp`. That
check compiles single objects; it is not the project's full exactness audit.

Other differences are handled by flags and link aliases in
`tools/build_clang.py` (see `COMMON_FLAGS`, `C_FLAGS` and `ALTERNATE_NAMES`).

To change a patch, edit the copy under `build/clang/<run>/tree/` and
regenerate it with `diff -u` against `src/` (paths `a/src/...`, `b/src/...`).
