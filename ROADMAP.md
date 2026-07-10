# Roadmap

An honest snapshot of what the `pdf` extension does today, what is being built
right now, and where it is (and is not) headed. See `README.md` for the full
function reference.

## Shipped

- **Readers** — `read_pdf` (page grain), `read_pdf_lines`, `read_pdf_words`
  (with bounding boxes + OCR confidence), `read_pdf_tables`,
  `read_pdf_elements` (layout elements in reading order), `pdf_chunks`
  (retrieval-ready chunking).
- **Whole-document scalars** — `pdf_to_text` / `pdf_to_markdown` /
  `pdf_to_html` / `pdf_to_xml` / `pdf_to_svg` / `pdf_to_png`, on paths, globs,
  and BLOBs.
- **Inspectors** — `pdf_info` (per-file census, incl. PDF/A `pdfa_part` /
  `pdfa_conformance` detection), `read_pdf_meta`, `pdf_outline`,
  `pdf_attachments`, `pdf_form_fields`, `pdf_annotations`, `pdf_revisions`
  (incremental-update forensics), `pdf_signatures` (detect + verify).
- **Transforms** — `pdf_merge`, `pdf_split`, `pdf_rotate`, `pdf_pages`,
  `pdf_compress`, `pdf_encrypt`, `pdf_decrypt`.
- **Writers** — `write_pdf` (native text-to-PDF), `COPY ... TO ... (FORMAT
  pdf)`, and `to_pdf` (office documents via LibreOffice).
- **OCR** — Tesseract-backed, auto-triggered on pages with no text layer, tuned
  via `ocr_dpi` / `ocr_psm` / `ocr_oem` / `tessdata_dir`.

### In flight (batch 2, landing now)

- `pdf_images` — extract embedded raster images as rows/BLOBs.
- `pdf_split_blank` — split a document on blank-page separators.
- `pdf_watermark` / `pdf_bates` — stamp watermarks and Bates numbering.

## In progress

- **`pdf_redact`** — raster-based redaction (render, black out regions,
  re-emit) so redacted content is truly gone from the page image.
- **`pdf_sign`** — CMS digital signing, the write-side complement to
  `pdf_signatures` verification.
- **OCR quality** — Leptonica preprocessing (deskew, binarize) plus a
  confidence-driven retry pass to lift accuracy on marginal scans.

## Planned

- **True content-stream redaction** — remove the underlying text/vector
  operators, not just the rendered pixels, so no recoverable content remains.
- **PDF/A validation** — beyond the current `pdfaid` detection, actually
  validate conformance against the PDF/A profile.
- **Standalone `ocr` extension** — a generic image-OCR extension for non-PDF
  inputs, factored out once the PDF OCR surface stabilizes.

## Non-goals

- **ML document understanding** — semantic layout models, entity extraction,
  question answering. That is docling / marker / model-server territory.
- **Merged-cell / borderless table reconstruction** — heuristic geometry
  handles ruled and simply-aligned tables; fully model-driven table recovery is
  out of scope.
