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

The `pdf` extension exposes the following functions covering the full range of PDF extraction tasks:

| Function | Kind | Description |
|---|---|---|
| `read_pdf` | Table | One row per page: `filename`, `page`, `page_count`, `text`, `width`, `height` |
| `read_pdf_lines` | Table | One row per layout-preserving line: `filename`, `page`, `line`, `text` |
| `read_pdf_meta` | Table | Document metadata: title, author, subject, keywords, creator, producer, pages, pdf_version, encrypted |
| `read_pdf_words` | Table | One row per word with bounding box + font: `x0,y0,x1,y1`, `font_name`, `font_size` |
| `read_pdf_tables` | Table | Tabular regions from digital PDFs: `page`, `table_index`, `row_index`, `cells VARCHAR[]` |
| `read_pdf_elements` | Table | One row per layout element (`heading`/`paragraph`/`list_item`/`other`): `file`, `page_number`, `element_idx`, `element_type`, `text`, `font_size`, `bbox_x0..bbox_y1` |
| `pdf_chunks` | Table | Retrieval-ready chunks packed from the element grain: `file`, `chunk_idx`, `text`, `page_start`, `page_end`, `n_chars`, `heading` (section context). `chunk_size` / `overlap` named params. |
| `pdf_to_text` | Scalar | Convert a whole PDF to plain text (optionally with a `layout` argument: `reading`, `physical`, or `raw`). Path form routes through DuckDB's FileSystem (s3://, https://, VFS when httpfs etc loaded). Also has BLOB overloads. |
| `pdf_to_html` | Scalar | Convert PDF to absolutely-positioned HTML (BLOB and path overloads; path uses DuckDB VFS). |
| `pdf_to_xml` | Scalar | Convert PDF to pdftoxml-style XML with per-word bboxes (BLOB and path; VFS for paths). |
| `pdf_to_svg` | Scalar | Render a page of the PDF to an SVG embedding a base64 PNG raster (BLOB and path; VFS for paths). |
| `pdf_to_png` | Scalar | Render a page of the PDF to PNG bytes returned as BLOB (BLOB and path; VFS for paths). |
| `write_pdf` | Scalar | **Native** (no LibreOffice) write of VARCHAR content to a PDF file using libharu (Letter, Helvetica 10pt, word-wrap + paginate). Returns the output path. |
| `to_pdf` | Scalar | Convert an office/markup document (docx, odt, rtf, html, pptx, xlsx, ...) **to** a PDF via LibreOffice (runtime shell-out). Only needed for rich document conversion. |
| `pdf_merge` | Scalar | **Document-level (qpdf)**: concatenate the input PDFs' pages, in list order, into one output file. Returns the output path. |
| `pdf_split` | Table | **Document-level (qpdf)**: write one single-page PDF per page as `<output_dir>/<stem>_p<N>.pdf` (zero-padded); one row per emitted file: `page`, `file`. |
| `pdf_rotate` | Scalar | **Document-level (qpdf)**: rotate pages by a multiple of 90°; `pages` is `'all'` (default) or a qpdf range like `'1-3,7'`. Returns the output path. |
| `pdf_compress` | Scalar | **Document-level (qpdf)**: structural optimization — object streams, stream recompression, linearization. Does **not** downsample images. Returns the output path. |
| `pdf_encrypt` | Scalar | **Document-level (qpdf)**: AES-256 (R6) password protection; optional distinct owner password. Returns the output path. |
| `pdf_decrypt` | Scalar | **Document-level (qpdf)**: remove password protection given the password. Returns the output path. |
| `pdf_pages` | Scalar | **Document-level (qpdf)**: extract a page subset (`'1-3,7'`, `'z'` = last, `'r2'` = second-to-last) into a new file, in range order. Returns the output path. |
| `pdf_form_fields` | Table | One row per AcroForm field: `file`, `page`, `field_name`, `field_type` (`text`/`button`/`choice`/`signature`), `value`, `is_required`. |
| `pdf_annotations` | Table | One row per page annotation: `file`, `page`, `subtype`, `contents`, `uri` (Link annotations), `rect_x0..rect_y1`. |

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

### `pdf_info` — full per-file census

One row per file: identity metadata plus the structural facts (a superset of `read_pdf_meta`). Unset metadata keys and dates are `NULL`, never `''`. `width`/`height` are the first-page media box in points; `file_size` is bytes on disk.

```sql
-- Census a folder of PDFs
SELECT file, title, author, page_count, pdf_version, file_size
FROM pdf_info('archive/*.pdf')
ORDER BY file_size DESC;

-- Which tool produced what, and when
SELECT producer, count(*) AS n, min(creation_date) AS earliest
FROM pdf_info('archive/*.pdf')
GROUP BY producer;
```

Columns: `file`, `title`, `author`, `subject`, `keywords`, `creator`, `producer`, `creation_date` (`TIMESTAMP`), `mod_date` (`TIMESTAMP`), `page_count`, `is_encrypted`, `is_linearized`, `pdf_version`, `width`, `height`, `file_size`.

### `pdf_outline` — bookmarks / table of contents

One row per bookmark, in depth-first document order. Files without an outline yield zero rows (not an error).

```sql
-- Table of contents with indentation
SELECT file, ord, repeat('  ', depth - 1) || title AS entry
FROM pdf_outline('manual.pdf')
ORDER BY ord;
```

Columns: `file`, `ord` (1-based document order), `depth` (1 = top level), `title`.

### `pdf_attachments` — embedded files

One row per embedded file. Files with no attachments yield zero rows. `data` carries the attachment bytes as a `BLOB`; `size` is `NULL` when the PDF does not declare it.

```sql
-- What is embedded across a folder?
SELECT file, name, mime_type, size
FROM pdf_attachments('invoices/*.pdf');
```

Columns: `file`, `name`, `description`, `size`, `mime_type`, `data` (`BLOB`).

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

### `read_pdf_elements` — layout-element extraction

Returns one row per layout element in reading order, with columns `file`, `page_number` (1-based), `element_idx` (1-based within page), `element_type`, `text`, `font_size` (dominant size of the element, NULL when the PDF carries no font info), and `bbox_x0`, `bbox_y0`, `bbox_x1`, `bbox_y1`.

Elements are built by deterministic geometry over poppler's positioned word list — words cluster into lines by vertical overlap, lines into blocks by vertical gap (> 0.6× median line height), font-size change (> 15%), or a list marker starting a line. Classification: `heading` = dominant font ≥ 1.15× the document's modal body size and < 200 chars, OR a short ALL-CAPS block at any font size (< 6 words, ≥ 4 alphabetic chars, ≥ 80% of them uppercase — catches section headers like `PROFESSIONAL SUMMARY` set at body size); `list_item` = first line starts with a bullet glyph (`•`, `–`, `▪`, or `-`/`*` + space) or an `N.` / `N)` marker; `paragraph` = any other block of ≥ 3 words; `other` = the rest (page numbers, isolated fragments). v1 reads the native text layer only (no OCR) and does not attempt table detection — table rows come out as paragraphs; use `read_pdf_tables` for tables.

```sql
-- Document outline: just the headings
SELECT page_number, text, font_size
FROM read_pdf_elements('report.pdf')
WHERE element_type = 'heading'
ORDER BY page_number, element_idx;

-- Chunk a PDF for RAG at paragraph grain, keeping position
SELECT file, page_number, element_idx, text, bbox_y0
FROM read_pdf_elements('docs/*.pdf')
WHERE element_type IN ('paragraph', 'list_item')
ORDER BY file, page_number, element_idx;
```

### `pdf_chunks` — retrieval-ready chunking

`pdf_chunks(files, chunk_size := 1200, overlap := 150)` packs the `read_pdf_elements` grain into retrieval-ready chunks — same file handling (single path or glob), plus `password` / `first_page` / `last_page`. Columns: `file`, `chunk_idx` (1-based per file), `text`, `page_start`, `page_end`, `n_chars` (length of the emitted text, overlap included), and `heading` (text of the nearest preceding `heading` element — every chunk carries its section context; NULL before the first heading).

Deterministic chunking rules (no ML, no tokenizers):

1. Elements pack into a chunk in reading order until the next one would push the chunk past `chunk_size`.
2. An element is never split across chunks — unless it alone exceeds `chunk_size`, in which case it splits at the last whitespace before the limit (repeatedly for very long elements).
3. A `heading` never ends a chunk: it glues forward to start the next one, keeping headers attached to their section.
4. Each chunk after the first is prefixed with the trailing `overlap` characters of the previous chunk's text, trimmed to a whitespace boundary so no word is cut.

```sql
-- One statement from a folder of PDFs to a chunk table
CREATE TABLE chunks AS FROM pdf_chunks('docs/*.pdf');

-- Every chunk knows its section
SELECT heading, chunk_idx, n_chars, text[:80]
FROM pdf_chunks('report.pdf', chunk_size := 800, overlap := 100);
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

### COPY options (FORMAT pdf)

Supported options (case-insensitive):

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| TITLE | VARCHAR | (none) | Sets PDF document metadata Title. |
| AUTHOR | VARCHAR | (none) | Sets PDF document metadata Author. |
| FONT_SIZE | DOUBLE | 10 | Body font size in points (4..72). |
| PAGE_SIZE | VARCHAR | 'letter' | 'letter', 'a4' or 'legal'. |
| MARGIN | DOUBLE | 54 | Margin in points (0..216). |
| HEADER | VARCHAR | (none) | Centered line at top margin band of every page (uses font_size-2, min 6). |
| FOOTER | VARCHAR | (none) | Centered line at bottom margin band of every page (supports literal `{page}` placeholder). |

If HEADER or FOOTER is supplied, MARGIN must be >=24 (to fit in band).

Example:

```sql
LOAD pdf;
COPY (SELECT * FROM my_query)
TO 'report.pdf' (FORMAT pdf,
                 TITLE 'Q3 Report',
                 AUTHOR 'Alok',
                 PAGE_SIZE 'a4',
                 FONT_SIZE 11,
                 HEADER 'ACME Internal',
                 FOOTER 'confidential - page {page}');
```

## Document-level operations (qpdf)

`pdf_merge`, `pdf_split`, and `pdf_rotate` are content-preserving structural
transforms powered by [qpdf](https://qpdf.sourceforge.io/) — no rasterization,
no re-typesetting; page content streams are carried over byte-identically.
They operate on **local filesystem paths exactly as given** (like `write_pdf`):
an existing output file is overwritten (COPY TO semantics), but a missing
output *directory* is an error, never created.

```sql
-- merge: pages concatenate in list order
SELECT pdf_merge(['a.pdf', 'b.pdf', 'c.pdf'], 'combined.pdf');

-- the glob recipe: build the input list in SQL
SELECT pdf_merge(list(DISTINCT filename ORDER BY filename), 'combined.pdf')
FROM read_pdf('reports/*.pdf');

-- split: one single-page PDF per page -> out/<stem>_p<N>.pdf (N zero-padded
-- to the page-count width); returns one row per emitted file
SELECT page, file FROM pdf_split('combined.pdf', 'out');

-- rotate: degrees must be a multiple of 90 (added to each page's existing
-- rotation); pages is 'all' or a qpdf-style range ('1-3,7', 'z' = last page)
SELECT pdf_rotate('scan.pdf', 'upright.pdf', 90);
SELECT pdf_rotate('scan.pdf', 'upright.pdf', -90, '2-4');
```

### The everyday suite: compress / encrypt / decrypt / pages

Same conventions as above: local paths exactly as given, existing output
overwritten, missing output directory is an error, and in-place operation
(`input == output`) is refused because qpdf reads the source lazily during
the write.

```sql
-- compress: object-stream generation + stream recompression + linearization
-- ("fast web view"). This optimizes document STRUCTURE and stream encoding;
-- it does NOT downsample or re-encode images — a scan-heavy PDF will not
-- shrink much.
SELECT pdf_compress('report.pdf', 'report_small.pdf');

-- encrypt: AES-256 (R6, the only password scheme the PDF 2.0 spec still
-- endorses), all permissions allowed. Omitted/empty owner password falls
-- back to the user password. Uses qpdf's built-in native crypto — no
-- external crypto library involved.
SELECT pdf_encrypt('report.pdf', 'report_locked.pdf', 'user-secret');
SELECT pdf_encrypt('report.pdf', 'report_locked.pdf', 'user-secret', 'owner-secret');

-- decrypt: open with the (user or owner) password, write a plain copy.
-- A wrong password raises an Invalid Input error ("invalid password").
SELECT pdf_decrypt('report_locked.pdf', 'report_plain.pdf', 'user-secret');

-- pages: extract a subset into a new document, in range order (qpdf range
-- grammar: '1-3,7', 'z' = last page, 'r2' = second-to-last; repeats allowed)
SELECT pdf_pages('report.pdf', 'summary.pdf', '1-3,7');
```

### Form fields and annotations

`pdf_form_fields(files)` returns one row per AcroForm field; `pdf_annotations(files)`
returns one row per page annotation. Both take a path or glob (resolved the same
way as `read_pdf`) and return zero rows for PDFs without forms/annotations.

```sql
-- one row per AcroForm field
-- file, page (NULL if the field has no widget on any page), field_name
-- (fully qualified), field_type ('text'/'button'/'choice'/'signature',
-- else qpdf's raw name), value (NULL if unset), is_required
SELECT field_name, field_type, value, is_required
FROM pdf_form_fields('forms/*.pdf');

-- one row per annotation: subtype (Link, Highlight, Text, FreeText, ...),
-- contents (NULL if none), uri (populated for Link annotations with an
-- /A /URI action), rect_x0/y0/x1/y1
SELECT page, subtype, contents FROM pdf_annotations('reviewed.pdf');

-- the hyperlink-extraction recipe
SELECT page, uri FROM pdf_annotations('reviewed.pdf') WHERE subtype = 'Link';
```

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

## RAG over a folder of PDFs in three statements

The extension stays a deterministic reader/writer — **it does not embed and it does not call models**. But `pdf_chunks` plus DuckDB's core [`vss`](https://duckdb.org/docs/stable/core_extensions/vss) extension is a complete local RAG substrate; you bring the embedder. The full runnable recipe lives in [`examples/rag_pdf_vss.sql`](examples/rag_pdf_vss.sql).

```sql
-- 1. chunk every PDF in the folder (deterministic, section-aware)
CREATE TABLE chunks AS FROM pdf_chunks('docs/*.pdf');

-- 2. add an embedding column and populate it with YOUR embedder of choice
--    (the pdf extension does not embed — this is the shape, not the call;
--    fill it from your embedding pipeline: an UDF, a COPY back in, etc.)
ALTER TABLE chunks ADD COLUMN embedding FLOAT[384];
-- UPDATE chunks SET embedding = <your_embedder>(text);

-- 3. index and search with the core vss extension
INSTALL vss; LOAD vss;
SET hnsw_enable_experimental_persistence = true;
CREATE INDEX chunks_hnsw ON chunks USING HNSW (embedding);

SELECT file, heading, page_start, text
FROM chunks
ORDER BY array_distance(embedding, [/* query embedding */]::FLOAT[384])
LIMIT 5;
```

Every hit comes back with its `file`, `page_start`/`page_end`, and the `heading` of the section it came from — citation-ready without a second lookup.

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
brew install poppler tesseract leptonica libharu qpdf

# Ubuntu / Debian
sudo apt-get install libpoppler-dev libtesseract-dev libleptonica-dev libhpdf-dev libqpdf-dev
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
