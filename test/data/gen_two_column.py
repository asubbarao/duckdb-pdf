#!/usr/bin/env python3
"""Generate test/data/two_column.pdf for the layout-engine tests.

Builds a one-page A4 PDF by hand (same minimal classic-xref assembly as
gen_blank_split.py — no external PDF library dependency) laid out the way the
DuckDB Friendly SQL Calendar is, because that document is what exposed the bug
this fixture guards:

    x 40..270                              x 330..548
    +--------------------------+  gutter   +--------------------------------+
    | prose, one sentence per  |   60pt    | MON TUE WED THU FRI SAT SUN    |
    | line, left column        |           |             01  02  03  04     |
    |                          |           | 05  06  07  08  09  10  11     |
    +--------------------------+           +--------------------------------+

Two things have to hold at once, and they pull in opposite directions:

  * the 60pt corridor between prose and grid IS a column boundary, so the
    prose must not interleave with the grid;
  * the ~23pt gaps between the seven weekday columns are NOT column
    boundaries, so the grid rows must stay whole.

Gap width alone cannot tell those apart (23pt is well over any sane minimum).
What separates them is the width of the columns they would create: the real
split leaves columns 39% and 37% of the page wide, the weekday splits would
leave columns 4% wide. See LAYOUT_BAND_MIN_WIDTH_RATIO in src/pdf_extension.cpp.

The grid deliberately starts on the FOURTH column — the first row reads
"01 02 03 04" beginning under THU — so a reader that drops horizontal position
produces a visibly different, and wrong, answer rather than a subtly different
one. That is exactly how the calendar bug presented: every month appeared to
start on a Monday.

Usage (from repo root):
  python3 test/data/gen_two_column.py
"""

from __future__ import annotations

from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "two_column.pdf"

PAGE_W = 595.276
PAGE_H = 841.89

PROSE_X = 40.0
PROSE_TOP = 760.0
PROSE_LEADING = 14.0
PROSE = [
    "Prefix aliases let you name an expression before you write it.",
    "The alias comes first and the expression follows, which reads",
    "the way the query was thought of rather than the way SQL",
    "normally forces you to write it down.",
]

GRID_X0 = 330.0
GRID_PITCH = 33.0
GRID_TOP = 760.0
GRID_LEADING = 22.0
WEEKDAYS = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"]
# Row 1 starts in column index 3 (THU), the way a month beginning on a Thursday does.
GRID_ROWS = [
    (3, ["01", "02", "03", "04"]),
    (0, ["05", "06", "07", "08", "09", "10", "11"]),
    (0, ["12", "13", "14", "15", "16", "17", "18"]),
]

FONT_SIZE = 9.0


def escape(text: str) -> str:
    return text.replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")


def show(x: float, y: float, text: str) -> str:
    """One absolutely-positioned run. Tm is set per run so the reader has to use
    geometry — there is no single text object whose order encodes the answer."""
    return f"BT /F1 {FONT_SIZE} Tf 1 0 0 1 {x:.2f} {y:.2f} Tm ({escape(text)}) Tj ET\n"


def content_stream() -> bytes:
    ops = []
    y = PROSE_TOP
    for line in PROSE:
        ops.append(show(PROSE_X, y, line))
        y -= PROSE_LEADING

    y = GRID_TOP
    for col, day in enumerate(WEEKDAYS):
        ops.append(show(GRID_X0 + col * GRID_PITCH, y, day))
    y -= GRID_LEADING
    for first_col, cells in GRID_ROWS:
        for offset, cell in enumerate(cells):
            ops.append(show(GRID_X0 + (first_col + offset) * GRID_PITCH, y, cell))
        y -= GRID_LEADING

    return "".join(ops).encode("latin-1")


def build() -> bytes:
    stream = content_stream()
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {PAGE_W} {PAGE_H}] "
            f"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"
        ).encode("latin-1"),
        b"<< /Length " + str(len(stream)).encode("latin-1") + b" >>\nstream\n" + stream + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    ]

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n".encode("latin-1") + body + b"\nendobj\n"

    xref_at = len(out)
    n = len(objects) + 1
    out += f"xref\n0 {n}\n".encode("latin-1")
    out += b"0000000000 65535 f \n"
    for off in offsets[1:]:
        out += f"{off:010d} 00000 n \n".encode("latin-1")
    out += f"trailer\n<< /Size {n} /Root 1 0 R >>\nstartxref\n{xref_at}\n%%EOF\n".encode("latin-1")
    return bytes(out)


if __name__ == "__main__":
    OUT.write_bytes(build())
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")
