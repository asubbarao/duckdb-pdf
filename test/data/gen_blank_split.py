#!/usr/bin/env python3
"""Generate test/data/blank_split.pdf for pdf_split_blank tests.

Builds a 7-page PDF by hand (same minimal classic-xref assembly technique as
scripts/gen_signed_pdf.py — no external PDF library dependency):

  page 1: DOC A PAGE 1   (content)
  page 2: DOC A PAGE 2   (content)
  page 3: (blank separator)
  page 4: DOC B PAGE 1   (content)
  page 5: (blank separator)
  page 6: (blank separator, consecutive with page 5)
  page 7: DOC C PAGE 1   (content)

A "blank" page is a zero-length content stream: no extractable text and an
all-white raster, exactly the separator pdf_split_blank is meant to detect.
Expected split result: 3 documents —
  doc 1: pages 1-2 (2 pages)
  doc 2: page  4   (1 page)
  doc 3: page  7   (1 page)

Usage (from repo root):
  python3 test/data/gen_blank_split.py
"""

from __future__ import annotations

from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "blank_split.pdf"

PAGE_TEXTS = [
    "DOC A PAGE 1",  # page 1
    "DOC A PAGE 2",  # page 2
    None,  # page 3 (blank separator)
    "DOC B PAGE 1",  # page 4
    None,  # page 5 (blank separator)
    None,  # page 6 (blank separator, consecutive)
    "DOC C PAGE 1",  # page 7
]


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


def build() -> bytes:
    n_pages = len(PAGE_TEXTS)
    # Object numbering: 1=Catalog, 2=Pages, 3=Font,
    # then for each page i (0-based): (4 + 2*i)=Page, (5 + 2*i)=Contents.
    page_obj_ids = [4 + 2 * i for i in range(n_pages)]
    contents_obj_ids = [5 + 2 * i for i in range(n_pages)]

    kids = " ".join(f"{pid} 0 R" for pid in page_obj_ids)
    objects: dict[int, bytes] = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: (f"<< /Type /Pages /Kids [{kids}] /Count {n_pages} >>".encode("ascii")),
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    }

    for i, text in enumerate(PAGE_TEXTS):
        page_id = page_obj_ids[i]
        contents_id = contents_obj_ids[i]
        objects[page_id] = (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]\n"
            f"/Contents {contents_id} 0 R /Resources << /Font << /F1 3 0 R >> >> >>"
        ).encode("ascii")
        if text is None:
            stream = b""  # zero-length content stream: no text, renders all-white
        else:
            stream = f"BT /F1 24 Tf 72 700 Td ({text}) Tj ET\n".encode("ascii")
        objects[contents_id] = (
            b"<< /Length " + str(len(stream)).encode("ascii") + b" >>\nstream\n" + stream + b"endstream"
        )

    return assemble(objects)


def main() -> None:
    pdf = build()
    OUT.write_bytes(pdf)
    print(f"wrote {OUT} ({len(pdf)} bytes), {len(PAGE_TEXTS)} pages")


if __name__ == "__main__":
    main()
