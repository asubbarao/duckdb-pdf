#!/usr/bin/env python3
"""Generate ``unreadable_short_stamp.pdf`` — a loss-run scan with a short
native-text stamp drawn in a font whose ToUnicode map sends every printable
code to a control character.

The stamp reads "Page 1 of 1" at 24pt: eight non-space glyphs, all
unreadable. That is below the 20-glyph minimum of the unreadable-layer check
and above the stamp probe's 16pt font-size gate, so before the all-unreadable
carve-out both detectors missed it and the page read as the stamp alone while
the loss run underneath stayed hidden.

Pillow is the only dependency (``pip install Pillow``).

Regenerate with:

    python3 test/data/gen_unreadable_short_stamp.py

Deterministic: no randomness, so re-running produces the same document.
"""

import io
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
OUT = HERE / "unreadable_short_stamp.pdf"

LINES = [
    "LOSS RUN REPORT",
    "Valuation Date: 01/31/2026",
    "Claim Number Date of Loss Status Paid Reserve Incurred",
    "WC-1001 03/14/2025 Closed 12,500.00 0.00 12,500.00",
    "WC-1002 07/02/2025 Open 4,210.55 9,000.00 13,210.55",
    "Totals 16,710.55 9,000.00 25,710.55",
]

STAMP = "Page 1 of 1"
STAMP_PT = 24

# Control characters other than the whitespace ones (tab, line feed, vertical
# tab, form feed, carriage return), which extraction treats as spacing.
CONTROLS = [c for c in range(1, 32) if c not in (9, 10, 11, 12, 13)]


def _load_font(size):
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size=size)


def render_scan():
    width, height = 1700, 2200
    img = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(img)
    big = _load_font(64)
    font = _load_font(48)
    y = 120
    draw.text((100, y), LINES[0], fill="black", font=big)
    y += 140
    for line in LINES[1:]:
        draw.text((100, y), line, fill="black", font=font)
        y += 90
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=92)
    return buf.getvalue(), img.size


def to_unicode_cmap() -> bytes:
    codes = list(range(0x21, 0x7F))
    entries = [f"<{code:02X}> <{CONTROLS[i % len(CONTROLS)]:04X}>" for i, code in enumerate(codes)]
    blocks = []
    for start in range(0, len(entries), 100):
        chunk = entries[start:start + 100]
        blocks.append(f"{len(chunk)} beginbfchar\n" + "\n".join(chunk) + "\nendbfchar")
    cmap = (
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        + "\n".join(blocks)
        + "\nendcmap\nCMapName currentdict /CMap defineresource pop\nend\nend"
    ).encode("ascii")
    return b"<< /Length " + str(len(cmap)).encode("ascii") + b" >>\nstream\n" + cmap + b"\nendstream"


def make_pdf() -> None:
    jpeg, (width, height) = render_scan()
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << /XObject << /Im0 4 0 R >> /Font << /F1 5 0 R >> >> /Contents 6 0 R >>",
        b"<< /Type /XObject /Subtype /Image /Width %d /Height %d "
        b"/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %d >>\nstream\n"
        % (width, height, len(jpeg)) + jpeg + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding /ToUnicode 7 0 R >>",
        b"<< /Length 0 >>\nstream\n\nendstream",  # placeholder, replaced below
        to_unicode_cmap(),
    ]
    content = b"q 612 0 0 792 0 0 cm /Im0 Do Q\n"
    content += ("BT /F1 %d Tf 230 745 Td (%s) Tj ET\n" % (STAMP_PT, STAMP)).encode()
    objects[5] = b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream"
    output = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for object_id, body in enumerate(objects, 1):
        offsets.append(len(output))
        output.extend(f"{object_id} 0 obj\n".encode("ascii"))
        output.extend(body)
        output.extend(b"\nendobj\n")
    xref_offset = len(output)
    output.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    output.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        output.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    output.extend(
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref_offset}\n%%EOF\n".encode("ascii")
    )
    OUT.write_bytes(output)


if __name__ == "__main__":
    make_pdf()
