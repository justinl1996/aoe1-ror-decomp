"""Symbolically run the span codelets of src/Asm_draw.asm.

The drawing engine dispatches each span piece to an unrolled codelet chosen
by piece length and destination alignment. In shadow mode the codelets
combine pixels with the dither mask and value words byte by byte, and the
byte each pixel uses is the byte's position in the register the codelet
built, which is not always the pixel's address modulo 4. This tool runs each
codelet on symbolic bytes and reports, for every pixel, which source pixel,
destination pixel, and mask/value byte it uses.

port/asm/Asm_draw.cpp encodes the result; `--check` verifies the port's
rule against every codelet, and `--dump` prints the raw mappings.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE = REPO_ROOT / "src/Asm_draw.asm"

# Dispatch tables: name -> (operation, mirrored).
TABLES = {
    "plain_copy_table": ("copy", False),
    "solid_fill_table": ("fill", False),
    "L56AB00": ("player", False),
    "L56AF00": ("xlate", False),
    "L56B600": ("copy", True),
    "L56BA00": ("fill", True),
    "L56BE00": ("player", True),
    "L56C200": ("xlate", True),
}
# Set per table while analyzing: mirrored xlate codelets mask, then add.
XLATE_MASK_FIRST = [False]
STATE_SYMBOLS = {"state_88C02C": "M", "state_88C030": "V", "state_88C034": "P"}
REG32 = ("eax", "ebx", "ecx", "edx")


def parse(path: Path = SOURCE):
    """Return (instructions, labels, tables) from the assembly."""
    instructions: list[str] = []
    labels: dict[str, int] = {}
    tables: dict[str, list[str]] = {}
    current_table = None
    for raw in path.read_text().splitlines():
        line = raw.split(";", 1)[0].rstrip()
        if not line.strip():
            continue
        table = re.match(r"^(\w+)\s+DWORD\s+(.*)$", line)
        if table:
            current_table = table[1]
            tables[current_table] = [x.strip() for x in table[2].split(",")]
            continue
        more = re.match(r"^\s+DWORD\s+(.*)$", line)
        if more and current_table:
            tables[current_table] += [x.strip() for x in more[1].split(",")]
            continue
        current_table = None
        label = re.match(r"^(\w+):\s*(.*)$", line)
        if label:
            labels[label[1]] = len(instructions)
            line = label[2]
            if not line.strip():
                continue
        text = line.strip()
        if text.startswith("CODE_FILL"):
            continue
        instructions.append(text)
    return instructions, labels, tables


class Halt(Exception):
    pass


class Machine:
    """A few x86 instructions over symbolic bytes.

    Byte values are tuples: ("S", offset) source pixel, ("D", offset)
    destination pixel, (sym, position) a byte of a state word, ("X",) the
    player color, ("F", position) a byte of the fill word, ("XL", value) an
    xlate lookup, ("and"/"or", a, b), or ("0",).
    """

    def __init__(self, operation: str, length: int, align: int):
        zero = ("0",)
        self.reg = {name: [zero] * 4 for name in REG32}
        self.ecx = length
        self.esi = 0
        self.edi = 0x1000 + align
        self.base = 0x1000 + align
        self.writes: dict[int, tuple] = {}
        self.flags_zero = False
        self.flags_above = False
        if operation == "fill":
            self.reg["eax"] = [("F", i) for i in range(4)]
        elif operation == "player":
            self.reg["ebx"] = [("X",)] * 4

    # -- operands
    def read_mem(self, operand: str, size: int) -> list:
        inner = re.search(r"\[(.*)\]", operand)[1].replace(" ", "")
        if "ds:" in operand:
            name = inner
            if name in STATE_SYMBOLS:
                return [(STATE_SYMBOLS[name], i) for i in range(size)]
            raise ValueError("unexpected state read " + name)
        if inner == "edx+ebx":
            return [("XL", self.reg["edx"][0])]
        match = re.fullmatch(r"(esi|edi)([+-][0-9A-Fa-f]+h?)?", inner)
        if not match:
            raise ValueError("operand " + operand)
        offset = int(match[2].rstrip("h"), 16 if match[2] and match[2].endswith("h") else 10) if match[2] else 0
        if match[1] == "esi":
            return [("S", self.esi + offset + i) for i in range(size)]
        address = self.edi + offset
        return [self.writes.get(address + i, ("D", address + i - self.base)) for i in range(size)]

    def write_mem(self, operand: str, values: list):
        inner = re.search(r"\[(.*)\]", operand)[1].replace(" ", "")
        match = re.fullmatch(r"edi([+-][0-9A-Fa-f]+h?)?", inner)
        if not match:
            raise ValueError("store " + operand)
        offset = int(match[1].rstrip("h"), 16 if match[1].endswith("h") else 10) if match[1] else 0
        for i, value in enumerate(values):
            self.writes[self.edi + offset + i] = value

    def get(self, name: str) -> list:
        if name in REG32:
            return list(self.reg[name])
        if name in ("ax", "bx", "cx", "dx"):
            return self.reg["e" + name][:2]
        if name in ("al", "bl", "cl", "dl"):
            return [self.reg["e" + name[0] + "x"][0]]
        if name in ("ah", "bh", "ch", "dh"):
            return [self.reg["e" + name[0] + "x"][1]]
        raise ValueError("register " + name)

    def set(self, name: str, values: list):
        if name in REG32:
            self.reg[name] = list(values)
        elif name in ("ax", "bx", "dx"):
            self.reg["e" + name][0:2] = values
        elif name in ("al", "bl", "dl"):
            self.reg["e" + name[0] + "x"][0] = values[0]
        elif name in ("ah", "bh", "dh"):
            self.reg["e" + name[0] + "x"][1] = values[0]
        else:
            raise ValueError("register " + name)

    @staticmethod
    def size_of(operand: str) -> int:
        if operand.startswith("byte"):
            return 1
        if operand.startswith("word"):
            return 2
        return 4

    def step(self, text: str, labels: dict[str, int], pc: int) -> int:
        parts = text.split(None, 1)
        op = parts[0]
        args = [a.strip() for a in parts[1].split(",")] if len(parts) > 1 else []
        if op == "jmp":
            if "draw_resume" in args[0]:
                raise Halt
            return labels[args[0].split()[-1]]
        if op in ("ja", "jne", "jbe", "je"):
            taken = {"ja": self.flags_above, "jbe": not self.flags_above,
                     "jne": not self.flags_zero, "je": self.flags_zero}[op]
            return labels[args[0].split()[-1]] if taken else pc + 1
        if op == "mov":
            dest, src = args
            if "[" in dest:
                self.write_mem(dest, self.get(src))
            else:
                size = len(self.get(dest))
                self.set(dest, self.read_mem(src, size) if "[" in src else self.get(src))
        elif op in ("and", "or"):
            dest, src = args
            left = self.get(dest)
            right = self.read_mem(src, len(left)) if "[" in src else self.get(src)
            self.set(dest, [(op, a, b) for a, b in zip(left, right)])
        elif op == "xchg":
            a, b = self.get(args[0]), self.get(args[1])
            self.set(args[0], b)
            self.set(args[1], a)
        elif op == "bswap":
            self.set(args[0], self.get(args[0])[::-1])
        elif op in ("shr", "shl"):
            count = int(args[1].rstrip("h"), 16 if args[1].endswith("h") else 10)
            values = self.get(args[0])
            nbytes = count // 8
            zero = ("0",)
            if op == "shr":
                values = values[nbytes:] + [zero] * nbytes
            else:
                values = [zero] * nbytes + values[:len(values) - nbytes]
            self.set(args[0], values)
        elif op in ("add", "sub", "inc", "dec"):
            target = args[0]
            amount = 1 if op in ("inc", "dec") else int(args[1].rstrip("h"), 16 if args[1].endswith("h") else 10)
            if op in ("sub", "dec"):
                amount = -amount
            if target == "esi":
                self.esi += amount
            elif target == "edi":
                self.edi += amount
            elif target == "ecx":
                old = self.ecx
                self.ecx += amount
                self.flags_zero = self.ecx == 0
                # ja after sub: above means no borrow and not zero (unsigned).
                if amount < 0:
                    self.flags_above = (old & 0xFFFFFFFF) > (-amount) and self.ecx != 0
                else:
                    self.flags_above = self.ecx != 0
            else:
                raise ValueError("arith " + text)
        else:
            raise ValueError("instruction " + text)
        return pc + 1


def run_codelet(instructions, labels, label: str, operation: str, length: int, align: int):
    machine = Machine(operation, length, align)
    pc = labels[label]
    for _ in range(100000):
        try:
            pc = machine.step(instructions[pc], labels, pc)
        except Halt:
            return machine
    raise RuntimeError("codelet did not finish: " + label)


def table_view(tables: dict[str, list[str]], name: str) -> list[str]:
    """A table and every table after it, as the dispatch reads memory.

    solid_fill_table has 128 entries; shadow-mode fills index past it into
    pattern_fill_table.
    """
    names = list(tables)
    flat: list[str] = []
    for other in names[names.index(name):]:
        flat += tables[other]
    return flat


def entry_index(length: int, align: int, shadow: bool) -> int:
    index = (length & 0xFF) | (0x80 if length >= 256 else 0)
    row = index if index < 16 else 16 + (index & 15)
    return row * 4 + align + (128 if shadow else 0)


def match_pixel(operation: str, shadow: bool, value):
    """(source, dest, position) for one written byte, or None.

    Normal-mode fills and copies do not depend on the position; it is None.
    """
    def pos(node, sym):
        return node[1] if node[0] == sym and len(node) == 2 else None

    if value is None:
        return None
    if operation == "fill":
        # Outside shadow mode the fill word holds one byte four times, so
        # the position does not matter.
        if value[0] != "F":
            return None
        return (None, None, value[1] if shadow else None)
    if operation == "xlate":
        if not shadow:
            if value[0] == "XL" and value[1][0] == "D":
                return (None, value[1][1], None)
            return None
        # Normal codelets: and(or(XL(D), V p), M p). Mirrored ones mask
        # first: or(and(XL(D), M p), V p). Each table uses one form.
        outer, inner_sym, outer_sym = ("and", "V", "M") if not XLATE_MASK_FIRST[0] else ("or", "M", "V")
        inner = "or" if outer == "and" else "and"
        if value[0] == outer and value[1][0] == inner and value[1][1][0] == "XL" and value[1][1][1][0] == "D":
            p1, p2 = pos(value[1][2], inner_sym), pos(value[2], outer_sym)
            if p1 is not None and p1 == p2:
                return (None, value[1][1][1][1], p1)
        return None
    if not shadow:
        if operation == "copy" and value[0] == "S":
            return (value[1], None, None)
        if operation == "player" and value[0] == "or" and value[1][0] == "S" and value[2] == ("X",):
            return (value[1][1], None, None)
        return None
    sym = "V" if operation == "copy" else "P"
    if value[0] == "or" and value[1][0] == "and" and value[1][1][0] == "S":
        p1, p2 = pos(value[1][2], "M"), pos(value[2], sym)
        if p1 is not None and p1 == p2:
            return (value[1][1][1], None, p1)
    return None


def natural(operation: str, shadow: bool, mirrored: bool, align: int, j: int):
    dest = -j if mirrored else j
    source = j if operation in ("copy", "player") else None
    position = ((align + dest) & 3) if (shadow or False) else None
    if operation == "fill":
        position = ((align + dest) & 3) if shadow else None
    return (source, dest if operation == "xlate" else None, position)


def codelet_shape(machine: Machine, operation: str, shadow: bool, mirrored: bool, length: int, align: int):
    """Per-pixel triples plus any anomaly that is not about single pixels."""
    problems = []
    triples = []
    for j in range(length):
        address = machine.base - j if mirrored else machine.base + j
        triple = match_pixel(operation, shadow, machine.writes.get(address))
        if triple is None:
            problems.append(f"pixel {j}: {describe(machine.writes.get(address))}")
        triples.append(triple)
    inside = {machine.base - j if mirrored else machine.base + j for j in range(length)}
    for address, value in machine.writes.items():
        if address not in inside and value != ("D", address - machine.base):
            problems.append(f"stray write at {address - machine.base}: {describe(value)}")
    if machine.edi - machine.base != (-length if mirrored else length):
        problems.append(f"edi ends at {machine.edi - machine.base}")
    if operation in ("copy", "player") and machine.esi != length:
        problems.append(f"esi ends at {machine.esi}")
    return triples, problems


def describe(value) -> str:
    if value is None:
        return "-"
    if value[0] in ("and", "or"):
        return f"{value[0]}({describe(value[1])},{describe(value[2])})"
    if len(value) == 1:
        return value[0]
    if isinstance(value[1], tuple):
        return value[0] + "(" + describe(value[1]) + ")"
    return value[0] + str(value[1])


def row_lengths(row: int, limit: int = 400) -> list[int]:
    """Piece lengths that dispatch to a table row."""
    if row < 16:
        return [row]
    return [n for n in range(16, limit) if 16 + (n & 15) == row]


def analyze_shapes(limit: int = 400):
    """For each (table, shadow, row, align): head/tail triples around a natural middle.

    Returns {key: dict(head=[...], tail=[...], label=...)} for entries that
    differ from the natural mapping, and a list of problems that the model
    cannot express.
    """
    instructions, labels, tables = parse()
    shapes, problems = {}, []
    for table, (operation, mirrored) in TABLES.items():
        entries = table_view(tables, table)
        XLATE_MASK_FIRST[0] = mirrored
        for shadow in (False, True):
            for row in range(1, 32):
                for align in range(4):
                    label = entries[row * 4 + align + (128 if shadow else 0)]
                    lengths = row_lengths(row, limit)
                    samples = {}
                    for length in lengths:
                        if label == "draw_done":
                            problems.append(f"{table} shadow={int(shadow)} len={length} align={align}: draw_done")
                            break
                        machine = run_codelet(instructions, labels, label, operation, length, align)
                        triples, issues = codelet_shape(machine, operation, shadow, mirrored, length, align)
                        for issue in issues:
                            problems.append(f"{table} shadow={int(shadow)} len={length} align={align} {label}: {issue}")
                        samples[length] = triples
                    if not samples:
                        continue
                    nat = lambda j: natural(operation, shadow, mirrored, align, j)
                    if row < 16:
                        triples = samples[row]
                        if any(t != nat(j) for j, t in enumerate(triples)):
                            shapes[(table, shadow, row, align)] = dict(head=triples, tail=[], label=label)
                        continue
                    # Long rows: smallest head/tail that explain every sample.
                    shortest = min(samples)
                    found = None
                    for head in range(0, 9):
                        for tail in range(0, 9):
                            if head + tail > shortest:
                                continue
                            ref = samples[shortest]
                            ok = True
                            for length, triples in samples.items():
                                for j in range(length):
                                    if j < head:
                                        want = ref[j]
                                        # head entries are absolute pixels
                                    elif j >= length - tail:
                                        k = j - length  # negative index from the end
                                        want_ref = ref[shortest + k]
                                        # tail positions are relative to the end;
                                        # translate by the length difference.
                                        want = shift_triple(want_ref, length - shortest, operation, mirrored)
                                    else:
                                        want = nat(j)
                                    if triples[j] != want:
                                        ok = False
                                        break
                                if not ok:
                                    break
                            if ok:
                                found = (head, tail)
                                break
                        if found:
                            break
                    if not found:
                        problems.append(f"{table} shadow={int(shadow)} row={row} align={align} {label}: no head/tail fit")
                        continue
                    head, tail = found
                    if head or tail:
                        ref = samples[shortest]
                        shapes[(table, shadow, row, align)] = dict(
                            head=ref[:head], tail=[relative_triple(ref[shortest - tail + i], shortest - tail + i,
                                                                   operation, mirrored, align, shadow)
                                                   for i in range(tail)],
                            label=label, tail_base=shortest - tail)
    return shapes, problems


def shift_triple(triple, delta: int, operation: str, mirrored: bool):
    """A tail triple seen at length L, moved to length L+delta."""
    if triple is None:
        return None
    source, dest, position = triple
    if source is not None:
        source += delta
    if dest is not None:
        dest += -delta if mirrored else delta
    if position is not None:
        position = (position + (-delta if mirrored else delta)) & 3
    return (source, dest, position)


def relative_triple(triple, j: int, operation: str, mirrored: bool, align: int, shadow: bool):
    """Express a tail pixel's triple as offsets from that pixel's natural triple."""
    if triple is None:
        return None
    source, dest, position = triple
    nat_source, nat_dest, nat_position = natural(operation, shadow, mirrored, align, j)
    return (None if source is None else source - nat_source,
            None if dest is None else dest - nat_dest,
            None if position is None else (position - nat_position) & 3)


EMIT_PATH = REPO_ROOT / "port/asm/Asm_draw_codelets.inc"


def emit(shapes) -> str:
    """C++ tables of shadow-mode byte positions for port/asm/Asm_draw.cpp.

    Only positions differ from the natural mapping in shadow mode (plus one
    normal-mode source quirk, which the port codes by hand); this asserts so.
    """
    entries, data = {}, []
    for (table, shadow, row, align), shape in sorted(shapes.items()):
        operation, mirrored = TABLES[table]
        if not shadow:
            if (table, row, align) != ("L56BE00", 7, 3):
                raise ValueError(f"unexpected normal-mode quirk in {table} row {row} align {align}")
            continue
        head, tail = [], []
        for j, (source, dest, position) in enumerate(shape["head"]):
            natural_source, natural_dest, _ = natural(operation, shadow, mirrored, align, j)
            if source != natural_source or dest != natural_dest:
                raise ValueError(f"unexpected source/dest quirk in {table} row {row} align {align}")
            head.append(position)
        for source, dest, position in shape["tail"]:
            if (source or 0) or (dest or 0):
                raise ValueError(f"unexpected tail quirk in {table} row {row} align {align}")
            tail.append(position)
        entries[(list(TABLES).index(table), row, align)] = (len(head), len(tail), len(data))
        data += head + tail
    lines = [
        "// Generated by tools/asm_draw_codelets.py --emit from src/Asm_draw.asm. Do not edit.",
        "//",
        "// Shadow-mode codelets combine each pixel with one byte of the mask and",
        "// value words: the byte at the pixel's position in the register the",
        "// codelet built. For most pixels that is (address & 3). These tables list",
        "// the exceptions, per dispatch table, length row and alignment:",
        "//   head: absolute positions of the first pixels;",
        "//   tail: positions of the last pixels, relative to (address & 3).",
        "// Tables: 0 copy, 1 fill, 2 player copy, 3 xlate, 4-7 the same, mirrored.",
        "struct CodeletShape {",
        "    unsigned char head;",
        "    unsigned char tail;",
        "    unsigned short offset;",
        "};",
        "",
        "static const CodeletShape kShadowShapes[8][32][4] = {",
    ]
    for table in range(8):
        lines.append("    {")
        for row in range(32):
            cells = []
            for align in range(4):
                head, tail, offset = entries.get((table, row, align), (0, 0, 0))
                cells.append("{%d, %d, %d}" % (head, tail, offset))
            lines.append("        {" + ", ".join(cells) + "},")
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("static const unsigned char kShadowPositions[%d] = {" % max(1, len(data)))
    for start in range(0, len(data), 24):
        lines.append("    " + ", ".join(str(x) for x in data[start:start + 24]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dump", action="store_true", help="Print every entry that differs from the natural mapping.")
    parser.add_argument("--emit", action="store_true", help=f"Write {EMIT_PATH.relative_to(REPO_ROOT)}.")
    parser.add_argument("--check", action="store_true",
                        help=f"Fail if {EMIT_PATH.relative_to(REPO_ROOT)} differs from what --emit writes.")
    args = parser.parse_args()
    shapes, problems = analyze_shapes()
    if args.emit or args.check:
        text = emit(shapes)
        if args.emit:
            EMIT_PATH.write_text(text)
            print(f"wrote {EMIT_PATH.relative_to(REPO_ROOT)}")
        elif not EMIT_PATH.is_file() or EMIT_PATH.read_text() != text:
            print(f"{EMIT_PATH.relative_to(REPO_ROOT)} is out of date; run with --emit")
            return 1
    for problem in problems:
        print("PROBLEM", problem)
    if args.dump:
        for key in sorted(shapes):
            table, shadow, row, align = key
            shape = shapes[key]
            print(f"{table} shadow={int(shadow)} row={row} align={align} {shape['label']}: head={shape['head']} tail={shape['tail']}")
    print(f"{len(shapes)} entries differ from the natural mapping; {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
