"""Build the game as a native 64-bit macOS program (arm64 or x86_64) with clang.

This is the macOS port. Like tools/build_linux.py and tools/build_clang.py, it
is a substitute build, never evidence for config/functions/ status.

It compiles the same sources as the Linux 64-bit build (src/, port/asm/,
port/sdl/, port/linux/) for macOS (arm64 or x86_64) with -fsigned-char, using
Wine's Win32 headers (declarations only; nothing of Wine is linked) and
Homebrew/system SDL2 and SDL2_ttf.

Prerequisites on macOS:
  brew install sdl2 sdl2_ttf
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

import build_clang
import build_linux
import fetch_sdl2
from build_executable import source_plan
from common import REPO_ROOT, load_build_modules, repo_path

TREE_ROOTS = build_linux.TREE_ROOTS

COMMON_FLAGS = [
    "-fdeclspec",
    "-fsigned-char",       # arm64 defaults to unsigned char; game expects signed char
    "-fno-strict-aliasing",
    "-fwrapv",
    "-O2", "-g",
    "-w",
]
C_FLAGS = ["-x", "c", "-std=gnu89", "-Wno-return-mismatch", "-Wno-implicit-function-declaration",
           "-Wno-implicit-int", "-Wno-int-conversion", "-Wno-incompatible-pointer-types"]
CXX_FLAGS = ["-std=gnu++98"]


def run(command: list, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in command], cwd=cwd, capture_output=True, text=True)


def detect_homebrew_prefix() -> Path:
    for candidate in ("/opt/homebrew", "/usr/local"):
        if Path(candidate, "include/SDL2").is_dir() or Path(candidate, "lib").is_dir():
            return Path(candidate)
    return Path("/opt/homebrew")


def main() -> int:
    default_arch = "arm64" if platform.machine() == "arm64" else "x86_64"
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), default=default_arch,
                        help="Target macOS architecture (arm64 for Apple Silicon, x86_64 for Intel).")
    parser.add_argument("--output-dir", default="build/macos",
                        help="Output directory under build/. Each run uses a new subdirectory.")
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--homebrew-prefix", default=os.environ.get("HOMEBREW_PREFIX"),
                        help="Homebrew prefix for SDL2 and SDL2_ttf (default: auto-detect /opt/homebrew or /usr/local).")
    parser.add_argument("--wine-include", default=os.environ.get("AOE_WINE_INCLUDE"),
                        help="Wine's Win32 headers (.../wine/windows). Default: pinned package.")
    parser.add_argument("--sanitize", help="Build with a clang sanitizer, e.g. address.")
    parser.add_argument("--compile-only", action="store_true", help="Stop after compiling objects.")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    sdl_config = json.loads((REPO_ROOT / "config/sdl.json").read_text())
    linux_config = json.loads(build_linux.LINUX_CONFIG.read_text())
    sdl_packages = fetch_sdl2.fetch_all()

    wine_include = args.wine_include
    if wine_include is None:
        packages = fetch_sdl2.fetch_all(config=build_linux.LINUX_CONFIG)
        package, relative = linux_config["wine_include"]
        wine_include = str(packages[package] / relative)

    brew = Path(args.homebrew_prefix) if args.homebrew_prefix else detect_homebrew_prefix()

    root = repo_path(args.output_dir).resolve()
    if not root.is_relative_to((REPO_ROOT / "build").resolve()):
        parser.error("Output must be under build/.")
    output = root / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    tree = output / "tree"
    objects_dir = output / "obj"
    objects_dir.mkdir(parents=True)
    report: dict = dict(output=str(output), arch=args.arch, wine_include=wine_include,
                        compiler=run([args.clang, "--version"], output).stdout.splitlines()[:1])

    for name in TREE_ROOTS:
        build_clang.case_insensitive_view(REPO_ROOT / name, tree / name)
    report["include_aliases"] = build_clang.add_include_aliases(tree)
    report["patches"] = build_clang.apply_patches(tree)

    config = json.loads((REPO_ROOT / "config/executable.json").read_text())
    plan = ([entry["source"] for entry in source_plan(config, load_build_modules())] + config["port_sources"]
            + sdl_config["sources"] + linux_config["sources"])

    target = ["-arch", args.arch, *COMMON_FLAGS]
    if args.sanitize:
        target += ["-fsanitize=" + args.sanitize, "-fno-omit-frame-pointer"]
    base = [args.clang, *target,
            "-I", tree / "port/linux/include", "-isystem", wine_include, "-I", tree / "include",
            *["-D" + d for d in linux_config["defines"]]]
    for package, relative in sdl_config["include_dirs"]:
        base += ["-isystem", sdl_packages[package] / relative]
    if (brew / "include/SDL2").is_dir():
        base += ["-isystem", brew / "include/SDL2"]
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
    link = None
    if not failed and not args.compile_only:
        link_args = [args.clang, *target, "-o", exe,
                     *[row["object"] for row in rows if row["compiled"]],
                     f"-L{brew / 'lib'}", f"-Wl,-rpath,{brew / 'lib'}",
                     "-lSDL2", "-lSDL2_ttf", "-lm", "-lpthread"]
        link = run(link_args, output)
        (output / "link.log").write_text(link.stdout + link.stderr)
    report["link"] = dict(returncode=None if link is None else link.returncode,
                          errors=[] if link is None else
                          [l for l in (link.stdout + link.stderr).splitlines() if "error" in l][:40])
    report["linked"] = link is not None and link.returncode == 0 and exe.is_file()
    if report["linked"]:
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
