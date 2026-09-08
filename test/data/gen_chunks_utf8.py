#!/usr/bin/env python3
"""Generate test/data/chunks_utf8.pdf for pdf_chunks tiny-budget UTF-8 tests.

Hand-assembled (same classic-xref technique as gen_blank_split.py). Three
pages of whitespace-free runs so pdf_chunks' C2 hard-cut path is forced:

  page 1: eight U+1F600 (4-byte UTF-8) via ActualText
  page 2: eight U+4E2D  (3-byte UTF-8) via ActualText
  page 3: eight U+00E9  (2-byte UTF-8) via ActualText

ActualText is UTF-16BE with BOM so Poppler text_list yields the scalars
even though the painted glyphs are placeholder ASCII.

Usage (from repo root):
  python3 test/data/gen_chunks_utf8.py
"""

from __future__ import annotations

from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "chunks_utf8.pdf"


def assemble(objs: dict[int, bytes]) -> bytes:
    header = b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
    body = bytearray(header)
    offsets = {0: 0}
    for i in sorted(objs):
        offsets[i] = len(body)
        body += f"{i} 0 obj\n".encode("ascii")
        body += objs[i]
        body += b"\nendobj\n"
    xref_pos = len(body)
    max_id = max(objs)
    body += f"xref\n0 {max_id + 1}\n".encode("ascii")
    body += b"0000000000 65535 f \n"
    for i in range(1, max_id + 1):
        body += f"{offsets[i]:010d} 00000 n \n".encode("ascii")
    body += (f"trailer\n<< /Size {max_id + 1} /Root 1 0 R >>\n" f"startxref\n{xref_pos}\n%%EOF\n").encode("ascii")
    return bytes(body)


def actual_text_hex(codepoints: list[int]) -> bytes:
    """PDF hex string for /ActualText: UTF-16BE with BOM."""
    payload = "FEFF" + "".join(
        f"{cp:04X}" if cp <= 0xFFFF else f"{0xD800 + ((cp - 0x10000) >> 10):04X}{0xDC00 + ((cp - 0x10000) & 0x3FF):04X}"
        for cp in codepoints
    )
    return payload.encode("ascii")


def marked_span(codepoints: list[int], placeholder: bytes) -> bytes:
    hex_text = actual_text_hex(codepoints)
    return (
        b"BT /F1 12 Tf 72 700 Td\n"
        b"/Span << /ActualText <" + hex_text + b"> >> BDC\n"
        b"(" + placeholder + b") Tj\n"
        b"EMC\n"
        b"ET\n"
    )


def build() -> bytes:
    streams = [
        marked_span([0x1F600] * 8, b"xxxxxxxx"),
        marked_span([0x4E2D] * 8, b"xxxxxxxx"),
        marked_span([0x00E9] * 8, b"xxxxxxxx"),
    ]

    objects: dict[int, bytes] = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count 3 >>",
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    }
    for i, stream in enumerate(streams):
        page_id = 4 + 2 * i
        contents_id = 5 + 2 * i
        objects[page_id] = (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]\n"
            f"/Contents {contents_id} 0 R /Resources << /Font << /F1 3 0 R >> >> >>"
        ).encode("ascii")
        objects[contents_id] = (
            b"<< /Length " + str(len(stream)).encode("ascii") + b" >>\nstream\n" + stream + b"endstream"
        )
    return assemble(objects)


def main() -> None:
    pdf = build()
    OUT.write_bytes(pdf)
    print(f"wrote {OUT} ({len(pdf)} bytes), 3 pages")


if __name__ == "__main__":
    main()
