"""Download and unpack the pinned packages the SDL build uses.

Reads config/sdl.json, downloads each package (SDL2 and SDL2_ttf VC
development packages, Liberation fonts), checks its SHA-256, and unpacks it
into its configured directory under build/. Skips packages already present.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from common import REPO_ROOT, repo_path

CONFIG = REPO_ROOT / "config/sdl.json"


def debian_data(archive: Path, scratch: Path) -> Path:
    """Extract the data.tar.* member of a .deb (an ar archive)."""
    blob = archive.read_bytes()
    offset = 8
    while offset + 60 <= len(blob):
        header = blob[offset:offset + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58].decode().strip())
        start = offset + 60
        if name.startswith("data.tar"):
            out = scratch / name
            out.write_bytes(blob[start:start + size])
            return out
        offset = start + size + (size & 1)
    raise SystemExit(f"{archive} has no data.tar member")


def fetch_package(package: dict, force: bool = False) -> Path:
    target = repo_path(package["directory"])
    if (target / package["probe"]).is_file() and not force:
        return target
    if not target.resolve().is_relative_to((REPO_ROOT / "build").resolve()):
        raise SystemExit(f"{target} is not under build/")
    with tempfile.TemporaryDirectory() as scratch:
        archive = Path(scratch) / "package"
        # Some mirrors refuse Python's default User-Agent.
        request = urllib.request.Request(package["url"], headers={"User-Agent": "aoe-build-fetch/1"})
        with urllib.request.urlopen(request) as response, archive.open("wb") as out:
            shutil.copyfileobj(response, out)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != package["sha256"]:
            raise SystemExit(f"SHA-256 mismatch for {package['url']}: {digest}")
        unpacked = Path(scratch) / "unpacked"
        if archive.read_bytes()[:8] == b"!<arch>\n":
            # A Debian package: an ar archive whose data.tar.* holds the files.
            data = debian_data(archive, Path(scratch))
            with tarfile.open(data) as bundle:
                bundle.extractall(unpacked, filter="data")
        elif zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as bundle:
                bundle.extractall(unpacked)
        else:
            with tarfile.open(archive) as bundle:
                bundle.extractall(unpacked, filter="data")
        roots = [p for p in unpacked.iterdir() if p.is_dir()]
        if len(roots) != 1 or not (roots[0] / package["probe"]).is_file():
            raise SystemExit(f"unexpected layout in {package['url']}")
        if target.exists():
            shutil.rmtree(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(roots[0]), target)
    return target


def fetch_all(force: bool = False, config: Path = CONFIG) -> dict[str, Path]:
    packages = json.loads(config.read_text())["packages"]
    return {name: fetch_package(package, force) for name, package in packages.items()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="Download again even if a package is present.")
    args = parser.parse_args()
    for name, path in fetch_all(args.force).items():
        print(f"{name}: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
