"""Build a playable empiresx.exe with clang-cl and lld-link, natively on Linux.

This is a substitute build for playing and porting. It is not byte-matching
and never evidence for config/functions/ status; the VC5 build is.

The sources were written for VC5 on a case-insensitive file system, and
keep VC5-specific quirks that the matching build needs. So the tool never
compiles src/ in place. It makes a copy under the output directory:

  - symlinks to src/, include/, port/asm/ and third_party/, with aliases for
    every spelling an #include uses, so case differences resolve;
  - the patches in port/clang/*.patch applied to real copies of the files
    they touch, for constructs clang rejects (see port/clang/README.md).

It compiles every source in the executable's build plan with clang-cl,
including the C++ ports in port/asm/ (config/executable.json port_sources), builds the
DirectPlay import library and the resources with the LLVM tools, and links
with lld-link against VC6's import libraries and C runtime.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

from build_executable import source_plan
from common import REPO_ROOT, load_build_modules, repo_path

PATCH_DIR = REPO_ROOT / "port/clang"
TREE_ROOTS = ("src", "include", "port/asm", "third_party")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)

# Link-time aliases for names clang mangles or chooses differently from VC5.
ALTERNATE_NAMES = {
    # VC6's runtime has no array new/delete; VC5 called the scalar ones.
    "??_U@YAPAXI@Z": "??2@YAPAXI@Z",
    "??_V@YAXPAX@Z": "??3@YAXPAX@Z",
    # Headers declare RGE_Armor_Weapon_Info as both struct and class; clang
    # mangles each caller by the key it saw, VC5 by the first one.
    "?damage@RGE_Static_Object@@UAEXHPAURGE_Armor_Weapon_Info@@MPAVRGE_Player@@PAV1@@Z":
        "?damage@RGE_Static_Object@@UAEXHPAVRGE_Armor_Weapon_Info@@MPAVRGE_Player@@PAV1@@Z",
    "?calculateDamage@RGE_Static_Object@@UAEMHPAURGE_Armor_Weapon_Info@@MPAVRGE_Player@@PAV1@@Z":
        "?calculateDamage@RGE_Static_Object@@UAEMHPAVRGE_Armor_Weapon_Info@@MPAVRGE_Player@@PAV1@@Z",
    # append_line(const char*, long) is declared, never defined: VC5 bound
    # string literals to the char* overload, clang picks the const one.
    "?append_line@TTextPanel@@QAEHPBDJ@Z": "?append_line@TTextPanel@@QAEHPADJ@Z",
}

# VC6 is used for headers, import libraries and the C runtime only.
DEFAULT_VC_DIR = "/home/ubuntu/aoe_web/MSVC600"

COMMON_FLAGS = [
    "--target=i686-pc-windows-msvc",
    "-fms-compatibility-version=12.00",  # VC6 headers; _MSC_VER 1200
    "-fms-extensions",
    "-mno-sse",                          # keep floating point on the x87, as VC5 did
    "/nologo", "/c", "/O2",
    "/GS-",                              # no stack cookies: VC6's runtime has no __security_cookie
    # No /GX: no source throws or catches, and clang's exception tables need
    # a newer __CxxFrameHandler than VC6's runtime has.
    "/Zl",                               # no default-library records; the link names VC6's LIBC
    "/DNO_STRICT",
    "-Wno-nonportable-include-path",
    "-Wno-microsoft-cast",
    "-w",                                # the sources predate most of clang's warnings
]
# Errors by default in modern clang, accepted by C89 compilers.
C_FLAGS = ["/clang:-std=gnu89", "-Wno-return-mismatch", "-Wno-implicit-function-declaration",
           "-Wno-implicit-int", "-Wno-int-conversion", "-Wno-incompatible-pointer-types"]
CXX_FLAGS = ["/clang:-std=gnu++98"]


def case_insensitive_view(source: Path, dest: Path) -> None:
    """Symlink every file under source into dest, also under its lowercase name."""
    for root, _, files in os.walk(source):
        relative = Path(root).relative_to(source)
        for directory in {relative, Path(str(relative).lower())}:
            out = dest / directory
            out.mkdir(parents=True, exist_ok=True)
            for name in files:
                for alias in {name, name.lower()}:
                    link = out / alias
                    if not link.exists() and not link.is_symlink():
                        link.symlink_to(Path(root) / name)


def add_include_aliases(tree: Path) -> int:
    """Alias each spelling an #include uses, relative to the including file."""
    added = 0
    for path in list(tree.rglob("*")):
        if path.suffix.lower() not in (".c", ".cpp", ".h", ".inc") or not path.is_file():
            continue
        text = path.read_text(errors="replace")
        for name in INCLUDE.findall(text):
            target = (path.parent / name)
            if target.exists():
                continue
            parent = target.parent
            if not parent.is_dir():
                continue
            matches = [p for p in parent.iterdir() if p.name.lower() == target.name.lower()]
            if matches:
                target.symlink_to(matches[0].resolve())
                added += 1
    return added


def apply_patches(tree: Path) -> list[str]:
    applied = []
    for patch in sorted(PATCH_DIR.glob("*.patch")):
        text = patch.read_text()
        for name in re.findall(r"^\+\+\+ b/(\S+)", text, re.M):
            target = tree / name
            if target.is_symlink():
                real = target.resolve()
                target.unlink()
                shutil.copy2(real, target)
        result = subprocess.run(["patch", "-p1", "--forward", "--batch", "-d", str(tree), "-i", str(patch)],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"{patch.name} did not apply:\n{result.stdout}{result.stderr}")
        applied.append(patch.name)
    return applied


def vc_view(vc_dir: Path, dest: Path) -> dict[str, Path]:
    vc98 = vc_dir / "VC98"
    paths = {}
    for key, relative in (("include", "Include"), ("mfc_include", "MFC/Include"), ("lib", "Lib")):
        out = dest / relative
        if not out.exists():
            case_insensitive_view(vc98 / relative, out)
        paths[key] = out
    return paths


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in command], cwd=cwd, capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", default="build/clang",
                        help="Output directory under build/. Each run uses a new subdirectory.")
    parser.add_argument("--vc-dir", default=os.environ.get("VC6_DIR", DEFAULT_VC_DIR),
                        help="Visual C++ 6.0 root (with VC98/), for headers and libraries. Env: VC6_DIR.")
    parser.add_argument("--clang-cl", default="clang-cl")
    parser.add_argument("--lld-link", default="lld-link")
    parser.add_argument("--llvm-lib", default="llvm-lib")
    parser.add_argument("--llvm-rc", default="llvm-rc")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    root = repo_path(args.output_dir).resolve()
    if not root.is_relative_to((REPO_ROOT / "build").resolve()):
        parser.error("Output must be under build/.")
    output = root / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    tree = output / "tree"
    objects_dir = output / "obj"
    objects_dir.mkdir(parents=True)
    report: dict = dict(output=str(output), compiler=run([args.clang_cl, "--version"], output).stdout.splitlines()[:1])

    for name in TREE_ROOTS:
        case_insensitive_view(REPO_ROOT / name, tree / name)
    report["include_aliases"] = add_include_aliases(tree)
    report["patches"] = apply_patches(tree)
    vc = vc_view(Path(args.vc_dir), root / "vc98")

    config = json.loads((REPO_ROOT / "config/executable.json").read_text())
    plan = [entry["source"] for entry in source_plan(config, load_build_modules())] + config["port_sources"]
    base = [args.clang_cl, *COMMON_FLAGS, "-imsvc", vc["include"], "-imsvc", vc["mfc_include"],
            "/I" + str(tree / "include")]

    def compile_one(source: str) -> dict:
        path = tree / source
        obj = objects_dir / (source.replace("/", "_") + ".obj")
        flags = C_FLAGS if path.suffix.lower() == ".c" else CXX_FLAGS
        result = run([*base, *flags, "/Fo" + str(obj), str(path)], tree)
        return dict(source=source, object=str(obj), returncode=result.returncode,
                    errors=[line for line in (result.stdout + result.stderr).splitlines() if ": error" in line][:20],
                    compiled=result.returncode == 0 and obj.is_file())

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        rows = list(pool.map(compile_one, plan))
    report["sources"] = rows
    failed = [row for row in rows if not row["compiled"]]
    for row in failed:
        print(f"FAILED {row['source']}")
        for line in row["errors"][:5]:
            print("   ", line.replace(str(tree) + "/", ""))

    # DirectPlay import library and resources.
    # llvm-lib takes .def names as written: add the C prefix that MS LIB adds.
    # The imports are by ordinal, so the name itself is not imported.
    dplayx = output / "dplayx.lib"
    definition = output / "dplayx.def"
    definition.write_text(re.sub(r"^(\s+)(\w+@\d+)", r"\1_\2",
                                 (REPO_ROOT / "config/dplayx.def").read_text(), flags=re.M))
    lib_result = run([args.llvm_lib, "/machine:x86", "/def:" + str(definition), "/out:" + str(dplayx)], output)
    script = tree / "src/empiresx.rc"
    text = script.read_text(encoding="ascii")
    stripped = output / "empiresx.rc"
    stripped.write_text("".join(line for line in text.splitlines(keepends=True)
                                if not re.match(r"^\s*\w+\s+ICON\b", line)), encoding="ascii")
    resource = output / "empiresx.res"
    # /C 1252: the version strings hold Windows-1252 characters.
    rc_result = run([args.llvm_rc, "/C", "1252", "/I", str(tree / "src"), "/I", str(tree / "include"), "/I", str(vc["include"]),
                     "/FO", str(resource), str(stripped)], output)
    report["dplayx"] = dict(returncode=lib_result.returncode, output=lib_result.stdout + lib_result.stderr)
    report["resources"] = dict(returncode=rc_result.returncode, output=rc_result.stdout + rc_result.stderr)

    exe = output / (config["name"] + ".exe")
    # lld-link spells the machine X86; it has no use for /OPT:NOREF here.
    link_args = ["/MACHINE:X86" if f.upper() == "/MACHINE:IX86" else f
                 for f in config["link_flags"] if not f.upper().startswith("/OPT")]
    # MS LINK infers the GUI startup from WinMain; lld-link would pull both.
    # /SAFESEH:NO: VC6's libraries predate SafeSEH, and the original had no table.
    link_args += ["/SAFESEH:NO", "/ENTRY:WinMainCRTStartup", "/INCLUDE:_WinMainCRTStartup",
                  *[f"/ALTERNATENAME:{a}={b}" for a, b in ALTERNATE_NAMES.items()],
                  "/OUT:" + str(exe), "/MAP:" + str(output / "empiresx.map"),
                  "/LIBPATH:" + str(vc["lib"]),
                  *["/LIBPATH:" + str(repo_path(p)) for p in config.get("library_dirs", [])],
                  *[row["object"] for row in rows if row["compiled"]],
                  *config["libraries"], str(dplayx), "libc.lib", "oldnames.lib"]
    if rc_result.returncode == 0:
        link_args.append(str(resource))
    response = output / "link.rsp"
    response.write_text("\n".join(f'"{a}"' if " " in a else a for a in link_args) + "\n")
    link = run([args.lld_link, "@" + str(response)], output)
    (output / "link.log").write_text(link.stdout + link.stderr)
    report["link"] = dict(returncode=link.returncode,
                          errors=[l for l in (link.stdout + link.stderr).splitlines() if "error" in l][:40])
    report["linked"] = link.returncode == 0 and exe.is_file() and not failed
    report["summary"] = dict(sources=len(rows), compiled=len(rows) - len(failed), patches=len(report["patches"]),
                             dplayx_ok=lib_result.returncode == 0, resources_ok=rc_result.returncode == 0,
                             linked=report["linked"])
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for line in report["link"]["errors"][:20]:
        print("   link:", line)
    if rc_result.returncode:
        print("   rc:", report["resources"]["output"][:500])
    print(json.dumps(report["summary"], indent=2))
    print(f"Executable: {exe}" if report["linked"] else f"Report: {output / 'report.json'}")
    return 0 if report["linked"] else 1


if __name__ == "__main__":
    sys.exit(main())
