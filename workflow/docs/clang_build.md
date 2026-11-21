# Building with clang (Linux, native)

`tools/build_clang.py` builds a playable `empiresx.exe` with clang-cl and
lld-link running natively on Linux: no Wine, no VC compiler, no assembler.
Like the VC6 build (`vc6_wine_build.md`), it is a substitute for playing and
porting. It is not byte-matching and never evidence for function status.

Last checked 2026-11-21 with clang 21.1.6: 230/230 sources compiled, 1
patch applied, linked. It has not been run or play-tested.

## Needs

- `clang`, `lld` and `llvm` packages (`clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc`).
- A Visual C++ 6.0 tree (`VC98/Include`, `VC98/Lib`): only its headers,
  import libraries and C runtime (`LIBC.LIB`) are used. Pass `--vc-dir` or
  set `VC6_DIR`.
- The Python environment from `vc6_wine_build.md`.

## Build

```sh
make            # or: VC6_DIR=/home/ubuntu/aoe_web/MSVC600 build/linux-venv/bin/python tools/build_clang.py
```

`make help` lists the other targets (VC6 builds, the port harness, tests, play).

Run `tools/build_clang.py --help` for options. Each run writes
`build/clang/<timestamp>/` with `empiresx.exe`, `report.json`, `link.log`,
the patched source copy (`tree/`) and the objects (`obj/`).

## What it does

- Compiles a copy of `src/`, `include/`, `port/asm/` and `third_party/`
  with include-name aliases (the sources assume a case-insensitive file
  system) and the patch in `port/clang/` (see its README).
- Uses the C++ ports in `port/asm/` instead of the three assembly files.
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
