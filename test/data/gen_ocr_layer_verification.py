#!/usr/bin/env python3
"""Create synthetic pages with accurate and deliberately mistranslated text layers."""

from pathlib import Path

HERE = Path(__file__).resolve().parent
LINES = [
    "LOSS RUN REPORT BY POLICY AND CLAIM",
    "Report Losses as of 11/08/2026",
    "CLAIM NO: R00000001 CLAIM STATUS: Open",
    "MEDICAL $32,068 $15,961",
    "INDEMNITY $37,599 $10,948",
    "TOTAL $79,167 $28,106",
]


def cmap(mangle: bool) -> bytes:
    entries = []
    for code in range(0x20, 0x7F):
        char = chr(code)
        if mangle and "A" <= char <= "Z":
            char = chr((ord(char) - ord("A") + 13) % 26 + ord("A"))
        elif mangle and "a" <= char <= "z":
            char = chr((ord(char) - ord("a") + 13) % 26 + ord("a"))
        elif mangle and char.isdigit():
            char = str((int(char) + 5) % 10)
        entries.append(f"<{code:02X}> <{ord(char):04X}>")
    cmap_text = (
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        f"{len(entries)} beginbfchar\n"
        + "\n".join(entries)
        + "\nendbfchar\nendcmap\nCMapName currentdict /CMap defineresource pop\nend\nend"
    ).encode("ascii")
    return b"<< /Length " + str(len(cmap_text)).encode() + b" >>\nstream\n" + cmap_text + b"\nendstream"


def stream(data: bytes) -> bytes:
    return b"<< /Length " + str(len(data)).encode() + b" >>\nstream\n" + data + b"\nendstream"


def make_pdf(path: Path, mangle: bool) -> None:
    ops = ["BT /F1 16 Tf 72 720 Td 24 TL"]
    for line in LINES:
        escaped = line.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        ops.append(f"({escaped}) Tj T*")
    ops.append("ET")
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding /ToUnicode 6 0 R >>",
        stream("\n".join(ops).encode("ascii")),
        cmap(mangle),
    ]
    output = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for object_id, body in enumerate(objects, 1):
        offsets.append(len(output))
        output.extend(f"{object_id} 0 obj\n".encode("ascii"))
        output.extend(body + b"\nendobj\n")
    xref = len(output)
    output.extend(f"xref\n0 {len(objects) + 1}\n".encode("ascii"))
    output.extend(b"0000000000 65535 f \n")
    for offset in offsets[1:]:
        output.extend(f"{offset:010d} 00000 n \n".encode("ascii"))
    output.extend(f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode("ascii"))
    path.write_bytes(output)


if __name__ == "__main__":
    make_pdf(HERE / "ocr_layer_mismatched.pdf", True)
    make_pdf(HERE / "ocr_layer_matching.pdf", False)
