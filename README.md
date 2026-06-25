# duckdb-pdf

A DuckDB community extension (`pdf`) for reading and extracting content from PDF files. Built on [Poppler](https://poppler.freedesktop.org/) for PDF parsing and [Tesseract](https://github.com/tesseract-ocr/tesseract) OCR for scanned documents.

## What it does

The `pdf` extension exposes eight functions covering the full range of PDF extraction tasks:

| Function | Kind | Description |
|---|---|---|
| `read_pdf` | Table | One row per page: `filename`, `page`, `page_count`, `text`, `width`, `height` |
| `read_pdf_lines` | Table | One row per layout-preserving line: `filename`, `page`, `line`, `text` |
| `read_pdf_meta` | Table | Document metadata: title, author, subject, keywords, creator, producer, pages, pdf_version, encrypted |
| `read_pdf_words` | Table | One row per word with bounding box + font: `x0,y0,x1,y1`, `font_name`, `font_size` |
| `read_pdf_tables` | Table | Tabular regions from digital PDFs: `page`, `table_index`, `row_index`, `cells VARCHAR[]` |
| `pdf_to_text` | Scalar | Convert a whole PDF to plain text (optionally with a `layout` argument: `reading`, `physical`, or `raw`) |

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

## Building from source

### Dependencies

Install native libraries before building:

```bash
# macOS
brew install poppler tesseract leptonica

# Ubuntu / Debian
sudo apt-get install libpoppler-dev libtesseract-dev libleptonica-dev
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
