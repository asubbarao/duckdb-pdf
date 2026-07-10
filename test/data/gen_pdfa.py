#!/usr/bin/env python3
"""Regenerate test/data/pdfa.pdf.

Hand-assembles a minimal single-page classic-xref PDF whose document catalog
carries an XMP /Metadata stream declaring PDF/A identification
(pdfaid:part = 2, pdfaid:conformance = B). This is a *detection* fixture only:
the file is NOT a valid PDF/A document, it merely carries the pdfaid claim in
its XMP packet so pdf_info's pdfa_part / pdfa_conformance columns have
something to parse.

Usage (from repo root):
  python3 test/data/gen_pdfa.py
"""

from __future__ import annotations

from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "pdfa.pdf"

# XMP packet declaring PDF/A part 2, conformance B, in element form.
XMP = (
    '<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>\n'
    '<x:xmpmeta xmlns:x="adobe:ns:meta/">\n'
    ' <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">\n'
    '  <rdf:Description rdf:about=""\n'
    '      xmlns:pdfaid="http://www.aiim.org/pdfa/ns/id/">\n'
    "   <pdfaid:part>2</pdfaid:part>\n"
    "   <pdfaid:conformance>B</pdfaid:conformance>\n"
    "  </rdf:Description>\n"
    " </rdf:RDF>\n"
    "</x:xmpmeta>\n"
    '<?xpacket end="w"?>'
).encode("utf-8")


def main() -> None:
    header = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n"

    objects = [
        b"1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Metadata 5 0 R >>\nendobj\n",
        b"2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n",
        b"3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        b"/Resources << >> /Contents 4 0 R >>\nendobj\n",
        b"4 0 obj\n<< /Length 0 >>\nstream\n\nendstream\nendobj\n",
        b"5 0 obj\n<< /Type /Metadata /Subtype /XML /Length "
        + str(len(XMP)).encode("ascii")
        + b" >>\nstream\n"
        + XMP
        + b"\nendstream\nendobj\n",
    ]

    body = bytearray()
    offsets = []
    cursor = len(header)
    for obj in objects:
        offsets.append(cursor)
        body += obj
        cursor += len(obj)

    xref_offset = len(header) + len(body)
    n = len(objects) + 1  # +1 for the free object 0

    xref = bytearray(b"xref\n")
    xref += f"0 {n}\n".encode("ascii")
    xref += b"0000000000 65535 f \n"
    for off in offsets:
        xref += f"{off:010d} 00000 n \n".encode("ascii")

    trailer = (
        b"trailer\n<< /Size "
        + str(n).encode("ascii")
        + b" /Root 1 0 R >>\nstartxref\n"
        + str(xref_offset).encode("ascii")
        + b"\n%%EOF\n"
    )

    out = header + bytes(body) + bytes(xref) + trailer
    OUT.write_bytes(out)
    print(f"wrote {OUT} ({len(out)} bytes); xref at {xref_offset}")


if __name__ == "__main__":
    main()
