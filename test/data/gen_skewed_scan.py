#!/usr/bin/env python3
"""Generate ``skewed_scan.pdf`` — an image-only (no text layer) scanned-page
fixture whose text raster is rotated ~2.5 degrees.

This mirrors the technique behind ``scanned.pdf``: rasterize text into an image
and embed that image as a full-page PDF with **no** selectable text layer, so the
only way to recover the words is OCR. The twist here is a deliberate ~2.5 degree
skew, which exercises the leptonica deskew stage of the OCR preprocessing
pipeline (``ocr_preprocess``).

Pillow is the only dependency (``pip install Pillow``); ``Image.save(..., "PDF")``
writes a single-page PDF that wraps the raster image with no text, exactly the
image-only shape we want.

Regenerate with:

    python3 test/data/gen_skewed_scan.py

Deterministic: no randomness, so re-running produces the same document.
"""

import os

from PIL import Image, ImageDraw, ImageFont

# Rotation applied to the rasterized text, in degrees. Chosen at 2.5 so the page
# is clearly skewed (well beyond the 0.3 degree deskew threshold) yet still within
# the range a deskew pass can straighten.
SKEW_DEGREES = 2.5

# The lines drawn onto the page. The test asserts on robust, dictionary words
# that tesseract recovers reliably once the page is deskewed.
LINES = [
    "SKEWED SCAN FIXTURE",
    "The quick brown fox jumps",
    "over the lazy dog today",
    "Invoice Number 12345",
]

OUT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "skewed_scan.pdf")


def _load_font(size):
    """Prefer a real TrueType face (clean glyphs OCR best); fall back to Pillow's
    bundled default at the requested size if none of the system faces exist."""
    candidates = [
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size=size)


def main():
    width, height = 1700, 1200
    font = _load_font(64)

    # Render the text onto a white canvas at a comfortable size/spacing so the
    # glyphs stay crisp through the rotation resample.
    canvas = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(canvas)
    y = 220
    for line in LINES:
        draw.text((180, y), line, fill="black", font=font)
        y += 160

    # Rotate the whole raster to introduce the skew. expand=True keeps every glyph
    # inside the frame; fillcolor="white" leaves a clean scanned-paper background.
    skewed = canvas.rotate(SKEW_DEGREES, resample=Image.BICUBIC, expand=True, fillcolor="white")

    # Save as an image-only PDF (no text layer). resolution sets the page DPI so
    # the physical page size is sane; the pixels are what matter for OCR.
    skewed.save(OUT_PATH, "PDF", resolution=150.0)
    print("wrote", OUT_PATH, skewed.size)


if __name__ == "__main__":
    main()
