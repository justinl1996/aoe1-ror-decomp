# Building with VC6 under Wine (Linux)

This produces a **playable substitute build** of `empiresx.exe` on Linux. It
uses Visual C++ 6.0 under Wine in place of the pinned VC5 toolchain.

VC6 does not reproduce the original code. Its objects and image are never
evidence for function status, `config/functions/`, or matching work. Use the
pinned VC5 toolchain (`config/toolchain.json`) for that.

The commands below were last checked on 2026-11-20: 227/227 sources compiled,
3/3 assembly sources assembled, 0 unresolved symbols, and the image linked.
`tools/build_executable.py --help` is the authoritative command reference.

## Prerequisites

- `wine` on `PATH` (or set `WINE` to the loader). Wine maps `Z:` to `/`.
- A VC6 tree containing `VC98/Bin/CL.EXE` and `Common/MSDev98/Bin/RC.EXE`
  (here: `/home/ubuntu/aoe_web/MSVC600`).
- MASM `ml.exe` 6.15.8803, SHA-256
  `3e4968acb7ae6ee5059e7a5a39dd0a0a25a3abfbe2b0dd5c41199f7d4987b277`
  (here: `/home/ubuntu/aoe_web/ml.exe`). The build refuses any other hash.
- `third_party/directplay/lib/x86/dplayx.lib` and `dxguid.lib`.
- Python 3.12+ with `tools/requirements.txt`.

## One-time setup

```sh
python3 -m venv build/linux-venv
build/linux-venv/bin/pip install -r tools/requirements.txt
```

Both local config files live under `build/`, which git ignores. Recreate them
on a new machine.

`build/toolchain-vc6.json` (`$VC5_DIR` expands to the VC6 root):

```json
{
  "toolchain_name": "vc6-wine-local",
  "status": "substitute",
  "compiler": {
    "cl": "$VC5_DIR\\VC98\\Bin\\CL.EXE",
    "link": "$VC5_DIR\\VC98\\Bin\\LINK.EXE",
    "nmake": "$VC5_DIR\\VC98\\Bin\\NMAKE.EXE",
    "rc": "$VC5_DIR\\Common\\MSDev98\\Bin\\RC.EXE",
    "default_rsp": "config\\flags\\default.rsp"
  },
  "environment": {
    "path_prepend": ["$VC5_DIR\\VC98\\Bin", "$VC5_DIR\\Common\\MSDev98\\Bin"],
    "include": ["$VC5_DIR\\VC98\\Include", "$VC5_DIR\\VC98\\MFC\\Include"],
    "lib": ["$VC5_DIR\\VC98\\Lib", "$VC5_DIR\\VC98\\MFC\\Lib"],
    "set": { "CL": "/DNO_STRICT" }
  },
  "notes": ["Local VC6 substitute for a playable Linux/Wine build. Not the pinned VC5 toolchain."]
}
```

`build/assembler-ml615.json` (`path` is a host path to `ml.exe`):

```json
{
  "path": "/home/ubuntu/aoe_web/ml.exe",
  "sha256": "3e4968acb7ae6ee5059e7a5a39dd0a0a25a3abfbe2b0dd5c41199f7d4987b277",
  "version": "6.15.8803",
  "flags": ["/nologo", "/c", "/coff"]
}
```

## Build

From the repository root:

```sh
export VC5_DIR='Z:\home\ubuntu\aoe_web\MSVC600'   # VC6 root, as Wine sees it
export AOE_TOOLCHAIN=build/toolchain-vc6.json
export AOE_ASSEMBLER=build/assembler-ml615.json

build/linux-venv/bin/python tools/build_executable.py \
    --output-dir build/executable/vc6 --no-icon
```

- Each run writes a new timestamped directory under `build/executable/vc6/`
  containing `empiresx.exe`, `empiresx.map`, `link.log`, and `report.json`.
- Success means the final summary shows `"linked": true` and
  `"unresolved_symbol_count": 0`.
- `--no-icon` skips the program icon, so no retail `EMPIRES.EXE` is needed.
  Drop it once a retail install is in `runtime/` (see `runtime/README.md`).
- Wine noise is silenced with `WINEDEBUG=-all` unless you set `WINEDEBUG`.

## Troubleshooting

- **Unresolved `_ASMDraw_*`, `_Inf32Decode`, `TDrawArea::PtrClear`, ...**: the
  assembly sources did not build. Usually `AOE_ASSEMBLER` is unset or its
  `path` is stale. Check `report.json` → `assembly_sources`.
- **`No module named 'iced_x86'`**: use the venv's Python, not the system one.
- **`Assembler hash differs from the pinned tool`**: the wrong `ml.exe`.
