#!/usr/bin/env python3
"""Regenerate test/data/redact_secret.pdf — a 3-page classic-xref PDF.

Each page shows plain uncompressed Helvetica text so read_pdf can extract it
without any filter dependency:

  * Page 1: "PAGE ONE PUBLIC HEADER"
  * Page 2: "PAGE TWO" plus the secret string "SECRET-CODE-42" positioned at a
            KNOWN location (72, 600) in PDF points (origin bottom-left) so the
            pdf_redact test can target it with a box.
  * Page 3: "PAGE THREE FOOTER"

Hand-assembled (no pypdf) exactly like gen_incremental_pdf.py so the byte layout
and xref offsets are fully controlled.

Usage (from repo root):
  python3 test/data/gen_redact.py
"""

from __future__ import annotations

from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "redact_secret.pdf"

# The secret lives on page 2 at (72, 600) with a 24pt font. The test redaction
# box covers roughly x=60..320, y=590..628 (points, bottom-left origin).
SECRET = "SECRET-CODE-42"


def content_stream(lines: list[tuple[float, float, int, str]]) -> bytes:
    """Build an uncompressed text content stream.

    lines: list of (x, y, font_size, text) drawn with the /F1 Helvetica font.
    """
    parts = ["BT"]
    for x, y, size, text in lines:
        # Escape parens/backslashes for PDF literal strings.
        esc = text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        parts.append(f"/F1 {size} Tf")
        parts.append(f"1 0 0 1 {x} {y} Tm")
        parts.append(f"({esc}) Tj")
    parts.append("ET")
    return ("\n".join(parts) + "\n").encode("ascii")


def main() -> None:
    # Object numbering:
    #   1 = Pages, 2 = Catalog, 3/5/7 = Page objects,
    #   4/6/8 = Page content streams, 9 = Font, 10 = shared Resources, 11 = Info
    page_contents = [
        content_stream([(72, 700, 18, "PAGE ONE PUBLIC HEADER")]),
        content_stream(
            [
                (72, 700, 18, "PAGE TWO"),
                (72, 600, 24, SECRET),
                (72, 500, 12, "more visible page two text below the secret"),
            ]
        ),
        content_stream([(72, 700, 18, "PAGE THREE FOOTER")]),
    ]

    # We assemble the body objects with placeholder byte offsets recorded as we go.
    objects: dict[int, bytes] = {}

    objects[1] = (
        b"<<\n/Type /Pages\n/Count 3\n/Kids [3 0 R 5 0 R 7 0 R]\n"
        b"/MediaBox [0 0 612 792]\n>>\n"
    )
    objects[2] = b"<<\n/Type /Catalog\n/Pages 1 0 R\n>>\n"

    objects[3] = (
        b"<<\n/Type /Page\n/Parent 1 0 R\n/Contents 4 0 R\n/Resources 10 0 R\n>>\n"
    )
    objects[4] = (
        b"<< /Length "
        + str(len(page_contents[0])).encode("ascii")
        + b" >>\nstream\n"
        + page_contents[0]
        + b"endstream\n"
    )
    objects[5] = (
        b"<<\n/Type /Page\n/Parent 1 0 R\n/Contents 6 0 R\n/Resources 10 0 R\n>>\n"
    )
    objects[6] = (
        b"<< /Length "
        + str(len(page_contents[1])).encode("ascii")
        + b" >>\nstream\n"
        + page_contents[1]
        + b"endstream\n"
    )
    objects[7] = (
        b"<<\n/Type /Page\n/Parent 1 0 R\n/Contents 8 0 R\n/Resources 10 0 R\n>>\n"
    )
    objects[8] = (
        b"<< /Length "
        + str(len(page_contents[2])).encode("ascii")
        + b" >>\nstream\n"
        + page_contents[2]
        + b"endstream\n"
    )
    objects[9] = (
        b"<<\n/Type /Font\n/Subtype /Type1\n/BaseFont /Helvetica\n"
        b"/Encoding /WinAnsiEncoding\n>>\n"
    )
    objects[10] = b"<<\n/Font << /F1 9 0 R >>\n/ProcSet [/PDF /Text]\n>>\n"
    objects[11] = b"<<\n/CreationDate (D:20260101000000Z)\n>>\n"

    header = b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"

    body = bytearray()
    body += header
    offsets: dict[int, int] = {}
    n_objects = len(objects)
    for num in range(1, n_objects + 1):
        offsets[num] = len(body)
        body += f"{num} 0 obj\n".encode("ascii") + objects[num] + b"endobj\n"

    xref_offset = len(body)
    xref = bytearray()
    xref += b"xref\n"
    xref += f"0 {n_objects + 1}\n".encode("ascii")
    xref += b"0000000000 65535 f \n"
    for num in range(1, n_objects + 1):
        xref += f"{offsets[num]:010d} 00000 n \n".encode("ascii")

    trailer = (
        b"trailer\n<<\n/Size "
        + str(n_objects + 1).encode("ascii")
        + b"\n/Root 2 0 R\n/Info 11 0 R\n>>\n"
        + b"startxref\n"
        + str(xref_offset).encode("ascii")
        + b"\n%%EOF\n"
    )

    out = bytes(body) + bytes(xref) + trailer
    OUT.write_bytes(out)
    print(
        f"wrote {OUT} ({len(out)} bytes); xref at {xref_offset}; secret={SECRET!r} on page 2 at (72,600)"
    )


if __name__ == "__main__":
    main()
