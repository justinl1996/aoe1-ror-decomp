"""Build a native 64-bit Windows PE32+ empiresx.exe (x86_64) with clang++ + lld (MinGW-w64).

This is the 64-bit Windows port (x86_64-w64-mingw32). Like tools/build_clang.py
and tools/build_linux.py, it is a substitute build, never evidence for
config/functions/ status.

It compiles the game sources (src/, port/asm/, port/sdl/) for 64-bit Windows
using clang++ targeting x86_64-w64-mingw32 with a MinGW-w64 sysroot (system
/usr/x86_64-w64-mingw32 or build/third_party/mingw64/usr) and links against
the x64 SDL2 and SDL2_ttf import libraries from config/sdl.json.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

import build_clang
import fetch_sdl2
from build_executable import source_plan
from common import REPO_ROOT, load_build_modules, repo_path

TREE_ROOTS = build_clang.TREE_ROOTS + ("port/win64",)

COMMON_FLAGS = [
    "--target=x86_64-w64-mingw32",
    "-fdeclspec",
    "-fsigned-char",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-O2", "-g",
    "-DNO_STRICT",
    "-DWIN32_LEAN_AND_MEAN=0",
    "-w",
]
C_FLAGS = ["-x", "c", "-std=gnu89", "-Wno-return-mismatch", "-Wno-implicit-function-declaration",
           "-Wno-implicit-int", "-Wno-int-conversion", "-Wno-incompatible-pointer-types"]
CXX_FLAGS = ["-std=gnu++98"]


def run(command: list, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in command], cwd=cwd, capture_output=True, text=True)


def find_mingw_sysroot(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).resolve()
    local = REPO_ROOT / "build/third_party/mingw64/usr"
    if (local / "x86_64-w64-mingw32/lib/libkernel32.a").is_file():
        return local.resolve()
    if Path("/usr/x86_64-w64-mingw32/lib/libkernel32.a").is_file():
        return Path("/usr")
    raise SystemExit("MinGW-w64 x86_64 sysroot not found. Install mingw-w64 or pass --sysroot.")


def find_gcc_dir(sysroot: Path) -> Path:
    base = sysroot / "lib/gcc/x86_64-w64-mingw32"
    if base.is_dir():
        for entry in sorted(base.iterdir(), reverse=True):
            if (entry / "libgcc.a").is_file() or (entry / "libstdc++.a").is_file():
                return entry
    raise SystemExit(f"GCC mingw64 runtime dir not found under {base}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", default="build/win64",
                        help="Output directory under build/. Each run uses a new subdirectory.")
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--sysroot", default=os.environ.get("AOE_MINGW64_SYSROOT"),
                        help="MinGW-w64 sysroot prefix (containing x86_64-w64-mingw32/).")
    parser.add_argument("--compile-only", action="store_true", help="Stop after compiling.")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    sysroot = find_mingw_sysroot(args.sysroot)
    gcc_dir = find_gcc_dir(sysroot)
    sdl_config = json.loads((REPO_ROOT / "config/sdl.json").read_text())
    sdl_packages = fetch_sdl2.fetch_all()

    root = repo_path(args.output_dir).resolve()
    if not root.is_relative_to((REPO_ROOT / "build").resolve()):
        parser.error("Output must be under build/.")
    output = root / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    tree = output / "tree"
    objects_dir = output / "obj"
    objects_dir.mkdir(parents=True)
    report: dict = dict(output=str(output), sysroot=str(sysroot),
                        compiler=run([args.clang, "--version"], output).stdout.splitlines()[:1])

    for name in TREE_ROOTS:
        build_clang.case_insensitive_view(REPO_ROOT / name, tree / name)
    report["include_aliases"] = build_clang.add_include_aliases(tree)
    report["patches"] = build_clang.apply_patches(tree)

    config = json.loads((REPO_ROOT / "config/executable.json").read_text())
    plan = ([entry["source"] for entry in source_plan(config, load_build_modules())] + config["port_sources"]
            + sdl_config["sources"] + ["port/win64/dplay_stub.cpp"])

    cxx_inc = gcc_dir / "include/c++"
    mingw_inc = sysroot / "x86_64-w64-mingw32/include"
    mingw_lib = sysroot / "x86_64-w64-mingw32/lib"

    base = [
        args.clang, *COMMON_FLAGS,
        f"--sysroot={sysroot}",
        "-isystem", cxx_inc,
        "-isystem", cxx_inc / "x86_64-w64-mingw32",
        "-isystem", cxx_inc / "backward",
        "-isystem", mingw_inc,
        "-I", tree / "include",
    ]
    for package, relative in sdl_config["include_dirs"]:
        base += ["-isystem", sdl_packages[package] / relative]
    base += ["-c"]

    def compile_one(source: str) -> dict:
        path = tree / source
        obj = objects_dir / (source.replace("/", "_") + ".o")
        flags = C_FLAGS if path.suffix.lower() == ".c" else CXX_FLAGS
        result = run([*base, *flags, "-o", obj, path], tree)
        return dict(source=source, object=str(obj), returncode=result.returncode,
                    errors=[line for line in (result.stdout + result.stderr).splitlines() if "error:" in line][:20],
                    compiled=result.returncode == 0 and obj.is_file())

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        rows = list(pool.map(compile_one, plan))
    report["sources"] = rows
    failed = [row for row in rows if not row["compiled"]]
    for row in failed:
        print(f"FAILED {row['source']}")
        for line in row["errors"][:5]:
            print("   ", line.replace(str(tree) + "/", ""))

    exe = output / (config["name"] + ".exe")
    link = None
    if not failed and not args.compile_only:
        sdl2_lib = sdl_packages["sdl2"] / "lib/x64/SDL2.lib"
        sdl2_ttf_lib = sdl_packages["sdl2_ttf"] / "lib/x64/SDL2_ttf.lib"
        link_args = [
            args.clang, "--target=x86_64-w64-mingw32", f"--sysroot={sysroot}",
            "-fuse-ld=lld", "-static-libgcc", "-static-libstdc++", "-mwindows",
            f"-L{gcc_dir}", f"-L{mingw_lib}",
            "-o", exe,
            *[row["object"] for row in rows if row["compiled"]],
            sdl2_lib, sdl2_ttf_lib,
            "-lwinmm", "-lwsock32", "-lws2_32", "-ladvapi32", "-luser32", "-lgdi32",
            "-lkernel32", "-lole32", "-luuid", "-ldxguid", "-lvfw32", "-lcomdlg32", "-lshell32",
        ]
        link = run(link_args, output)
        (output / "link.log").write_text(link.stdout + link.stderr)
    report["link"] = dict(returncode=None if link is None else link.returncode,
                          errors=[] if link is None else
                          [l for l in (link.stdout + link.stderr).splitlines() if "error" in l][:40])
    report["linked"] = link is not None and link.returncode == 0 and exe.is_file()
    if report["linked"]:
        shutil.copy2(sdl_packages["sdl2"] / "lib/x64/SDL2.dll", output / "SDL2.dll")
        shutil.copy2(sdl_packages["sdl2_ttf"] / "lib/x64/SDL2_ttf.dll", output / "SDL2_ttf.dll")
        for package, relative, folder in sdl_config["runtime_files"]:
            if folder:
                (output / folder).mkdir(parents=True, exist_ok=True)
                shutil.copy2(sdl_packages[package] / relative, output / folder / Path(relative).name)
    report["summary"] = dict(sources=len(rows), compiled=len(rows) - len(failed), linked=report["linked"])
    (output / "report.json").write_text(json.dumps(report, indent=2, default=str) + "\n")
    for line in report["link"]["errors"][:20]:
        print("   link:", line)
    print(json.dumps(report["summary"], indent=2))
    print(f"Executable: {exe}" if report["linked"] else f"Report: {output / 'report.json'}")
    return 0 if (report["linked"] or (args.compile_only and not failed)) else 1


if __name__ == "__main__":
    sys.exit(main())
