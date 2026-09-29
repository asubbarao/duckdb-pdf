#!/usr/bin/env python3
"""Write small tagged PDFs, including one with a cyclic structure child."""

from pathlib import Path


def make_pdf(objects):
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
    output.extend(f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref_offset}\n%%EOF\n".encode())
    return output


def tagged_objects():
    return [
        b"<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 6 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 5 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 5 0 R >>",
        b"<< /Length 0 >>\nstream\n\nendstream",
        b"<< /Type /StructTreeRoot /RoleMap << /CustomP /P >> /K 7 0 R >>",
        b"<< /Type /StructElem /S /Document /Lang (en-US) /K [8 0 R 9 0 R 12 0 R 16 0 R 17 0 R 22 0 R] >>",
        b"<< /Type /StructElem /S /H1 /Pg 3 0 R /K 10 0 R >>",
        b"<< /Type /StructElem /S /P /Pg 3 0 R /ActualText (actual paragraph) /K [1 11 0 R] >>",
        b"<< /Type /MCR /Pg 3 0 R /MCID 0 >>",
        b"<< /Type /MCR /Pg 3 0 R /MCID 2 >>",
        b"<< /Type /StructElem /S /L /Pg 3 0 R /K 13 0 R >>",
        b"<< /Type /StructElem /S /LI /K [14 0 R 15 0 R] >>",
        b"<< /Type /StructElem /S /Lbl /K 3 >>",
        b"<< /Type /StructElem /S /LBody /K 18 0 R >>",
        b"<< /Type /StructElem /S /Table /Pg 4 0 R /K 19 0 R >>",
        b"<< /Type /StructElem /S /Figure /Pg 4 0 R /Alt (A diagram) /K [21 0 R 23 0 R] >>",
        b"<< /Type /MCR /Pg 3 0 R /MCID 4 >>",
        b"<< /Type /StructElem /S /TR /K 20 0 R >>",
        b"<< /Type /StructElem /S /TD /K 5 >>",
        b"<< /Type /StructElem /S /Span /K 6 >>",
        b"<< /Type /StructElem /S /CustomP /Pg 4 0 R /K 24 0 R >>",
        b"<< /Type /OBJR /Obj 4 0 R >>",
        b"<< /Type /MCR /Pg 4 0 R /MCID 7 >>",
    ]


def cyclic_objects():
    objects = tagged_objects()[:6]
    objects.extend(
        [
            b"<< /Type /StructElem /S /Document /K 7 0 R >>",
        ]
    )
    return objects


if __name__ == "__main__":
    directory = Path(__file__).parent
    (directory / "structure.pdf").write_bytes(make_pdf(tagged_objects()))
    (directory / "structure_cycle.pdf").write_bytes(make_pdf(cyclic_objects()))
