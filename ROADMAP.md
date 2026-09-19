# Roadmap

An honest snapshot of what the `pdf` extension does today, what is being built
right now, and where it is (and is not) headed. See `README.md` for the full
function reference.

## What it is

`pdf` turns PDFs into tables and tables into PDFs, in SQL, in the DuckDB
process — no `pdftotext`, no Python, no temp files. Every entry point answers
one of six questions:

- **What does this document say** — text, at whatever grain you need.
- **What is it** — a census: metadata, page counts, encryption, version.
- **What is inside it** — the non-text structure: outline, attachments, form
  fields, annotations, embedded images, signatures.
- **Show me a page** — a page or a document as an image.
- **Make me a different PDF** — merge, split, rotate, compress, encrypt,
  redact, sign, stamp.
- **Make a PDF from this data** — typeset text, a query result, or another
  document format into a PDF.

## Shipped

### What does this document say

- **Grain readers** — `read_pdf` (one row per page), `read_pdf_lines` (one row
  per layout-preserving line), `read_pdf_words` / `read_pdf_layout` (one row
  per word: bbox, font, OCR source/confidence, geometric `line`,
  `column_index`, `page_width`/`page_height` — `read_pdf_layout` is the same
  bind/scan/init triple under a second name), `read_pdf_tables` (one row per
  detected table row), `read_pdf_elements` (one row per layout element —
  heading/paragraph/list_item/other — in reading order), `pdf_chunks`
  (retrieval-ready chunks with section headings). OCR is not a separate step:
  `read_pdf` and `read_pdf_words` auto-trigger Tesseract on pages with no text
  layer, tunable via `ocr_vars` / `ocr_config` / `ocr_dpi` / `ocr_psm` /
  `ocr_oem` / `ocr_preprocess` / `ocr_retry` / `tessdata_dir`. English (eng) is
  bundled, so community binaries OCR a scan with no host tessdata install.
- **Whole-document scalars** — `pdf_to_text`, `pdf_to_markdown`, `pdf_to_html`,
  `pdf_to_xml`, `pdf_json`, on paths, globs, and BLOBs.
- **OCR primitives** — `tesseract_ocr` (scalar, text from an image BLOB) and
  `ocr_image` (table, named-param OCR returning text + confidence + format) for
  pipelines that need OCR decoupled from PDF reading.

### What is it

- **Per-file census** — `pdf_info` (metadata, timestamps, dimensions, size,
  encryption, PDF/A detection), `read_pdf_meta` (the original metadata
  function, now a subset of `pdf_info`), `pdf_qpdf_info` (qpdf's own document
  report: xref census, encryption bits, linearization), `pdf_permissions`.
- **Per-page geometry** — `pdf_pages_info` (crop/media size, rotation,
  orientation, label per page).

### What is inside it

- **Navigation** — `pdf_outline` (bookmarks, depth-first), `pdf_destinations`
  (named/explicit destinations with position and zoom).
- **Embedded objects** — `pdf_attachments` (embedded files as BLOBs),
  `pdf_images` (embedded raster XObjects), `pdf_fonts`.
- **Interactive layer** — `pdf_form_fields` (AcroForm fields with type and
  value), `pdf_annotations` (annotations and hyperlinks).
- **Provenance and trust** — `pdf_revisions` (incremental-update forensics,
  oldest first), `pdf_signatures` (detect + OpenSSL CMS verification).

### Show me a page

- **Page PNGs at scale** — `pdf_page_images` (one row per page, rendered PNG
  BLOB), `pdf_write_page_images` (writes `out_dir/<stem>/p{N}.png` trees).
  Bundled URW base-14 fonts so community/vcpkg builds do not raster blank
  pages.
- **Single-page scalars** — `pdf_to_svg`, `pdf_to_png`, on paths, globs, and
  BLOBs.
- **Rendering primitive** — `poppler_render_page` (one page of a PDF BLOB as
  PNG bytes); `poppler_version()` reports the linked Poppler build.

### Make me a different PDF

- **Assemble / split** — `pdf_merge` (concatenate in list order), `pdf_split`
  (one single-page PDF per page), `pdf_split_blank` (split on blank-page
  separators, for mailroom batches).
- **Reflow** — `pdf_rotate` (multiples of 90°), `pdf_pages` (extract a range
  like `'1-3,7'` or `'r2'`), `pdf_watermark` (diagonal text stamp), `pdf_bates`
  (Bates numbering).
- **Repair, shrink, lock** — `pdf_compress` (structural compression +
  linearization), `pdf_repair` (rebuild a damaged file via qpdf), `pdf_encrypt`
  / `pdf_decrypt` (AES-256).
- **Redact** — `pdf_redact` (constant-arg raster true-removal of boxed
  regions), `pdf_redact_lateral` (column-ref / dependent-join form of the same
  operation); both fail loudly rather than emitting a blank page when Poppler
  has no display fonts.
- **Sign** — `pdf_sign` (apply a `pkcs7.detached` CMS signature, verified via
  `pdf_signatures`).

### Make a PDF from this data

- **Native write** — `write_pdf` (text to PDF via libharu), `COPY ... TO ...
  (FORMAT pdf)` (a query result typeset as a PDF, with `TITLE`/`AUTHOR`/
  `HEADER`/`FOOTER`/`FONT_SIZE`/`PAGE_SIZE`/`MARGIN`).
- **Office conversion** — `to_pdf` (office/markup documents via a LibreOffice
  runtime shell-out).

## 0.9.0 — in progress

0.8.0 is live: the `duckdb/community-extensions` descriptor for `pdf` reads
`version: 0.8.0`, `ref: 6535c81`, so `INSTALL pdf FROM community` matches what
this section builds on.

1. **The layout engine** — `layout := 'auto'` as the default, so reading order
   is correct on multi-column pages with no parameters; an unrecognised
   `layout` value fails loudly instead of silently producing wrong text;
   `read_pdf_words.column_index`; the DuckDB submodule bumped to v1.5.5.
   **Done, pending merge** — [PR #21](https://github.com/asubbarao/duckdb-pdf/pull/21),
   still a draft.
2. **`FunctionDescription` on every registered function.** Today
   `duckdb_functions()` reports `col0, col1, col2` and a NULL description for
   every one of them — nothing in `LoadInternal` sets a description. This also
   fixes the public registry page at duckdb.org, generated from a
   `duckdb_functions()` diff, which currently reads "The function table
   contains no parameter descriptions or examples."
3. **`sniff_pdf(file)`** — modelled column-for-column on DuckDB core's own
   `sniff_csv` (`Delimiter`, `Quote`, …, `UserArguments`, `Prompt`): a census
   of the file (encrypted, form fields present, OCR needed, table-like content,
   page count) plus a `UserArguments` column and a paste-ready `Prompt` column
   suggesting which reader to call.
4. **`pdf_help()`** — reads the same static description table as (2), so the
   descriptions live in one place. A deliberate deviation from the rest of
   DuckDB: nothing in core, and nothing in the community extensions installed
   here, exposes a help function. The justification is that there is no
   `COMMENT ON FUNCTION` for table functions, so there is nowhere else to put a
   description a person can query.

Then: publish 0.9.0 as a one-file PR to `extensions/pdf/description.yml`,
bumping `version` and `repo.ref`.

## 1.0.0 — the consolidation

A breaking change: **50 registered function names collapse to 19** (21 scalar +
29 table today, plus the `COPY ... TO ... (FORMAT pdf)` registration). The old
version of this file was itself the symptom — its "Shipped" section was an
inventory of every registered name, which is exactly the shape that lets the
count climb unnoticed one function at a time. Nineteen names is a surface a
person, or an agent, can hold in their head; fifty is not.

Two of the fifty are not even distinct behaviour. `read_pdf_layout` is a second
registration of `read_pdf_words`'s own bind/scan/init triple, and
`poppler_render_page(BLOB, INTEGER, INTEGER)` is byte-for-byte `pdf_to_png`:
both return the same 14,602 bytes for page 1 of the DuckLake docs at 72 dpi,
and `poppler_render_page(b, 1, 72) = pdf_to_png(b, 1, 72)` is `true`. That is
what the consolidation is fixing — not a reshuffle of names, but a surface that
grew duplicates without anyone noticing.

| 1.0.0 name | absorbs |
| --- | --- |
| `read_pdf(file, grain := 'page')` | `read_pdf_lines`, `read_pdf_elements`, `read_pdf_tables`, `pdf_chunks`, `read_pdf_layout` — `grain` chooses the schema at bind time |
| `read_pdf_words` | keeps its own name (the one grain with a genuinely different row shape) |
| `pdf_info(file, detail := 'basic')` | `read_pdf_meta`, `pdf_permissions`, `pdf_qpdf_info` |
| `pdf_objects(file, kind := …)` | `pdf_outline`, `pdf_destinations`, `pdf_fonts`, `pdf_form_fields`, `pdf_annotations`, `pdf_attachments`, `pdf_images`, `pdf_signatures`, `pdf_revisions`, `pdf_pages_info` |
| `pdf_text(src, format := 'text')` | `pdf_to_text`, `pdf_to_markdown`, `pdf_to_html`, `pdf_to_xml`, `pdf_json` |
| `pdf_render(file, format := 'png', …)` | `pdf_to_png`, `pdf_to_svg`, `pdf_page_images`, `pdf_write_page_images`, `poppler_render_page` |
| `pdf_merge` | `pdf_merge` |
| `pdf_pages` | `pdf_pages`, `pdf_rotate` |
| `pdf_split` | `pdf_split`, `pdf_split_blank` |
| `pdf_stamp` | `pdf_watermark`, `pdf_bates` |
| `pdf_rewrite` | `pdf_compress`, `pdf_repair`, `pdf_encrypt`, `pdf_decrypt` |
| `pdf_redact` | `pdf_redact` |
| `pdf_redact_lateral` | `pdf_redact_lateral` |
| `pdf_sign` | `pdf_sign` |
| `write_pdf` | `write_pdf` |
| `to_pdf` | `to_pdf` |
| `ocr_image` | `ocr_image`, `tesseract_ocr` — two names and nine overloads become one function with named parameters. `tesseract_ocr` is an eight-rung positional ladder, `(BLOB)` through `(BLOB, VARCHAR, INTEGER, …)` out to eight arguments, beside an `ocr_image` that takes eighteen; `duckdb_functions()` renders the whole thing as `col0 … col7` |
| `sniff_pdf` | new in 0.9.0 |
| `pdf_help` | new in 0.9.0, and the home for `poppler_version` — "what am I actually running" belongs next to the Tesseract and qpdf versions, not in its own registration |

Each transform takes a STRUCT of options, so the call site labels itself
instead of relying on positional order.

Also in 1.0.0: canonical column names everywhere. `filename` first, then
`page`, `page_count`, `x0`/`y0`/`x1`/`y1`, `is_encrypted` — retiring four bbox
spellings (`x0`, `bbox_x0`, `rect_x0`, `left`/`bottom`/`right`/`top`), two file
spellings (`filename`, `file`), and two page spellings (`page`, `page_number`).
Old names are removed, not aliased.

The compatibility surface, recorded honestly: the known downstream consumer is
`teaguesterling`'s `duckeye`, a terminal document reader driving DuckDB from a
shell script. It uses exactly three things — `read_pdf(file, first_page := N,
last_page := M)` reading `page` and `text`; `pdf_to_text(<BLOB>)`, the BLOB
overload, fed straight from `zim_get_content()`; and `pdf_to_markdown` in its
`--help`. Page-range pushdown is load-bearing to it ("reading 2 pages of 400
beats reading 400 and discarding 398"), and its tests assert on text content
rather than row counts. So `read_pdf`'s name, its `first_page`/`last_page`
parameters, and its `page`/`text` columns are the contract 1.0.0 must keep;
`pdf_to_text`'s BLOB overload must survive as `pdf_text(src, …)` accepting a
BLOB.

## Planned

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
- **Shared pack open, narrowed** — one word walk already feeds `read_pdf`,
  `read_pdf_lines`, `read_pdf_elements` and `pdf_to_markdown`, so the remaining
  case for a shared Poppler-document open is narrower than this file used to
  claim: it is specifically the pack that needs page images alongside text
  grains, without opening the document twice.

## Non-goals

- **ML document understanding** — semantic layout models, entity extraction,
  question answering. That is docling / marker / model-server territory.
- **Merged-cell / borderless table reconstruction** — heuristic geometry
  handles ruled and simply-aligned tables; fully model-driven table recovery is
  out of scope.
- **Deprecation shims** — 1.0.0 removes old names rather than aliasing them.
  Keeping the retired spellings around would defeat the reason for doing the
  consolidation at all.
