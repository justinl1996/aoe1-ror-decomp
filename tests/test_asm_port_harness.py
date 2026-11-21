"""Tool tests for the assembly-port harness and the codelet analyzer.

These do not build or run the harness program (that needs the VC toolchain
and Wine); tools/asm_port_harness.py does.
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import asm_draw_codelets  # noqa: E402
import asm_port_harness  # noqa: E402


def test_rename_symbols_renames_whole_symbols_only():
    text = "PUBLIC _ASMDraw_HLine\n_ASMDraw_HLine:\n    call _ASMDraw_HLine2\n"
    renamed = asm_port_harness.rename_symbols(text, {"_ASMDraw_HLine": "_REF_ASMDraw_HLine"})
    assert renamed == "PUBLIC _REF_ASMDraw_HLine\n_REF_ASMDraw_HLine:\n    call _ASMDraw_HLine2\n"


def test_rename_symbols_handles_mangled_names_and_requires_presence():
    mangled = "?PtrClear@TDrawArea@@QAEXPAUtagRECT@@H@Z"
    text = f"PUBLIC {mangled}\n{mangled} PROC\n{mangled} ENDP\n"
    renamed = asm_port_harness.rename_symbols(text, {mangled: "@REF_PtrClear@16"})
    assert renamed.count("@REF_PtrClear@16") == 3 and mangled not in renamed
    with pytest.raises(ValueError):
        asm_port_harness.rename_symbols("nothing here", {mangled: "x"})


def test_renames_cover_every_public_symbol():
    root = Path(asm_port_harness.REPO_ROOT)
    for source, renames in asm_port_harness.RENAMES.items():
        publics = {line.split()[1] for line in (root / source).read_text().splitlines()
                   if line.startswith("PUBLIC ")}
        assert publics == set(renames), source


def test_parse_summaries():
    text = "FAIL x case=1\nSUMMARY ASMDraw_HLine cases=10 failures=2 skipped=1\nTOTAL cases=10 failures=2\n"
    assert asm_port_harness.parse_summaries(text) == {
        "ASMDraw_HLine": dict(cases=10, failures=2, skipped=1)}


def test_inf32_streams_round_trip(tmp_path):
    path = tmp_path / "streams.bin"
    count = asm_port_harness.write_inf32_streams(path)
    data = path.read_bytes()
    assert data[:4] == b"INF1" and struct.unpack_from("<I", data, 4)[0] == count
    offset, kinds = 8, set()
    for _ in range(count):
        kind, packed_size, raw_size = struct.unpack_from("<III", data, offset)
        offset += 12
        packed = data[offset:offset + packed_size]
        offset += packed_size
        raw = data[offset:offset + raw_size]
        offset += raw_size
        kinds.add(kind)
        if kind != 100:
            assert zlib.decompress(packed, -15) == raw
    assert offset == len(data)
    assert 100 in kinds and len(kinds) > 2


def test_codelet_table_is_current():
    shapes, problems = asm_draw_codelets.analyze_shapes()
    assert problems == []
    assert asm_draw_codelets.EMIT_PATH.read_text() == asm_draw_codelets.emit(shapes)
