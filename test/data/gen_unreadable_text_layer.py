#!/usr/bin/env python3
"""Write a loss-run-like page whose text layer cannot be read.

The page is drawn in Helvetica, so it renders as ordinary text, but its font
carries a ToUnicode map that sends every printable code to a control character.
Text extraction then returns control characters, as it does for real carrier
reports drawn with fonts that have no usable Unicode map.
"""

from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "unreadable_text_layer.pdf"

LINES = [
    "LOSS RUN REPORT BY POLICY AND CLAIM",
    "Report Losses as of 11/08/2026",
    "CLAIM NO: R00000001 CLAIM STATUS: Open",
    "MEDICAL $32,068 $15,961",
    "INDEMNITY $37,599 $10,948",
    "TOTAL $79,167 $28,106",
]

# Control characters other than the whitespace ones (tab, line feed, vertical
# tab, form feed, carriage return), which extraction treats as spacing.
CONTROLS = [c for c in range(1, 32) if c not in (9, 10, 11, 12, 13)]


def to_unicode_cmap() -> bytes:
    codes = list(range(0x21, 0x7F))
    entries = [f"<{code:02X}> <{CONTROLS[i % len(CONTROLS)]:04X}>" for i, code in enumerate(codes)]
    blocks = []
    for start in range(0, len(entries), 100):
        chunk = entries[start : start + 100]
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


def page_content() -> bytes:
    ops = ["BT /F1 14 Tf 72 720 Td 20 TL"]
    for line in LINES:
        escaped = line.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        ops.append(f"({escaped}) Tj T*")
    ops.append("ET")
    content = "\n".join(ops).encode("ascii")
    return b"<< /Length " + str(len(content)).encode("ascii") + b" >>\nstream\n" + content + b"\nendstream"


def make_pdf() -> None:
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding /ToUnicode 6 0 R >>",
        page_content(),
        to_unicode_cmap(),
    ]
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
