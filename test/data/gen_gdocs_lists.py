"""Regenerate gdocs_lists.pdf and running_header.pdf (Arial + ZWSP markers).

COPY (FORMAT pdf) is WinAnsi/Helvetica and drops U+25CF / U+200B, so these
fixtures are committed binaries. Re-run:

    uv run --with reportlab python3 test/data/gen_gdocs_lists.py
"""
from pathlib import Path

from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

HERE = Path(__file__).resolve().parent
FONT_CANDIDATES = [
    "/Library/Fonts/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/Library/Fonts/Arial Unicode.ttf",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
]


def arial() -> str:
    for p in FONT_CANDIDATES:
        if Path(p).exists():
            return p
    raise SystemExit("Arial.ttf not found — needed for U+25CF")


def write_gdocs_lists(font: str) -> None:
    pdfmetrics.registerFont(TTFont("ArialU", font))
    out = HERE / "gdocs_lists.pdf"
    c = canvas.Canvas(str(out), pagesize=(432, 648))
    c.setFont("ArialU", 12)
    c.drawString(50, 600, "Lead paragraph with enough words to stay a paragraph")
    c.drawString(50, 560, "\u25cf\u200b Black circle item one")
    c.drawString(50, 530, "1.\u200b Numbered zwsp item two")
    c.drawString(50, 490, "Trailing paragraph with enough words after the list")
    c.save()
    print("wrote", out)


def write_running_header() -> None:
    out = HERE / "running_header.pdf"
    c = canvas.Canvas(str(out), pagesize=(432, 648))
    for i in range(6):
        c.setFont("Helvetica", 12)
        c.drawString(150, 620, "RUNNING HEADER")
        c.drawString(50, 400, f"Unique body paragraph with plenty of words page {i + 1}")
        if i == 0:
            c.setFont("Helvetica-Bold", 20)
            c.drawString(150, 300, "RUNNING HEADER")
        c.showPage()
    c.save()
    print("wrote", out)


if __name__ == "__main__":
    write_gdocs_lists(arial())
    write_running_header()
