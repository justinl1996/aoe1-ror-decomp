"""Build the game for web browsers (WebAssembly) with Emscripten.

The browser port (workflow/docs/emscripten_port.md). Like the other substitute
builds, it is never evidence for config/functions/ status.

It compiles the Linux build's sources (tools/build_linux.py: the clang build's
source tree, port/sdl and port/linux) plus port/web for wasm32 with em++,
against Emscripten's SDL2 and SDL2_ttf ports:

  - Wine's Win32 headers (config/linux.json) are copied into the build tree
    and given the two CPU-specific declarations they lack for wasm32
    (CONTEXT, NtCurrentTeb), which nothing in the game uses;
  - port/web/include comes before port/linux/include, for web-only shims;
  - the output is build/web/<time>/: index.html, empiresx.js, empiresx.wasm
    and empiresx.data (the fonts).

Emscripten comes from emsdk (config/web.json says which version and where).
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
import build_linux
import fetch_sdl2
from build_executable import source_plan
from common import REPO_ROOT, load_build_modules, repo_path

WEB_CONFIG = REPO_ROOT / "config/web.json"
TREE_ROOTS = build_linux.TREE_ROOTS + ("port/web",)

COMMON_FLAGS = [
    "-fdeclspec",          # __declspec(...) in the sources
    "-fsigned-char",
    "-fno-strict-aliasing",
    "-fwrapv",             # signed overflow wraps, as the original's code assumed
    "-w",                  # the sources predate most of clang's warnings
]
C_FLAGS = ["-x", "c", "-std=gnu89", "-Wno-return-mismatch", "-Wno-implicit-function-declaration",
           "-Wno-implicit-int", "-Wno-int-conversion", "-Wno-incompatible-pointer-types"]
CXX_FLAGS = ["-std=gnu++98"]

# Wine's winnt.h stops for CPUs it does not know. The game never uses a
# thread context or the TEB, so wasm32/wasm64 gets placeholders.
WINE_WASM_EDITS = {
    "winnt.h": [
        ("#if !defined(CONTEXT_FULL) && !defined(RC_INVOKED)\n#error You need to define a CONTEXT for your CPU\n",
         "#if (defined(__wasm32__) || defined(__wasm64__)) && !defined(CONTEXT_FULL)\n"
         "/* aoe1-ror-decomp web build: no CPU context (unused by the game). */\n"
         "#define CONTEXT_FULL 0\n"
         "typedef struct _CONTEXT { DWORD ContextFlags; } CONTEXT, *PCONTEXT;\n"
         "#endif\n"
         "#if !defined(CONTEXT_FULL) && !defined(RC_INVOKED)\n#error You need to define a CONTEXT for your CPU\n"),
        ("#elif !defined(RC_INVOKED)\n# error You must define NtCurrentTeb() for your architecture\n",
         "#elif defined(__wasm32__) || defined(__wasm64__)\n"
         "/* aoe1-ror-decomp web build: no TEB (unused by the game). */\n"
         "static FORCEINLINE struct _TEB * WINAPI NtCurrentTeb(void)\n{\n    return 0;\n}\n"
         "#elif !defined(RC_INVOKED)\n# error You must define NtCurrentTeb() for your architecture\n"),
    ],
}


def wine_headers_for_wasm(source: Path, dest: Path) -> None:
    shutil.copytree(source, dest, symlinks=False)
    for name, edits in WINE_WASM_EDITS.items():
        path = dest / name
        text = path.read_text(errors="replace")
        for old, new in edits:
            if text.count(old) != 1:
                raise SystemExit(f"{name}: expected text not found once; Wine's headers changed?")
            text = text.replace(old, new)
        path.write_text(text)


def emscripten_tool(name: str, config: dict) -> str:
    emsdk = os.environ.get("EMSDK") or str(repo_path(config["emsdk_dir"]))
    candidate = Path(emsdk) / "upstream/emscripten" / name
    if candidate.is_file():
        return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    raise SystemExit(f"{name} not found: install emsdk (see config/web.json)")


def run(command: list, cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in command], cwd=cwd, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", default="build/web",
                        help="Output directory under build/. Each run uses a new subdirectory.")
    parser.add_argument("--debug", action="store_true",
                        help="-O0 -g with Emscripten's run-time checks (assertions, SAFE_HEAP=2 without the alignment check: the game reads its data unaligned, as wasm allows, and stack checks).")
    parser.add_argument("--memory64", action="store_true",
                        help="Build for 64-bit WebAssembly (wasm64, -sMEMORY64=1).")
    parser.add_argument("--compile-only", action="store_true", help="Stop after compiling.")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    web_config = json.loads(WEB_CONFIG.read_text())
    linux_config = json.loads(build_linux.LINUX_CONFIG.read_text())
    sdl_config = json.loads((REPO_ROOT / "config/sdl.json").read_text())
    emcc = emscripten_tool("emcc", web_config)
    empp = emscripten_tool("em++", web_config)

    # Wine's headers: the pinned package (the system's may be another version).
    packages = fetch_sdl2.fetch_all(config=build_linux.LINUX_CONFIG)
    package, relative = linux_config["wine_include"]
    wine_source = packages[package] / relative
    sdl_packages = fetch_sdl2.fetch_all()

    root = repo_path(args.output_dir).resolve()
    if not root.is_relative_to((REPO_ROOT / "build").resolve()):
        parser.error("Output must be under build/.")
    output = root / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    tree = output / "tree"
    objects_dir = output / "obj"
    objects_dir.mkdir(parents=True)
    report: dict = dict(output=str(output), compiler=run([empp, "--version"], output).stdout.splitlines()[:1])

    for name in TREE_ROOTS:
        if (REPO_ROOT / name).exists():
            build_clang.case_insensitive_view(REPO_ROOT / name, tree / name)
    report["include_aliases"] = build_clang.add_include_aliases(tree)
    report["patches"] = build_clang.apply_patches(tree)
    wine_include = output / "wine-include"
    wine_headers_for_wasm(wine_source, wine_include)

    config = json.loads((REPO_ROOT / "config/executable.json").read_text())
    plan = ([entry["source"] for entry in source_plan(config, load_build_modules())] + config["port_sources"]
            + sdl_config["sources"] + linux_config["sources"] + web_config["sources"])

    optimize = ["-O0", "-g"] if args.debug else ["-O2"]
    mem64 = ["-sMEMORY64=1"] if args.memory64 else []
    base = [*COMMON_FLAGS, *optimize, *mem64, *web_config["compile_flags"],
            "-I", tree / "port/web/include", "-I", tree / "port/linux/include", "-isystem", wine_include,
            "-I", tree / "include", *["-D" + d for d in web_config["defines"]],
            "-include", tree / "port/linux/include/aoe_linux.h", "-c"]

    def compile_one(source: str) -> dict:
        path = tree / source
        obj = objects_dir / (source.replace("/", "_") + ".o")
        is_c = path.suffix.lower() == ".c"
        result = run([emcc if is_c else empp, *base, *(C_FLAGS if is_c else CXX_FLAGS), "-o", obj, path], tree)
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

    report["linked"] = False
    if not failed and not args.compile_only:
        fonts = output / "fonts"
        fonts.mkdir()
        for package, relative, folder in sdl_config["runtime_files"]:
            if folder == "fonts":
                shutil.copy2(sdl_packages[package] / relative, fonts / Path(relative).name)
        page = output / "index.html"
        link_args = [empp, *optimize, *mem64, *web_config["link_flags"],
                     *(["-sASSERTIONS=2", "-sSAFE_HEAP=2", "-sSTACK_OVERFLOW_CHECK=2"] if args.debug else []),
                     "--preload-file", f"{fonts}@/fonts",
                     *[row["object"] for row in rows], "-o", page]
        shell = REPO_ROOT / "port/web/shell.html"
        if shell.is_file():
            link_args += ["--shell-file", shell]
        link = run(link_args, output)
        (output / "link.log").write_text(link.stdout + link.stderr)
        report["link"] = dict(returncode=link.returncode,
                              errors=[l for l in (link.stdout + link.stderr).splitlines() if "error" in l][:60])
        report["linked"] = link.returncode == 0 and page.is_file()
        for line in report["link"]["errors"][:40]:
            print("   link:", line)
    report["summary"] = dict(sources=len(rows), compiled=len(rows) - len(failed), linked=report["linked"])
    (output / "report.json").write_text(json.dumps(report, indent=2, default=str) + "\n")
    print(json.dumps(report["summary"], indent=2))
    print(f"Page: {output / 'index.html'}" if report["linked"] else f"Report: {output / 'report.json'}")
    return 0 if (report["linked"] or (args.compile_only and not failed)) else 1


if __name__ == "__main__":
    sys.exit(main())
