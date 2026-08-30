# Roadmap

An honest snapshot of what the `pdf` extension does today, what is being built
right now, and where it is (and is not) headed. See `README.md` for the full
function reference.

## Shipped

- **Readers** — `read_pdf` (page grain), `read_pdf_lines`, `read_pdf_words`
  (bounding boxes + OCR confidence + **`line`** geometric line id +
  **`page_width`/`page_height`**), `read_pdf_tables`, `read_pdf_elements`
  (layout elements in reading order), `pdf_chunks` (retrieval-ready chunking).
- **Whole-document scalars** — `pdf_to_text` / `pdf_to_markdown` /
  `pdf_to_html` / `pdf_to_xml` / `pdf_to_svg` / `pdf_to_png`, on paths, globs,
  and BLOBs.
- **Page previews (no `pdftoppm`)** — `pdf_page_images` (PNG BLOB per page),
  `pdf_write_page_images` (on-disk `out_dir/<stem>/p{N}.png` trees). Bundled
  URW base-14 fonts so community/vcpkg builds do not raster blank pages.
- **Inspectors** — `pdf_info` (per-file census, incl. PDF/A `pdfa_part` /
  `pdfa_conformance` detection), `pdf_pages_info` (per-page media/crop/
  rotation/label), `read_pdf_meta`, `pdf_outline`, `pdf_attachments`,
  `pdf_form_fields`, `pdf_annotations`, `pdf_revisions` (incremental-update
  forensics), `pdf_signatures` (detect + verify).
- **Transforms** — `pdf_merge`, `pdf_split`, `pdf_split_blank`, `pdf_rotate`,
  `pdf_pages`, `pdf_compress`, `pdf_encrypt`, `pdf_decrypt`, `pdf_watermark` /
  `pdf_bates`, `pdf_images` (embedded XObjects).
- **Redaction** — `pdf_redact` (constant-arg) + `pdf_redact_lateral` (column-ref
  / dependent join): raster true-removal of boxed regions; fails loudly if
  Poppler has no display fonts (bundled base-14 makes that path rare).
- **Signing** — `pdf_sign` (CMS write-side) complementing `pdf_signatures`
  verification.
- **Writers** — `write_pdf` (native text-to-PDF), `COPY ... TO ... (FORMAT
  pdf)`, and `to_pdf` (office documents via LibreOffice).
- **OCR** — Tesseract-backed, auto-triggered on pages with no text layer; Leptonica
  preprocess + confidence retry via `ocr_dpi` / `ocr_psm` / `ocr_oem` /
  `ocr_preprocess` / `ocr_retry` / `tessdata_dir`. **English (eng) model is
  bundled** (tessdata_fast) so community binaries OCR scans with zero host
  tessdata install.

## In progress

- **Community publish** — ship **0.8.0** (base-14, fail-loudly, lateral redact,
  `pdf_write_page_images`, C++20 private-API TUs, **bundled eng tessdata**,
  **`read_pdf_words.line` + page_width/height**, `pdf_pages_info`) through
  `duckdb/community-extensions` so `INSTALL pdf FROM community` matches main.
  Pin notes: bump `packaging/description.yml` `repo.ref` to the release SHA
  when opening the community-extensions PR; version stays **0.8.0** if this
  lands before first community cut, else **0.8.1**.

## Planned

- **Shared pack open / multi-grain cache** — one Poppler document open serving
  words + lines (+ optional page images) for the same path list without two full
  walks. Apps can already derive lines from `read_pdf_words` via `line` +
  `string_agg`; a true multi-output or session cache is the next densification
  step when dual-grain cost still shows up on large packs.
- **True content-stream redaction** — remove the underlying text/vector
  operators, not just the rendered pixels, so no recoverable content remains
  even under specialized PDF forensics tools. Raster redaction already makes
  text unextractable via `read_pdf` / ordinary copy-paste.
- **PDF/A validation** — beyond the current `pdfaid` detection, actually
  validate conformance against the PDF/A profile.
- **`write_pdf_table`** — typeset a query result as a multi-column table PDF
  (issue #19).
- **Standalone `ocr` extension** — a generic image-OCR extension for non-PDF
  inputs, factored out once the PDF OCR surface stabilizes further.

## Non-goals

- **ML document understanding** — semantic layout models, entity extraction,
  question answering. That is docling / marker / model-server territory.
- **Merged-cell / borderless table reconstruction** — heuristic geometry
  handles ruled and simply-aligned tables; fully model-driven table recovery is
  out of scope.
