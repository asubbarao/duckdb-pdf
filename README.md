# duckdb-pdf

A DuckDB community extension (`pdf`) for reading and extracting content from PDF files. Built on [Poppler](https://poppler.freedesktop.org/) for PDF parsing and [Tesseract](https://github.com/tesseract-ocr/tesseract) OCR for scanned documents.

## Quick demo

Query any PDF like it's a table — text, lines, words with coordinates, tables, and metadata — **entirely in SQL, in the DuckDB process, with no external tools:**

```sql
INSTALL pdf FROM community;
LOAD pdf;

-- grep a PDF the way you'd grep a text file, keeping page + line context
SELECT page, line, text
FROM read_pdf_lines('resume.pdf')
WHERE text ILIKE '%duckdb%';
```

**Round-trip: read a PDF, edit it with SQL, write a fresh PDF** — the extraction and transformation never leave DuckDB. Use the new native `write_pdf` (libharu, in-process, no LibreOffice) for plain text→PDF; `to_pdf` (LibreOffice) remains only for rich office docs (see [Writing PDFs natively (no LibreOffice)](#writing-pdfs-natively-no-libreoffice) and [Saving / converting documents to PDF](#saving--converting-documents-to-pdf)).

```sql
-- 1. read the PDF, rewrite a job title in pure SQL, emit the edited doc as HTML
COPY (
  SELECT '<html><body style="font-family:Helvetica;white-space:pre-wrap">'
      || replace(pdf_to_text('resume.pdf'), 'Senior Engineer', 'Staff Engineer')
      || '</body></html>'
) TO 'edited.html' (FORMAT csv, HEADER false, QUOTE '');

-- 2. render the edited HTML to a new PDF, then read it back to prove the loop closed
SELECT to_pdf('edited.html', 'resume_v2.pdf');          -- requires LibreOffice
SELECT page, page_count, left(text, 60) AS head FROM read_pdf('resume_v2.pdf');
```

## What it does

The `pdf` extension exposes eight functions covering the full range of PDF extraction tasks:

| Function | Kind | Description |
|---|---|---|
| `read_pdf` | Table | One row per page: `filename`, `page`, `page_count`, `text`, `width`, `height` |
| `read_pdf_lines` | Table | One row per layout-preserving line: `filename`, `page`, `line`, `text` |
| `read_pdf_meta` | Table | Document metadata: title, author, subject, keywords, creator, producer, pages, pdf_version, encrypted |
| `read_pdf_words` | Table | One row per word with bounding box + font: `x0,y0,x1,y1`, `font_name`, `font_size` |
| `read_pdf_tables` | Table | Tabular regions from digital PDFs: `page`, `table_index`, `row_index`, `cells VARCHAR[]` |
| `pdf_to_text` | Scalar | Convert a whole PDF to plain text (optionally with a `layout` argument: `reading`, `physical`, or `raw`). Path form routes through DuckDB's FileSystem (s3://, https://, VFS when httpfs etc loaded). Also has BLOB overloads. |
| `pdf_to_html` | Scalar | Convert PDF to absolutely-positioned HTML (BLOB and path overloads; path uses DuckDB VFS). |
| `pdf_to_xml` | Scalar | Convert PDF to pdftoxml-style XML with per-word bboxes (BLOB and path; VFS for paths). |
| `pdf_to_svg` | Scalar | Render a page of the PDF to an SVG embedding a base64 PNG raster (BLOB and path; VFS for paths). |
| `pdf_to_png` | Scalar | Render a page of the PDF to PNG bytes returned as BLOB (BLOB and path; VFS for paths). |
| `write_pdf` | Scalar | **Native** (no LibreOffice) write of VARCHAR content to a PDF file using libharu (Letter, Helvetica 10pt, word-wrap + paginate). Returns the output path. |
| `to_pdf` | Scalar | Convert an office/markup document (docx, odt, rtf, html, pptx, xlsx, ...) **to** a PDF via LibreOffice (runtime shell-out). Only needed for rich document conversion. |

## Installation

```sql
INSTALL pdf FROM community;
LOAD pdf;
```

Or in a fresh session:

```sql
INSTALL pdf FROM 'https://community-extensions.duckdb.org';
LOAD pdf;
```

## Function reference and examples

### `read_pdf` — page-level text extraction

Returns one row per page with columns `filename`, `page`, `page_count`, `text`, `width`, `height` (page size in PDF points). Accepts a single file, a list of files, or a glob.

```sql
-- Read all pages of a single PDF
SELECT page, text
FROM read_pdf('report.pdf');

-- Read multiple PDFs with a glob
SELECT filename, page, text
FROM read_pdf('docs/*.pdf')
WHERE text ILIKE '%revenue%';

-- Count pages across a set of files
SELECT filename, count(*) AS page_count
FROM read_pdf('invoices/*.pdf')
GROUP BY filename;
```

### `read_pdf_lines` — layout-preserving line extraction

Returns one row per line of text with columns `filename`, `page`, `line` (1-based, reset per page), and `text`. A PDF-aware analog to the `read_lines` extension. Honors the same `layout` and page-range parameters as `read_pdf`.

```sql
-- Read a PDF as numbered lines
SELECT page, line, text
FROM read_pdf_lines('report.pdf')
ORDER BY page, line;

-- Grep across a directory of PDFs, keeping line context
SELECT filename, page, line, text
FROM read_pdf_lines('docs/*.pdf')
WHERE text ILIKE '%total due%';
```

### `read_pdf_meta` — document metadata

Returns document-level metadata. One row per file.

```sql
-- Inspect metadata for a single PDF
SELECT *
FROM read_pdf_meta('report.pdf');

-- List all PDFs that have no author set
SELECT filename, title, author
FROM read_pdf_meta('archive/*.pdf')
WHERE author IS NULL OR author = '';

-- Find the largest PDFs and which tool produced them
SELECT filename, producer, pages
FROM read_pdf_meta('docs/*.pdf')
ORDER BY pages DESC;
```

Columns: `filename`, `title`, `author`, `subject`, `keywords`, `creator`, `producer`, `pages`, `pdf_version`, `encrypted`.

### `read_pdf_words` — word-level extraction with bounding boxes

Returns one row per word with columns `filename`, `page`, `word`, `x0`, `y0`, `x1`, `y1` (coordinates in PDF user-space points, origin at bottom-left).

```sql
-- Extract all words from a PDF with coordinates
SELECT page, word, x0, y0, x1, y1
FROM read_pdf_words('form.pdf');

-- Find words in the top quarter of each page (approximate header zone)
-- PDF coordinates origin is bottom-left; page height is typically 792 pt for Letter
SELECT page, word, y0
FROM read_pdf_words('report.pdf')
WHERE y0 > 594   -- top ~25% of a 792-pt Letter page
ORDER BY page, y0 DESC, x0;

-- Reconstruct lines by grouping words close in y-coordinate
SELECT page,
       round(y0 / 12) * 12 AS line_band,
       string_agg(word, ' ' ORDER BY x0) AS line_text
FROM read_pdf_words('report.pdf')
GROUP BY page, line_band
ORDER BY page, line_band DESC;
```

### `read_pdf_tables` — structured table extraction

Extracts tabular regions from digital PDFs. Returns one row per table row with columns `filename`, `page`, `table_index`, `row_index`, and `cells` (a `VARCHAR[]` of the row's cells).

It uses a precision-first geometric heuristic (word bounding-box column clustering with a regularity gate): it favors not emitting spurious tables from prose over catching every table. Clean, aligned tables work well; merged cells, borderless/sparse tables, and scanned tables are out of scope — for those, see the note under [Scope](#scope).

```sql
-- Extract all tables from a PDF
SELECT *
FROM read_pdf_tables('financial_report.pdf');

-- Get tables from a specific page
SELECT *
FROM read_pdf_tables('financial_report.pdf')
WHERE page = 5;

-- Combine tables across multiple PDFs
SELECT filename, page, table_index, *
FROM read_pdf_tables('quarterly_reports/*.pdf')
ORDER BY filename, page, table_index;
```

### `pdf_to_text` — full-document text as a scalar

Returns the entire document as a single VARCHAR. Useful in expressions, JOINs, or when you want to embed text into a larger query.

```sql
-- Get the full text of a PDF
SELECT pdf_to_text('report.pdf') AS full_text;

-- Embed PDF text into a table
CREATE TABLE doc_texts AS
SELECT filename, pdf_to_text(filename) AS body
FROM glob('docs/*.pdf');

-- Search across many PDFs using a lateral join
SELECT f.filename, length(pdf_to_text(f.filename)) AS char_count
FROM glob('archive/*.pdf') AS f(filename)
WHERE pdf_to_text(f.filename) ILIKE '%quarterly earnings%';
```

**Path-based scalars now support DuckDB VFS:** `pdf_to_text('s3://...')`, `pdf_to_html('https://...')` etc. work when the `httpfs` (or other VFS) extension is loaded, because all path scalars now use `ReadAllBytes` (DuckDB FileSystem) + in-memory poppler load.

**BLOB overloads:** all five (`pdf_to_text`/`html`/`xml`/`svg`/`png` and their variants) accept `BLOB` for direct bytes input (no FS involved).

```sql
LOAD pdf;
-- All-DuckDB roundtrip: fetch bytes, parse PDF, no temp files
SELECT contains(pdf_to_text(content), 'hello')
FROM read_blob('s3://my-bucket/doc.pdf');   -- works with httpfs loaded

-- BLOB column roundtrip
SELECT pdf_to_text(content) FROM read_blob('local.pdf');
```

### `pdf_to_png` — page raster as PNG BLOB

`pdf_to_png(path, page[, dpi])` and the BLOB overload render a page exactly like the raster step of `pdf_to_svg` but return the raw PNG bytes as a `BLOB` (no SVG wrapper). Default DPI is 150; valid range 1–2400. Use for thumbnails, embedding, or feeding image tools.

```sql
LOAD pdf;

-- Typical usage: inspect size or feed downstream
SELECT octet_length(pdf_to_png('report.pdf', 1)) AS png_bytes;

-- DPI controls output size (higher = larger raster)
SELECT
    octet_length(pdf_to_png('report.pdf', 1, 72))  AS small,
    octet_length(pdf_to_png('report.pdf', 1, 300)) AS large;
```

BLOBs from `pdf_to_png` can be written out-of-band (e.g. via application code or `COPY` + post-processing) since DuckDB's `COPY` for BLOB columns performs text/CSV serialization.

## Writing PDFs natively (no LibreOffice)

`write_pdf` is a **purely native, in-process** scalar that authors a PDF from a
VARCHAR (plain text or SQL-generated). It uses libharu directly — **no external
tools, no LibreOffice, no shell-out**. It is always available once the extension
is loaded.

```sql
LOAD pdf;

-- 1-arg: writes to a temp file (platform TMPDIR/TMP/TEMP or /tmp + UUID); returns the path
SELECT write_pdf('Line one.' || chr(10) || 'Line two with wrapping that will be handled.');

-- 2-arg: explicit output path; returns that path
SELECT write_pdf('Hello native PDF from DuckDB.', 'out.pdf');

-- NULLs propagate
SELECT write_pdf(NULL);                 -- NULL
SELECT write_pdf('x', NULL);            -- NULL
```

**Behavior and limits:**
- Letter page size, 0.75 in margins, Helvetica 10 pt.
- Splits on `\n`, expands tabs, word-wraps each logical line, auto-paginates.
- Empty content produces a single blank (but valid) page.
- Only Base-14 Helvetica (WinAnsi/Latin-1 encoding): non-Latin scripts and many
  UTF-8 glyphs will not render correctly (they are dropped or replaced). This is
  acceptable for v1; use `to_pdf` + rich HTML for international text.
- Output path must be writable; failures raise a clear `IOException`.

**Self-contained roundtrip (read → edit in SQL → write → read, all in-process):**

```sql
LOAD pdf;
-- read a PDF, edit its text in SQL, write a NEW pdf entirely in-process:
SELECT write_pdf(replace(pdf_to_text('in.pdf'), 'old', 'new'), 'out.pdf');
SELECT page, text FROM read_pdf('out.pdf');
```

You can also use the `COPY` statement to write query results directly to a PDF:

```sql
LOAD pdf;
COPY (SELECT page, text FROM read_pdf('in.pdf')) TO 'out.pdf' (FORMAT pdf);  -- native, no LibreOffice
SELECT page, text FROM read_pdf('out.pdf');
```

(Note: each result row is emitted as one text line with columns space-separated; Helvetica 10pt, word-wrapped + paginated exactly as `write_pdf`.)

## Markdown extraction (layout mode)

`pdf_to_markdown(path VARCHAR) → VARCHAR` converts a PDF to GitHub-flavoured Markdown
using only poppler's word-level geometry — no AI, no external tools, fully deterministic
and local. This is the open-source equivalent of Snowflake's `AI_PARSE_DOCUMENT` LAYOUT
mode, without the API cost or privacy implications.

```sql
LOAD pdf;

-- Convert a PDF to Markdown (headings, bold, bullets, pipe tables detected automatically)
SELECT pdf_to_markdown('report.pdf');

-- Pipeline: extract structured markdown then parse it further in SQL
SELECT * FROM (
    SELECT pdf_to_markdown('report.pdf') AS md
) WHERE contains(md, '# ');
```

**What it detects:**
- **Headings**: lines whose font size is ≥ 1.15× the body font size; level assigned by descending size rank (`#`, `##`, `###`, `####`).
- **Tables**: aligned word columns detected by `ReconstructPageGrid`; emitted as GitHub pipe tables with a header-separator row.
- **Bold spans**: consecutive words whose font name contains "Bold" wrapped in `**...**`.
- **Lists**: lines starting with `-`, `*`, `•`, `◦`, or `^[0-9]+\.` converted to Markdown list items.
- **Paragraphs**: consecutive same-indent body lines merged into one paragraph block; blank lines between blocks.

Pages are joined with `\n\n`. NULL input → NULL output. Missing or encrypted PDFs raise an error.

## Saving / converting documents to PDF

The reading functions above go *from* a PDF. To go the other way — turn a
`docx`, `doc`, `odt`, `rtf`, `html`, `odp`, `pptx`, `xlsx`, ... into a PDF — use
`write_pdf` (above) for plain-text content, or `to_pdf` when you need rich
office-to-PDF conversion. `to_pdf` is the only route that shells out to
LibreOffice at runtime.

### Route 1 — the native `to_pdf` function

`to_pdf(input_path[, output_path])` is a scalar that converts a document to a PDF
and returns the output path. Like OCR needs Tesseract, **`to_pdf` needs
LibreOffice installed at runtime** — it shells out to the `soffice` binary. It is
*not* a build/link dependency, so the extension installs and loads fine on
machines without LibreOffice; the function only errors (with an actionable
message) if you call it when no converter is present.

```bash
# Install LibreOffice once
brew install --cask libreoffice                 # macOS
sudo apt-get install libreoffice                # Debian / Ubuntu
# Windows: https://www.libreoffice.org/download
```

```sql
-- Convert next to the input (extension swapped to .pdf); returns the new path
SELECT to_pdf('resume.docx');                   -- -> 'resume.pdf'

-- Convert to an explicit output path; returns that path
SELECT to_pdf('slides.pptx', '/tmp/slides.pdf');

-- Convert a directory of docs, then read them straight back
SELECT f.file, to_pdf(f.file) AS pdf_path
FROM glob('docs/*.docx') AS f(file);

-- One-shot: convert and immediately read the produced PDF
SELECT page, text
FROM read_pdf((SELECT to_pdf('report.odt')));
```

The `soffice`/`libreoffice` binary is auto-detected on `$PATH` (and the macOS app
bundle at `/Applications/LibreOffice.app/Contents/MacOS/soffice`). Resolution
order: **`LIBREOFFICE_PATH` env var → `soffice` on `$PATH` → `libreoffice` on
`$PATH` → the macOS app bundle.** If yours is in a non-standard location, set
`LIBREOFFICE_PATH` to the binary's absolute path. NULL input yields NULL output.

### Route 2 — pure SQL via shellfs (no extension change)

If you'd rather not depend on `to_pdf`, the same conversion composes from the
community [`shellfs`](https://duckdb.org/community_extensions/extensions/shellfs)
extension, which runs a shell command as a virtual file — no change to this
extension needed:

```sql
LOAD shellfs;  -- run a shell command as a virtual file
-- convert with LibreOffice, swallow stdout:
SELECT * FROM read_text(
  'soffice --headless --convert-to pdf --outdir /tmp "/path/resume.docx" && echo ok |'
);
-- then read the produced PDF straight back into DuckDB:
SELECT * FROM read_pdf('/tmp/resume.pdf');
```

## Building from source

### Dependencies

Install native libraries before building:

```bash
# macOS
brew install poppler tesseract leptonica libharu

# Ubuntu / Debian
sudo apt-get install libpoppler-dev libtesseract-dev libleptonica-dev libhpdf-dev
```

### Build

```bash
git clone --recurse-submodules https://github.com/asubbarao/duckdb-pdf.git
cd duckdb-pdf
make release
```

On Windows, dependencies are resolved through vcpkg (MSVC toolchain); see
`.github/workflows/MainDistributionPipeline.yml` for the exact build configuration.

The compiled extension will be at `build/release/extension/pdf/pdf.duckdb_extension`.

### Load locally (without installing)

```sql
LOAD 'build/release/extension/pdf/pdf.duckdb_extension';
```

## OCR support

Pages with no extractable text layer are OCR'd automatically (`auto_ocr`, on by default). Pass `ocr := true` to force OCR on every page, even ones that already have text.

**OCR needs a Tesseract language model at runtime**, and package managers do **not** bundle one with the library. The good news: once you install one the usual way, **it just works — no environment variable required.** The extension auto-detects the standard model directories that Homebrew, apt, and the Windows installer write to, so for most people OCR is a one-line install:

```bash
# macOS — install once, OCR works immediately (no TESSDATA_PREFIX needed)
brew install tesseract tesseract-lang

# Debian / Ubuntu — likewise auto-detected
sudo apt-get install tesseract-ocr tesseract-ocr-eng tesseract-ocr-deu   # English, German

# Windows — the UB Mannheim installer (https://github.com/UB-Mannheim/tesseract/wiki)
#   installs to C:\Program Files\Tesseract-OCR\tessdata, which is auto-detected.
```

The auto-detected locations are `/opt/homebrew/share/tessdata` and `/usr/local/share/tessdata` (macOS), `/usr/share/tessdata` and `/usr/share/tesseract-ocr/{5,4.00}/tessdata` (Linux), and `C:\Program Files\Tesseract-OCR\tessdata` (Windows).

**Connecting an existing / non-standard install.** If your models live somewhere else, point at them directly per query — no global config:

```sql
SELECT page, text
FROM read_pdf('scan.pdf', ocr := true, tessdata_dir := '/opt/models/tessdata');
```

You can also set the `TESSDATA_PREFIX` environment variable (Tesseract's own mechanism). Resolution order is: **`tessdata_dir` parameter → `TESSDATA_PREFIX` → auto-detected standard paths.** If no model is found anywhere, OCR raises a clear, actionable error — it never silently returns empty text.

Select the language with the `ocr_language` parameter (default `eng`), and tune the OCR path with `ocr_dpi` (default 300), `ocr_psm`, and `ocr_oem`:

```sql
SELECT page, text
FROM read_pdf('german_doc.pdf', ocr := true, ocr_language := 'deu', ocr_dpi := 300);
```

### OCR for `read_pdf_words` and `read_pdf_tables`

`read_pdf_words` and `read_pdf_tables` now return results for scanned/image-only PDFs (no text layer) by using tesseract's word-level `ResultIterator`. Previously these functions returned zero rows for scanned PDFs when no embedded text was present.

`read_pdf_words` emits two additional trailing columns:

- `source` VARCHAR: `'text'` for native text-layer words, `'ocr'` for words recovered via OCR.
- `confidence` DOUBLE: `NULL` for native words; a value in `[0, 100]` for OCR words.

For OCR words, `font_name` and `font_size` are `NULL` (no font metadata from the image).

```sql
SELECT word, x0, y0, x1, y1, confidence
FROM read_pdf_words('scanned.pdf')
WHERE source = 'ocr'
ORDER BY y1 DESC, x0;
```

Native-text PDFs continue to return `source = 'text'` with `confidence IS NULL` (no behavior change).

## Scope

This extension targets the ~80% of everyday PDF extraction — page/line/word text, metadata, and simple tables — directly in SQL, using Poppler (parsing/rendering) and Tesseract (OCR). It deliberately does **not** attempt ML-based layout analysis or table-structure recognition. For state-of-the-art document understanding (reading order, complex/merged-cell tables, form fields, RAG-ready output), reach for purpose-built tools such as [docling](https://github.com/docling-project/docling), [marker](https://github.com/VikParuchuri/marker), or a cloud Document AI service. A Poppler/Tesseract expert can extend this extension's heuristics (e.g. Leptonica preprocessing, ruling-line table detection) from the same building blocks it already exposes.

## Platform support

| Platform | Supported |
|---|---|
| Linux (x86_64, arm64) | Yes |
| macOS (x86_64, arm64) | Yes |
| Windows x64 (MSVC) | Yes |
| Windows (mingw, rtools, arm64) | No — see note |
| WebAssembly (all) | No — see note |

All dependencies (Poppler, Tesseract, Leptonica and their transitive native
libraries) are resolved through **vcpkg** and statically linked, so Linux, macOS,
and Windows x64 (MSVC) are all first-class. The **mingw/rtools** Windows variants
use a different toolchain than the MSVC static linkage this extension relies on,
**windows_arm64** is untested, and **WebAssembly** cannot link Poppler/Tesseract —
those targets are excluded.

## License

This extension is licensed under the **GNU General Public License v2.0 or later (GPL-2.0-or-later)**.

It statically links [Poppler](https://poppler.freedesktop.org/), which is distributed under the GPL-2.0 license. Any binary distribution of this extension is therefore subject to the GPL-2.0 terms: you must make the corresponding source code available to recipients.

See the [LICENSE](LICENSE) file for the full license text.
