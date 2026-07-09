#!/usr/bin/env python3
"""Regenerate test/data/incremental.pdf from hello.pdf.

Appends one minimal classic-xref incremental update so the file has two
PDF revisions (original + one appended update chained via trailer /Prev).

Usage (from repo root):
  python3 test/data/gen_incremental_pdf.py
"""

from __future__ import annotations

from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "hello.pdf"
OUT = HERE / "incremental.pdf"


def main() -> None:
    base = SRC.read_bytes()
    if not base.endswith(b"%%EOF\n"):
        raise SystemExit(f"unexpected hello.pdf ending: {base[-20:]!r}")
    if b"startxref\n669\n%%EOF\n" not in base:
        raise SystemExit("hello.pdf startxref offset changed; update this script")

    orig_size = len(base)  # 979; original xref at 669
    obj_offset = orig_size
    obj = (
        b"8 0 obj\n"
        b"<< /Type /Metadata /Subtype /XML /Length 11 >>\n"
        b"stream\n"
        b"rev-update\n"
        b"endstream\n"
        b"endobj\n"
    )
    xref_offset = obj_offset + len(obj)
    xref_section = (
        b"xref\n"
        b"8 1\n" + f"{obj_offset:010d} 00000 n \n".encode("ascii") + b"trailer\n"
        b"<< /Size 9 /Root 2 0 R /Prev 669 >>\n"
        b"startxref\n" + f"{xref_offset}\n".encode("ascii") + b"%%EOF\n"
    )
    out = base + obj + xref_section
    OUT.write_bytes(out)
    print(f"wrote {OUT} ({len(out)} bytes); new xref at {xref_offset}, /Prev 669")


if __name__ == "__main__":
    main()
