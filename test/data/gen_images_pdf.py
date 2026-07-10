#!/usr/bin/env python3
"""Generate test/data/images.pdf with two embedded raster image XObjects.

The fixture exercises both extraction paths of pdf_images:
  * Im0 — a 16x16 DCTDecode (JPEG) image → pdf_images emits format='jpeg' and
    passes the raw JFIF bytes through (data starts with the JPEG SOI 0xFFD8).
  * Im1 — a 4x3 FlateDecode 8-bit DeviceRGB bitmap → pdf_images fully decodes
    the samples and re-wraps them as a PNG (data starts with the PNG signature
    0x89504E47).

Raw PDF construction (like gen_incremental_pdf.py) is used for exact control of
the filter chain — Pillow only supplies the JPEG codestream. Known dimensions
are asserted by test/sql/pdf_images.test.

Usage (from repo root):
  python3 test/data/gen_images_pdf.py
"""

from __future__ import annotations

import io
import zlib
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
OUT = HERE / "images.pdf"

JPEG_W, JPEG_H = 16, 16
RGB_W, RGB_H = 4, 3


def jpeg_bytes() -> bytes:
    """A 16x16 solid-red JPEG codestream (DCTDecode source data)."""
    img = Image.new("RGB", (JPEG_W, JPEG_H), (220, 30, 40))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=90)
    data = buf.getvalue()
    assert data[:2] == b"\xff\xd8", "expected JPEG SOI marker"
    return data


def flate_rgb_bytes() -> bytes:
    """Raw 4x3 8-bit RGB samples, zlib (FlateDecode) compressed."""
    # A distinct color per pixel so the decode is unambiguous; row-major RGB.
    pixels = bytearray()
    for y in range(RGB_H):
        for x in range(RGB_W):
            pixels += bytes(((x * 60) % 256, (y * 80) % 256, ((x + y) * 40) % 256))
    assert len(pixels) == RGB_W * RGB_H * 3
    return zlib.compress(bytes(pixels))


def build_pdf() -> bytes:
    jpeg = jpeg_bytes()
    flate = flate_rgb_bytes()

    objects: list[bytes] = []

    def add(body: bytes) -> None:
        objects.append(body)

    # 1: Catalog
    add(b"<< /Type /Catalog /Pages 2 0 R >>")
    # 2: Pages
    add(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    # 3: Page (draws both images)
    add(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
        b"/Resources << /XObject << /Im0 4 0 R /Im1 5 0 R >> >> "
        b"/Contents 6 0 R >>"
    )
    # 4: DCTDecode JPEG image XObject
    add(
        b"<< /Type /XObject /Subtype /Image /Width "
        + str(JPEG_W).encode()
        + b" /Height "
        + str(JPEG_H).encode()
        + b" /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length "
        + str(len(jpeg)).encode()
        + b" >>\nstream\n"
        + jpeg
        + b"\nendstream"
    )
    # 5: FlateDecode 8-bit DeviceRGB bitmap image XObject
    add(
        b"<< /Type /XObject /Subtype /Image /Width "
        + str(RGB_W).encode()
        + b" /Height "
        + str(RGB_H).encode()
        + b" /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length "
        + str(len(flate)).encode()
        + b" >>\nstream\n"
        + flate
        + b"\nendstream"
    )
    # 6: content stream drawing both images
    content = b"q 100 0 0 100 10 90 cm /Im0 Do Q\n" b"q 100 0 0 75 10 5 cm /Im1 Do Q\n"
    add(
        b"<< /Length "
        + str(len(content)).encode()
        + b" >>\nstream\n"
        + content
        + b"endstream"
    )

    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n".encode("ascii") + body + b"\nendobj\n"

    xref_offset = len(out)
    n = len(objects) + 1
    out += f"xref\n0 {n}\n".encode("ascii")
    out += b"0000000000 65535 f \n"
    for off in offsets[1:]:
        out += f"{off:010d} 00000 n \n".encode("ascii")
    out += (
        b"trailer\n<< /Size "
        + str(n).encode()
        + b" /Root 1 0 R >>\nstartxref\n"
        + str(xref_offset).encode()
        + b"\n%%EOF\n"
    )
    return bytes(out)


def main() -> None:
    pdf = build_pdf()
    OUT.write_bytes(pdf)
    print(
        f"wrote {OUT} ({len(pdf)} bytes); Im0={JPEG_W}x{JPEG_H} JPEG, Im1={RGB_W}x{RGB_H} FlateRGB"
    )


if __name__ == "__main__":
    main()
