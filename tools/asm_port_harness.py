"""Differential harness: hand-written assembly against its C++ port.

Each assembly module in src/ is copied under build/ with its public symbols
renamed to REF_* names and assembled with the configured assembler. The C++
ports in port/asm/ keep the real names. Both link into one console program,
tests/asm_port/, which feeds the same inputs to each pair inside one shared
memory arena and compares every byte the calls may touch, every return value
and out-parameter, and every Lock/Unlock call.

The reference objects come from this repository's own assembly sources, so
the harness needs no original executable. Off Windows the tools and the test
program run under Wine (see tools/common.py).
"""
from __future__ import annotations

import argparse
import json
import random
import re
import struct
import subprocess
import zlib
from datetime import datetime, timezone
from pathlib import Path

from build_assembly import assemble_source
from common import REPO_ROOT, load_toolchain, repo_path, tool_command, tool_env, win_path

HARNESS_DIR = REPO_ROOT / "tests/asm_port"
PORT_DIR = REPO_ROOT / "port/asm"

# Public symbol -> reference symbol. Drawing and Inf32 entry points are
# __cdecl and keep that convention under a REF_ prefix. The TDrawArea methods
# are __thiscall; their reference copies become __fastcall functions, which
# take `this` in ECX, a dummy EDX, and the same callee-popped stack arguments.
RENAMES = {
    "src/Asm_draw.asm": {
        "_" + name: "_REF_" + name
        for name in ("ASMSet_Shadowing", "ASMSet_ShadowingEx", "ASMSet_Surface_Info",
                     "ASMSet_Color_Xform", "ASMGet_Color_Xform", "ASMSet_Xlate_Table",
                     "ASMGet_Xlate_Table", "ASMDraw_HLine", "ASMDraw_Sprite")
    },
    "src/Drawarea_asm.asm": {
        "?PtrClear@TDrawArea@@QAEXPAUtagRECT@@H@Z": "@REF_PtrClear@16",
        "?PtrSpanCopy@TDrawArea@@QAEXPAV1@JJPAUtagRECT@@PAPAE@Z": "@REF_PtrSpanCopy@28",
        "?OverlayMemCopy@TDrawArea@@QAEXAAUtagRECT@@0HHHH@Z": "@REF_OverlayMemCopy@32",
    },
    "src/Inf32.asm": {"_Inf32BufSize": "_REF_Inf32BufSize", "_Inf32Decode": "_REF_Inf32Decode"},
}

HARNESS_SOURCES = ("harness.cpp", "cases_asm_draw.cpp", "cases_drawarea.cpp", "cases_inf32.cpp")
PORT_SOURCES = ("Asm_draw.cpp", "Drawarea_asm.cpp", "Inf32.cpp")
SUMMARY = re.compile(r"^SUMMARY (\S+) cases=(\d+) failures=(\d+) skipped=(\d+)$")


def rename_symbols(text: str, renames: dict[str, str]) -> str:
    """Rename whole symbols in assembly text; every symbol must be present."""
    for old, new in renames.items():
        pattern = re.compile(r"(?<![\w?@$])" + re.escape(old) + r"(?![\w?@$])")
        text, count = pattern.subn(new, text)
        if not count:
            raise ValueError(f"symbol {old} not found")
    return text


def _sample_data(rng: random.Random, kind: str, size: int) -> bytes:
    if kind == "zeros":
        return bytes(size)
    if kind == "random":
        return rng.randbytes(size)
    if kind == "pattern":
        unit = rng.randbytes(rng.randint(1, 9))
        return (unit * (size // len(unit) + 1))[:size]
    if kind == "skewed":
        # Geometric symbol frequencies give long (9-15 bit) Huffman codes.
        return bytes(min(255, int(rng.expovariate(0.08))) for _ in range(size))
    if kind == "text":
        words = [b"the", b"rise", b"of", b"rome", b"age", b"empires", b"legion", b"villager",
                 b"wonder", b"\n", b"priest", b"scenario", b"0123", b"farm", b"gold"]
        out = bytearray()
        while len(out) < size:
            out += rng.choice(words) + b" "
        return bytes(out[:size])
    # mixed: runs, copies and noise, like saved-game data
    out = bytearray()
    while len(out) < size:
        choice = rng.random()
        if choice < 0.3:
            out += bytes([rng.randrange(256)]) * rng.randint(1, 300)
        elif choice < 0.6 and out:
            start = rng.randrange(len(out))
            out += out[start:start + rng.randint(3, 258)]
        else:
            out += struct.pack("<%dI" % 8, *(rng.randrange(1 << 32) for _ in range(8)))
    return bytes(out[:size])


def _deflate(data: bytes, level: int, strategy: int, mem_level: int, flush_every: int, flush_mode: int) -> bytes:
    compressor = zlib.compressobj(level, zlib.DEFLATED, -15, mem_level, strategy)
    out = bytearray()
    if flush_every:
        for start in range(0, len(data), flush_every):
            out += compressor.compress(data[start:start + flush_every])
            out += compressor.flush(flush_mode)
    else:
        out += compressor.compress(data)
    out += compressor.flush(zlib.Z_FINISH)
    return bytes(out)


class _BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.value = 0
        self.count = 0

    def bits(self, value: int, count: int):
        self.value |= (value & ((1 << count) - 1)) << self.count
        self.count += count
        while self.count >= 8:
            self.out.append(self.value & 0xFF)
            self.value >>= 8
            self.count -= 8

    def code(self, code: int, length: int):
        # Huffman codes are sent most significant bit first.
        self.bits(int(format(code, "0%db" % length)[::-1], 2) if length else 0, length)

    def finish(self) -> bytes:
        if self.count:
            self.out.append(self.value & 0xFF)
        return bytes(self.out)


def _canonical(lengths: list[int]) -> list[int]:
    counts = [0] * 17
    for length in lengths:
        counts[length] += 1
    counts[0] = 0
    code, next_code = 0, [0] * 17
    for bits in range(1, 17):
        code = (code + counts[bits - 1]) << 1
        next_code[bits] = code
    codes = []
    for length in lengths:
        codes.append(next_code[length] if length else 0)
        if length:
            next_code[length] += 1
    return codes


def _random_lengths(rng: random.Random, count: int, used: int, shape: str) -> list[int]:
    """Code lengths for `used` of `count` symbols: complete, or deliberately not."""
    lengths = [0] * count
    symbols = rng.sample(range(count), used)
    if shape == "single":
        lengths[symbols[0]] = rng.randint(1, 15)
        return lengths
    # Build a complete code by splitting leaves, then maybe break it.
    leaves = [1]
    while len(leaves) < used:
        index = rng.randrange(len(leaves))
        if leaves[index] >= 15:
            continue
        depth = leaves.pop(index) + 1
        leaves += [depth, depth]
    for symbol, length in zip(symbols, leaves):
        lengths[symbol] = length
    if shape == "incomplete":
        deepest = max(range(count), key=lambda s: lengths[s])
        lengths[deepest] = 0
    elif shape == "oversubscribed":
        shortest = min((s for s in range(count) if lengths[s]), key=lambda s: lengths[s])
        if lengths[shortest] > 1:
            lengths[shortest] -= 1
        else:
            lengths[rng.choice([s for s in range(count) if not lengths[s]] or [0])] = 1
    return lengths


def _crafted_dynamic_block(rng: random.Random) -> bytes:
    """One final dynamic block with chosen code lengths and random symbols.

    The literal/length and distance codes may be complete, single-symbol,
    incomplete or over-subscribed; data after the header is random bits, so
    the decoder runs into end-of-block, errors, or the end of input.
    """
    shapes = ("complete", "complete", "single", "incomplete", "oversubscribed")
    literal_count = rng.randint(257, 286)
    distance_count = rng.randint(1, 30)
    literal = _random_lengths(rng, literal_count, rng.randint(2, literal_count), rng.choice(shapes))
    distance = _random_lengths(rng, distance_count, rng.randint(1, distance_count),
                               rng.choice(shapes) if distance_count > 1 else "single")
    if rng.random() < 0.7:
        literal[256] = literal[256] or max(1, min(15, max(literal)))
    all_lengths = literal + distance
    # Code-length code: lengths 0..15 sent directly, with a complete code
    # over the symbols used (plus 16/17/18 sometimes).
    used = sorted(set(all_lengths) | ({16, 17, 18} if rng.random() < 0.3 else set()))
    cl_lengths = [0] * 19
    if len(used) == 1:
        cl_lengths[used[0]] = 1
    else:
        leaves = [1]
        while len(leaves) < len(used):
            index = rng.randrange(len(leaves))
            if leaves[index] >= 7:
                continue
            depth = leaves.pop(index) + 1
            leaves += [depth, depth]
        for symbol, length in zip(used, leaves):
            cl_lengths[symbol] = length
    cl_codes = _canonical(cl_lengths)
    order = (16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15)
    code_count = max(4, max(i + 1 for i, symbol in enumerate(order) if cl_lengths[symbol]))
    writer = _BitWriter()
    writer.bits(1, 1)
    writer.bits(2, 2)
    writer.bits(literal_count - 257, 5)
    writer.bits(distance_count - 1, 5)
    writer.bits(code_count - 4, 4)
    for symbol in order[:code_count]:
        writer.bits(cl_lengths[symbol], 3)
    position = 0
    while position < len(all_lengths):
        value = all_lengths[position]
        run = 1
        while position + run < len(all_lengths) and all_lengths[position + run] == value:
            run += 1
        if value == 0 and run >= 11 and cl_lengths[18]:
            take = min(run, 138)
            writer.code(cl_codes[18], cl_lengths[18])
            writer.bits(take - 11, 7)
        elif value == 0 and run >= 3 and cl_lengths[17]:
            take = min(run, 10)
            writer.code(cl_codes[17], cl_lengths[17])
            writer.bits(take - 3, 3)
        elif position and run >= 3 and cl_lengths[16] and all_lengths[position - 1] == value:
            take = min(run, 6)
            writer.code(cl_codes[16], cl_lengths[16])
            writer.bits(take - 3, 2)
        else:
            take = 1
            writer.code(cl_codes[value], cl_lengths[value])
        position += take
    # Symbols: valid codes from the chosen tables where possible.
    literal_codes, distance_codes = _canonical(literal), _canonical(distance)
    for _ in range(rng.randint(0, 400)):
        symbol = rng.choice([s for s in range(literal_count) if literal[s]] or [0])
        if not literal[symbol]:
            break
        writer.code(literal_codes[symbol], literal[symbol])
        if symbol == 256:
            break
        if symbol > 256:
            extra = (0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                     4, 4, 4, 4, 5, 5, 5, 5, 0)[min(symbol - 257, 28)]
            writer.bits(rng.getrandbits(extra) if extra else 0, extra)
            dsym = rng.choice([s for s in range(distance_count) if distance[s]] or [0])
            writer.code(distance_codes[dsym], distance[dsym])
            dextra = max(0, (dsym - 2) // 2) if dsym >= 4 else 0
            writer.bits(rng.getrandbits(dextra) if dextra else 0, dextra)
    for _ in range(rng.randint(0, 8)):
        writer.bits(rng.getrandbits(8), 8)
    return writer.finish()


def write_inf32_streams(path: Path, seed: int = 1998) -> int:
    """Raw DEFLATE streams for the Inf32Decode cases (format in cases_inf32.cpp)."""
    rng = random.Random(seed)
    kinds = ("zeros", "random", "pattern", "skewed", "text", "mixed")
    sizes = (0, 1, 2, 3, 7, 100, 1000, 4096, 32767, 32768, 32769, 40000, 65536, 70000, 150000)
    strategies = (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE, zlib.Z_FIXED)
    streams = []
    for kind_index, kind in enumerate(kinds):
        for size in sizes:
            data = _sample_data(rng, kind, size)
            variants = [(6, zlib.Z_DEFAULT_STRATEGY, 8, 0, 0)]
            for _ in range(3):
                flush_every = rng.choice((0, 0, 0, rng.randint(1, 5000)))
                variants.append((rng.choice((0, 1, 6, 9)), rng.choice(strategies), rng.randint(1, 9),
                                 flush_every, rng.choice((zlib.Z_SYNC_FLUSH, zlib.Z_FULL_FLUSH))))
            for level, strategy, mem_level, flush_every, flush_mode in variants:
                if size > 40000 and rng.random() < 0.5:
                    continue
                packed = _deflate(data, level, strategy, mem_level, flush_every, flush_mode)
                streams.append((kind_index, packed, data))
    # Hand-built dynamic blocks with unusual codes. Their output has no
    # reference text, so raw is empty and the sanity check skips them.
    for _ in range(400):
        streams.append((100, _crafted_dynamic_block(rng), b""))
    with path.open("wb") as handle:
        handle.write(b"INF1" + struct.pack("<I", len(streams)))
        for kind_index, packed, data in streams:
            handle.write(struct.pack("<III", kind_index, len(packed), len(data)))
            handle.write(packed)
            handle.write(data)
    return len(streams)


def run(command: list[str], cwd: Path, env: dict[str, str], log: list[dict], timeout: int = 600):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True,
                            timeout=timeout, errors="replace")
    log.append(dict(command=command, returncode=result.returncode,
                    stdout=result.stdout, stderr=result.stderr))
    return result


def parse_summaries(text: str) -> dict[str, dict[str, int]]:
    rows = {}
    for line in text.splitlines():
        match = SUMMARY.match(line.strip())
        if match:
            rows[match[1]] = dict(cases=int(match[2]), failures=int(match[3]), skipped=int(match[4]))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", default="build/asm_port",
                        help="Output directory under build/. Each run uses a new subdirectory.")
    parser.add_argument("--only", action="append", default=[],
                        help="Run only this function's cases (repeatable), e.g. ASMDraw_Sprite.")
    parser.add_argument("--cases", type=int, help="Cases per function (default: the program's own).")
    parser.add_argument("--seed", type=lambda v: int(v, 0), help="Base random seed.")
    parser.add_argument("--max-print", type=int, default=5,
                        help="Failures printed in detail per function.")
    parser.add_argument("--list", action="store_true", help="List the functions and exit after building.")
    args = parser.parse_args()

    root = repo_path(args.output_dir).resolve()
    if not root.is_relative_to((REPO_ROOT / "build").resolve()):
        parser.error("Output must be under build/.")
    output = root / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output.mkdir(parents=True)
    toolchain = load_toolchain()
    env = tool_env(toolchain)
    log: list[dict] = []
    report: dict = dict(output=str(output), steps=log)

    def fail(stage: str) -> int:
        report["failed_stage"] = stage
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        last = log[-1] if log else {}
        print(last.get("stdout", "") + last.get("stderr", ""))
        print(f"{stage} failed; report: {output / 'report.json'}")
        return 2

    objects: list[Path] = []
    for source_name, renames in RENAMES.items():
        source = REPO_ROOT / source_name
        copy = output / ("ref_" + source.name)
        copy.write_text(rename_symbols(source.read_text(), renames))
        obj = copy.with_suffix(".obj")
        row = assemble_source(copy, obj)
        log.append(row)
        if not row["compiled"]:
            return fail("assemble " + source_name)
        objects.append(obj)

    compile_flags = ["/nologo", "/c", "/O2", "/W3", "/GX",
                     "/I" + win_path(REPO_ROOT / "include"), "/I" + win_path(HARNESS_DIR)]
    for directory, names in ((HARNESS_DIR, HARNESS_SOURCES), (PORT_DIR, PORT_SOURCES)):
        for name in names:
            source = directory / name
            obj = output / (directory.name + "_" + source.with_suffix(".obj").name)
            command = tool_command(toolchain["compiler"]["cl"], *compile_flags,
                                   "/Fo" + win_path(obj), win_path(source))
            if run(command, output, env, log).returncode or not obj.is_file():
                return fail("compile " + str(source.relative_to(REPO_ROOT)))
            objects.append(obj)

    exe = output / "asm_port_harness.exe"
    command = tool_command(toolchain["compiler"]["link"], "/nologo", "/subsystem:console",
                           "/out:" + win_path(exe), *[win_path(o) for o in objects], "kernel32.lib")
    if run(command, output, env, log).returncode or not exe.is_file():
        return fail("link")

    streams = output / "inf32_streams.bin"
    report["inf32_stream_count"] = write_inf32_streams(streams)

    arguments = ["--max-print", str(args.max_print), "--inf32-data", win_path(streams)]
    for name in args.only:
        arguments += ["--only", name]
    if args.cases is not None:
        arguments += ["--cases", str(args.cases)]
    if args.seed is not None:
        arguments += ["--seed", str(args.seed)]
    if args.list:
        arguments = ["--list"]
    result = run(tool_command(exe, *arguments), output, env, log, timeout=3600)
    (output / "harness_output.txt").write_text(result.stdout + result.stderr)
    print(result.stdout, end="")
    if result.stderr.strip():
        print(result.stderr, end="")
    summaries = parse_summaries(result.stdout)
    report.update(returncode=result.returncode, functions=summaries,
                  passed=result.returncode == 0 and bool(summaries)
                  and all(row["failures"] == 0 for row in summaries.values()))
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Report: {output / 'report.json'}")
    if args.list:
        return result.returncode
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
