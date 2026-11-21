"""Download and unpack the pinned SDL2 development package for the SDL build.

Reads config/sdl.json, downloads the SDL2 VC development package, checks its
SHA-256, and unpacks it into the configured directory under build/. Does
nothing if that directory already holds the package.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from common import REPO_ROOT, repo_path

CONFIG = REPO_ROOT / "config/sdl.json"


def sdl_directory() -> Path:
    return repo_path(json.loads(CONFIG.read_text())["sdl2"]["directory"])


def fetch(force: bool = False) -> Path:
    sdl = json.loads(CONFIG.read_text())["sdl2"]
    target = repo_path(sdl["directory"])
    if (target / "include/SDL.h").is_file() and not force:
        return target
    if not target.resolve().is_relative_to((REPO_ROOT / "build").resolve()):
        raise SystemExit(f"{target} is not under build/")
    with tempfile.TemporaryDirectory() as scratch:
        archive = Path(scratch) / "sdl2.zip"
        with urllib.request.urlopen(sdl["url"]) as response, archive.open("wb") as out:
            shutil.copyfileobj(response, out)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != sdl["sha256"]:
            raise SystemExit(f"SHA-256 mismatch for {sdl['url']}: {digest}")
        unpacked = Path(scratch) / "unpacked"
        with zipfile.ZipFile(archive) as package:
            package.extractall(unpacked)
        roots = [p for p in unpacked.iterdir() if p.is_dir()]
        if len(roots) != 1 or not (roots[0] / "include/SDL.h").is_file():
            raise SystemExit("unexpected package layout")
        if target.exists():
            shutil.rmtree(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(roots[0]), target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="Download again even if the package is present.")
    args = parser.parse_args()
    print(fetch(args.force))
    return 0


if __name__ == "__main__":
    sys.exit(main())
