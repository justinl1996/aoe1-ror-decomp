"""Build the game as a native 32-bit Linux program with clang.

This is the Linux port (workflow/docs/linux_build.md). Like tools/build_clang.py
it is a substitute build, never evidence for config/functions/ status.

It compiles the same sources as the clang Windows build, with the same SDL
layer (config/sdl.json), for i686-linux-gnu:

  - the source tree is the clang build's: a case-insensitive symlink view
    with the port/clang patches applied;
  - port/linux/include/aoe_linux.h is included first in every source. It
    takes the Win32 declarations from Wine's headers (config/linux.json;
    nothing of Wine is linked) and the Microsoft C runtime's extras from
    port/linux/include/msvcrt.h;
  - port/linux/*.cpp supply the C runtime extras, the Win32 calls the game
    still makes, and main();
  - the program links against the system's 32-bit SDL2 and SDL2_ttf.

Needs clang and lld, 32-bit glibc and libstdc++ development files (Debian
and Ubuntu: gcc-multilib g++-multilib, or --sysroot), and the 32-bit SDL2
and SDL2_ttf runtime libraries (libsdl2-2.0-0:i386 libsdl2-ttf-2.0-0:i386).
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

LINUX_CONFIG = REPO_ROOT / "config/linux.json"
TREE_ROOTS = build_clang.TREE_ROOTS + ("port/linux",)

COMMON_FLAGS = [
    "--target=i686-linux-gnu",
    "-fdeclspec",          # __declspec(...) in the sources
    "-malign-double",      # doubles 8-byte aligned in structs, as with MSVC
    "-mno-sse",            # x87 floating point, as the original
    "-fno-strict-aliasing",
    "-fwrapv",             # signed overflow wraps, as the original's code assumed
    "-O2", "-g",
    "-w",                  # the sources predate most of clang's warnings
]
C_FLAGS = ["-x", "c", "-std=gnu89", "-Wno-return-mismatch", "-Wno-implicit-function-declaration",
           "-Wno-implicit-int", "-Wno-int-conversion", "-Wno-incompatible-pointer-types"]
CXX_FLAGS = ["-std=gnu++98"]


def run(command: list, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in command], cwd=cwd, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", default="build/linux",
                        help="Output directory under build/. Each run uses a new subdirectory.")
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--sysroot", default=os.environ.get("AOE_LINUX_SYSROOT"),
                        help="A sysroot with 32-bit glibc and libstdc++ development files, when the system has none "
                             "(gcc-multilib). Env: AOE_LINUX_SYSROOT.")
    parser.add_argument("--wine-include", default=os.environ.get("AOE_WINE_INCLUDE"),
                        help="Wine's Win32 headers (…/wine/windows). Default: the system's libwine-dev, else the "
                             "pinned package. Env: AOE_WINE_INCLUDE.")
    parser.add_argument("--library-dir", action="append", default=[],
                        help="Extra folder with the 32-bit SDL2 and SDL2_ttf libraries.")
    parser.add_argument("--sanitize", help="Build with a clang sanitizer, e.g. address (for debugging).")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    sdl_config = json.loads((REPO_ROOT / "config/sdl.json").read_text())
    linux_config = json.loads(LINUX_CONFIG.read_text())
    sdl_packages = fetch_sdl2.fetch_all()

    wine_include = args.wine_include
    if wine_include is None and Path(linux_config["system_wine_include"], "windows.h").is_file():
        wine_include = linux_config["system_wine_include"]
    if wine_include is None:
        packages = fetch_sdl2.fetch_all(config=LINUX_CONFIG)
        package, relative = linux_config["wine_include"]
        wine_include = str(packages[package] / relative)

    root = repo_path(args.output_dir).resolve()
    if not root.is_relative_to((REPO_ROOT / "build").resolve()):
        parser.error("Output must be under build/.")
    output = root / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    tree = output / "tree"
    objects_dir = output / "obj"
    objects_dir.mkdir(parents=True)
    report: dict = dict(output=str(output), wine_include=wine_include,
                        compiler=run([args.clang, "--version"], output).stdout.splitlines()[:1])

    for name in TREE_ROOTS:
        build_clang.case_insensitive_view(REPO_ROOT / name, tree / name)
    report["include_aliases"] = build_clang.add_include_aliases(tree)
    report["patches"] = build_clang.apply_patches(tree)

    config = json.loads((REPO_ROOT / "config/executable.json").read_text())
    plan = ([entry["source"] for entry in source_plan(config, load_build_modules())] + config["port_sources"]
            + sdl_config["sources"] + linux_config["sources"])

    target = [*COMMON_FLAGS]
    if args.sanitize:
        target += ["-fsanitize=" + args.sanitize, "-fno-omit-frame-pointer"]
    if args.sysroot:
        target.append("--sysroot=" + str(Path(args.sysroot).resolve()))
    base = [args.clang, *target,
            "-I", tree / "port/linux/include", "-isystem", wine_include, "-I", tree / "include",
            *["-D" + d for d in linux_config["defines"]]]
    for package, relative in sdl_config["include_dirs"]:
        base += ["-isystem", sdl_packages[package] / relative]
    base += ["-include", tree / "port/linux/include/aoe_linux.h", "-c"]

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

    exe = output / config["name"]
    link_args = [args.clang, *target, "-fuse-ld=lld", "-o", exe,
                 *[row["object"] for row in rows if row["compiled"]],
                 *[f"-L{d}" for d in args.library_dir],
                 "-Wl,-rpath,$ORIGIN", *linux_config["libraries"]]
    link = run(link_args, output) if not failed else None
    if link is not None:
        (output / "link.log").write_text(link.stdout + link.stderr)
    report["link"] = dict(returncode=None if link is None else link.returncode,
                          errors=[] if link is None else
                          [l for l in (link.stdout + link.stderr).splitlines() if "error" in l][:40])
    report["linked"] = link is not None and link.returncode == 0 and exe.is_file()
    if report["linked"]:
        for package, relative, folder in sdl_config["runtime_files"]:
            if folder:  # the fonts; the DLLs are Windows'
                (output / folder).mkdir(parents=True, exist_ok=True)
                shutil.copy2(sdl_packages[package] / relative, output / folder / Path(relative).name)
    report["summary"] = dict(sources=len(rows), compiled=len(rows) - len(failed), linked=report["linked"])
    (output / "report.json").write_text(json.dumps(report, indent=2, default=str) + "\n")
    for line in report["link"]["errors"][:20]:
        print("   link:", line)
    print(json.dumps(report["summary"], indent=2))
    print(f"Executable: {exe}" if report["linked"] else f"Report: {output / 'report.json'}")
    return 0 if report["linked"] else 1


if __name__ == "__main__":
    sys.exit(main())
