#!/usr/bin/env python3
"""Write a small three-page PDF with explicit and named outline targets."""

from pathlib import Path


def make_pdf():
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R /Outlines 7 0 R /Names << /Dests << /Names [(named) [4 0 R /Fit]] >> >> >>",
        b"<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 6 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 6 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 6 0 R >>",
        b"<< /Length 0 >>\nstream\n\nendstream",
        b"<< /Type /Outlines /First 8 0 R /Last 11 0 R /Count 3 >>",
        b"<< /Title (Root) /Parent 7 0 R /First 9 0 R /Last 9 0 R /Next 10 0 R /Count 1 /Dest [3 0 R /Fit] >>",
        b"<< /Title (Child) /Parent 8 0 R /Dest [4 0 R /Fit] >>",
        b"<< /Title (Action) /Parent 7 0 R /Next 11 0 R /A << /S /GoTo /D [5 0 R /Fit] >> >>",
        b"<< /Title (Named) /Parent 7 0 R /Dest (named) >>",
    ]
    output = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for object_id, body in enumerate(objects, 1):
        offsets.append(len(output))
        output.extend(f"{object_id} 0 obj\n".encode())
        output.extend(body)
        output.extend(b"\nendobj\n")
    xref_offset = len(output)
    output.extend(f"xref\n0 {len(objects) + 1}\n".encode())
    output.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        output.extend(f"{offset:010d} 00000 n \n".encode())
    output.extend(
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref_offset}\n%%EOF\n".encode()
    )
    Path(__file__).with_name("outline.pdf").write_bytes(output)


if __name__ == "__main__":
    make_pdf()
