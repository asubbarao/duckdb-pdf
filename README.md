# duckdb-pdf

**Everything PDF, in SQL.** A DuckDB community extension (`pdf`) that turns PDFs into tables and tables into PDFs: read text at every grain (page, line, word, layout element, retrieval chunk), inspect metadata / outlines / attachments / form fields / annotations / revisions / digital signatures, and do the everyday document operations people open Adobe Acrobat for — merge, split, rotate, compress, encrypt, decrypt, extract pages, write new PDFs. Built on [Poppler](https://poppler.freedesktop.org/) (parsing/rendering), [Tesseract](https://github.com/tesseract-ocr/tesseract) (OCR for scanned documents), [qpdf](https://qpdf.sourceforge.io/) (document surgery), and [libharu](https://github.com/libharu/libharu) (native PDF writing) — all in the DuckDB process, no external tools required at runtime.

Every function accepts a single path **or a glob**, so the natural unit of work is a folder of PDFs:

```sql
INSTALL pdf FROM community; LOAD pdf;

-- census a folder: producer, page count, size, encryption — one row per file
SELECT file, producer, page_count, is_encrypted, file_size
FROM pdf_info('docs/*.pdf') ORDER BY file_size DESC;

-- full-text search the folder, with page numbers
SELECT filename, page, text FROM read_pdf('docs/*.pdf') WHERE text ILIKE '%revenue%';

-- merge the folder into one PDF, then structurally compress it
SELECT pdf_compress(
         pdf_merge(list(file ORDER BY file), 'combined.pdf'),
         'combined_small.pdf')
FROM pdf_info('docs/*.pdf');

-- chunk everything for retrieval — one statement, section-aware
CREATE TABLE chunks AS FROM pdf_chunks('docs/*.pdf');
```

## Installation

```sql
INSTALL pdf FROM community;
LOAD pdf;
```

Or equivalently:

```sql
INSTALL pdf FROM 'https://community-extensions.duckdb.org';
LOAD pdf;
```

## Read & extract

Six table functions read a PDF at six grains, plus scalar converters for whole-document output. All of them take a single path or a glob; the extraction never leaves the DuckDB process.

Common named parameters for `read_pdf`, `read_pdf_lines`, `read_pdf_words`, and `read_pdf_tables`:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `password` | VARCHAR | — | Password for encrypted files. |
| `first_page` / `last_page` | INTEGER | full document | 1-based inclusive page range. |
| `layout` | VARCHAR | `'reading'` | Text extraction order: `'reading'`, `'physical'` (preserves column alignment), or `'raw'` (content-stream order). |
| `parse_tables` | BOOLEAN | false | Force physical layout (column-preserving) extraction. |
| `ocr` | BOOLEAN | false | Force OCR on every page, even pages with a text layer. |
| `auto_ocr` | BOOLEAN | true | OCR pages that have no extractable text layer. |
| `ocr_language` | VARCHAR | `'eng'` | Tesseract language model. |
| `ocr_dpi` | INTEGER | 300 | Render DPI for the OCR raster (1–2400). |
| `ocr_psm` / `ocr_oem` | INTEGER | Tesseract defaults | Page segmentation mode / OCR engine mode. |
| `tessdata_dir` | VARCHAR | auto-detected | Explicit Tesseract model directory (see [OCR support](#ocr-support)). |
| `ignore_errors` | BOOLEAN | false | `read_pdf` / `read_pdf_meta` only: skip unopenable files in a multi-file scan instead of aborting it. |

`read_pdf_elements` and `pdf_chunks` accept only `password`, `first_page`, `last_page` (plus `pdf_chunks`' own `chunk_size` / `overlap`) — they read the native text layer only.

### `read_pdf` — one row per page

Columns: `filename`, `page`, `page_count`, `text`, `width`, `height` (page size in PDF points).

The multi-file scan is **parallel**: each worker thread takes whole files, so a glob over a folder uses all your cores. (On Windows the scan runs serially — Poppler is not thread-safe across documents there.)

By default one corrupt or password-protected file aborts the whole scan. Pass `ignore_errors := true` (also on `read_pdf_meta`) to skip unopenable files and keep the good ones; the skipped set stays recoverable in SQL:

```sql
-- Which files were skipped?
SELECT file FROM glob('docs/*.pdf')
EXCEPT
SELECT DISTINCT filename FROM read_pdf('docs/*.pdf', ignore_errors := true);
```

```sql
-- Read all pages of a single PDF
SELECT page, text FROM read_pdf('report.pdf');

-- Search a folder of PDFs — parallel across files
SELECT filename, page, text
FROM read_pdf('docs/*.pdf')
WHERE text ILIKE '%revenue%';

-- Count pages across a set of files
SELECT filename, count(*) AS page_count
FROM read_pdf('invoices/*.pdf')
GROUP BY filename;
```

### `read_pdf_lines` — one row per line

Columns: `filename`, `page`, `line` (1-based, reset per page), `text`. A PDF-aware analog of `read_lines`: grep a PDF the way you'd grep a text file, keeping page + line context.

```sql
SELECT page, line, text
FROM read_pdf_lines('report.pdf')
ORDER BY page, line;

-- Grep across a directory of PDFs, keeping line context
SELECT filename, page, line, text
FROM read_pdf_lines('invoices/*.pdf')
WHERE text ILIKE '%total due%';
```

### `read_pdf_words` — one row per word, with bounding boxes

Columns: `filename`, `page`, `word`, `x0`, `y0`, `x1`, `y1` (PDF user-space points, origin bottom-left), `font_name`, `font_size`, `source`, `confidence`.

For scanned/image-only pages the words come from Tesseract's word-level iterator: `source` is `'ocr'` (vs `'text'` for native words) and `confidence` is a value in `[0, 100]` (`NULL` for native words). OCR words have `NULL` `font_name`/`font_size` — there is no font metadata in an image.

```sql
-- All words with coordinates
SELECT page, word, x0, y0, x1, y1 FROM read_pdf_words('report.pdf');

-- Words in the top quarter of each page (approximate header zone;
-- page height is typically 792 pt for Letter)
SELECT page, word, y0
FROM read_pdf_words('report.pdf')
WHERE y0 > 594
ORDER BY page, y0 DESC, x0;

-- OCR word boxes from a scanned document
SELECT word, x0, y0, x1, y1, confidence
FROM read_pdf_words('scan.pdf')
WHERE source = 'ocr'
ORDER BY y1 DESC, x0;
```

### `read_pdf_tables` — structured table extraction

Columns: `filename`, `page`, `table_index`, `row_index`, `cells` (a `VARCHAR[]` of the row's cells). Works on scanned PDFs too, via the OCR word layer.

Two detectors run per page:

- **Lattice (ruled) tables** — horizontal/vertical rule segments are collected from the page content streams and, when they form a usable grid, used as authoritative cell separators. Bordered tables are cut exactly along their lines, including cells whose text alignment alone would mis-cluster.
- **Unruled tables** — a precision-first geometric heuristic (word bounding-box column clustering with a regularity gate, plus document-wide column-edge voting so **right-aligned numeric columns** cluster correctly). It favors not emitting spurious tables from prose over catching every table.

Merged cells and borderless/sparse tables remain out of scope — see [Scope](#scope).

```sql
-- Extract all tables from a PDF
SELECT * FROM read_pdf_tables('financial_report.pdf');

-- Combine tables across a folder
SELECT filename, page, table_index, row_index, cells
FROM read_pdf_tables('docs/*.pdf')
ORDER BY filename, page, table_index, row_index;
```

### `read_pdf_elements` — layout elements in reading order

Columns: `file`, `page_number` (1-based), `element_idx` (1-based within page), `element_type`, `text`, `font_size` (dominant size of the element, `NULL` when the PDF carries no font info), `bbox_x0`, `bbox_y0`, `bbox_x1`, `bbox_y1`.

Elements are built by deterministic geometry over Poppler's positioned word list — words cluster into lines by vertical overlap, lines into blocks by vertical gap (> 0.6× median line height), font-size change (> 15%), or a list marker starting a line. Classification: `heading` = dominant font ≥ 1.15× the document's modal body size and < 200 chars, OR a short ALL-CAPS block at any font size; `list_item` = first line starts with a bullet glyph or an `N.` / `N)` marker; `paragraph` = any other block of ≥ 3 words; `other` = the rest (page numbers, isolated fragments). Reads the native text layer only (no OCR) and does not attempt table detection — use `read_pdf_tables` for tables.

```sql
-- Document outline: just the headings
SELECT page_number, text, font_size
FROM read_pdf_elements('report.pdf')
WHERE element_type = 'heading'
ORDER BY page_number, element_idx;

-- Paragraph-grain extraction across a folder, keeping position
SELECT file, page_number, element_idx, text, bbox_y0
FROM read_pdf_elements('docs/*.pdf')
WHERE element_type IN ('paragraph', 'list_item')
ORDER BY file, page_number, element_idx;
```

### `pdf_chunks` — retrieval-ready chunking

`pdf_chunks(files, chunk_size := 1200, overlap := 150)` packs the `read_pdf_elements` grain into retrieval-ready chunks. Columns: `file`, `chunk_idx` (1-based per file), `text`, `page_start`, `page_end`, `n_chars` (length of the emitted text, overlap included), and `heading` (text of the nearest preceding `heading` element — every chunk carries its section context; `NULL` before the first heading).

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

### Whole-document scalars: `pdf_to_text` / `markdown` / `html` / `xml` / `svg` / `png`

Scalar converters return the whole document (or one rendered page) as a single value — useful in expressions, joins, and `COPY` pipelines.

| Function | Signature | Returns |
|---|---|---|
| `pdf_to_text` | `(path_or_blob [, layout])` | Plain text; `layout` is `'reading'`, `'physical'`, or `'raw'`. |
| `pdf_to_markdown` | `(path)` | GitHub-flavoured Markdown (headings, bold, bullets, pipe tables) — see below. Path form only. |
| `pdf_to_html` | `(path_or_blob)` | Absolutely-positioned HTML. |
| `pdf_to_xml` | `(path_or_blob)` | pdftoxml-style XML with per-word bounding boxes. |
| `pdf_to_svg` | `(path_or_blob, page [, dpi])` | SVG embedding a base64 PNG raster of the page. Default DPI 150, range 1–2400. |
| `pdf_to_png` | `(path_or_blob, page [, dpi])` | Raw PNG bytes as a `BLOB`. Same raster and DPI rules as `pdf_to_svg`. |

**Paths go through DuckDB's FileSystem**, so `pdf_to_text('s3://...')` or `pdf_to_html('https://...')` work when `httpfs` (or another VFS) is loaded. **BLOB overloads** take the raw bytes directly — no filesystem involved:

```sql
-- Full text of a PDF as one value
SELECT pdf_to_text('report.pdf') AS full_text;

-- Physical layout preserves column alignment
SELECT pdf_to_text('report.pdf', 'physical');

-- Materialize a folder into a text table
CREATE TABLE doc_texts AS
SELECT file, pdf_to_text(file) AS body
FROM glob('docs/*.pdf');

-- BLOB roundtrip: bytes in, text out, no temp files
SELECT pdf_to_text(content) FROM read_blob('report.pdf');

-- Render page 1 as a PNG; DPI controls raster size
SELECT octet_length(pdf_to_png('report.pdf', 1))      AS png_150dpi,
       octet_length(pdf_to_png('report.pdf', 1, 300)) AS png_300dpi;
```

`pdf_to_markdown` converts using only Poppler's word-level geometry — no AI, no external tools, fully deterministic and local. It detects **headings** (font ≥ 1.15× body size, level by descending size rank), **tables** (aligned word columns emitted as GitHub pipe tables), **bold spans** (font name contains "Bold"), **lists** (`-`, `*`, `•`, `◦`, `N.` markers), and **paragraphs** (consecutive same-indent lines merged). Pages are joined with `\n\n`; `NULL` input → `NULL` output; missing or encrypted files raise an error.

```sql
SELECT pdf_to_markdown('report.pdf') AS md;
```

## Inspect

These table functions answer "what is in this file?" without extracting body text. All take a path or glob; `password := '...'` is accepted by `pdf_info`, `read_pdf_meta`, `pdf_outline`, `pdf_attachments`, `pdf_revisions`, and `pdf_signatures`.

### `pdf_info` — full per-file census

One row per file: identity metadata plus structural facts. Unset metadata keys and dates are `NULL`, never `''`. `width`/`height` are the first-page media box in points; `file_size` is bytes on disk.

Columns: `file`, `title`, `author`, `subject`, `keywords`, `creator`, `producer`, `creation_date` (`TIMESTAMP`), `mod_date` (`TIMESTAMP`), `page_count`, `is_encrypted`, `is_linearized`, `pdf_version`, `width`, `height`, `file_size`, `pdfa_part` (`INTEGER`), `pdfa_conformance` (`VARCHAR`).

`pdfa_part` and `pdfa_conformance` are read straight from the `pdfaid:part` / `pdfaid:conformance` claims in the document's XMP metadata packet — this is **detection only**, not validation. A file that merely declares `pdfaid:part=2, conformance=B` will report those values even if it does not actually conform to PDF/A. Both columns are `NULL` when the XMP packet is absent or carries no `pdfaid` claim.

```sql
-- Census a folder of PDFs
SELECT file, title, author, page_count, pdf_version, file_size
FROM pdf_info('docs/*.pdf')
ORDER BY file_size DESC;

-- Which tool produced what, and when
SELECT producer, count(*) AS n, min(creation_date) AS earliest
FROM pdf_info('docs/*.pdf')
GROUP BY producer;
```

### `read_pdf_meta` — legacy per-file metadata

The original metadata function, kept for compatibility. `pdf_info` is a superset (adds timestamps, linearization, page dimensions, file size); prefer it for new queries.

Columns: `filename`, `title`, `author`, `subject`, `keywords`, `creator`, `producer`, `pages`, `pdf_version`, `encrypted`.

```sql
SELECT filename, title, author, pages
FROM read_pdf_meta('report.pdf');
```

### `pdf_outline` — bookmarks / table of contents

One row per bookmark, in depth-first document order. Files without an outline yield zero rows (not an error). Columns: `file`, `ord` (1-based document order), `depth` (1 = top level), `title`.

```sql
-- Table of contents with indentation
SELECT file, ord, repeat('  ', depth - 1) || title AS entry
FROM pdf_outline('manual.pdf')
ORDER BY ord;
```

### `pdf_attachments` — embedded files

One row per embedded file; zero rows when there are none. `data` carries the attachment bytes as a `BLOB`; `size` is `NULL` when the PDF does not declare it. Columns: `file`, `name`, `description`, `size`, `mime_type`, `data`.

```sql
-- What is embedded across a folder?
SELECT file, name, mime_type, size
FROM pdf_attachments('invoices/*.pdf');
```

### `pdf_form_fields` — AcroForm fields

One row per form field. Columns: `file`, `page` (`NULL` if the field has no widget on any page), `field_name` (fully qualified), `field_type` (`'text'`/`'button'`/`'choice'`/`'signature'`, else qpdf's raw type name), `value` (`NULL` if unset), `is_required`.

```sql
-- Every field in a folder of forms
SELECT file, field_name, field_type, value, is_required
FROM pdf_form_fields('forms/*.pdf');
```

### `pdf_annotations` — annotations and hyperlinks

One row per page annotation. Columns: `file`, `page`, `subtype` (`Link`, `Highlight`, `Text`, `FreeText`, ...), `contents` (`NULL` if none), `uri` (populated for Link annotations with an `/A` `/URI` action), `rect_x0`, `rect_y0`, `rect_x1`, `rect_y1`.

```sql
SELECT page, subtype, contents FROM pdf_annotations('forms/*.pdf');

-- The hyperlink-extraction recipe
SELECT file, page, uri
FROM pdf_annotations('forms/*.pdf')
WHERE subtype = 'Link';
```

### `pdf_revisions` — incremental-update forensics

PDFs are append-only: editing a file with most tools adds an *incremental update* (a new body + xref + `%%EOF`) after the original bytes, leaving every earlier revision intact and recoverable. `pdf_revisions` enumerates those revisions by parsing the raw bytes (`startxref` / `%%EOF` markers and the trailer `/Prev` chain — classic xref tables and xref streams alike), oldest first.

One row per revision. Columns: `revision_index` (0 = original document), `startxref_offset`, `eof_offset` (byte offset just past this revision's `%%EOF`), `size_bytes` (bytes this revision added), `is_incremental` (`false` only for revision 0).

```sql
-- Has this contract been modified after it was first written?
SELECT count(*) - 1 AS updates_after_original
FROM pdf_revisions('contract.pdf');

-- Timeline of what each edit added
SELECT revision_index, size_bytes, eof_offset
FROM pdf_revisions('contract.pdf')
ORDER BY revision_index;
```

A single-revision file yields exactly one row. Truncating a file at any earlier `eof_offset` reconstructs that revision — useful for "what did this document say before the last edit?" forensics.

### `pdf_signatures` — digital signatures: detect + verify

One row per filled AcroForm signature field (unsigned files and empty signature fields yield zero rows, not an error). Columns: `file`, `field_name`, `subfilter` (e.g. `adbe.pkcs7.detached`), `signing_time` (`TIMESTAMP`), `signer_name`, `reason`, `location`, `covers_whole_file`, `verified`.

`verified` is a real cryptographic check: the detached CMS/PKCS#7 blob (`/Contents`) is verified with OpenSSL over the exact `/ByteRange` spans it signs. It attests **integrity** — the signed bytes have not been altered — but does not validate certificate-chain trust (no CA store is consulted). `covers_whole_file = false` means the file was extended after signing (see `pdf_revisions`): the signature can still verify over its own range while later revisions changed the visible document.

```sql
-- Are these contracts signed, by whom, and are the signatures intact?
SELECT file, signer_name, signing_time, verified, covers_whole_file
FROM pdf_signatures('contracts/*.pdf');

-- Signed-then-modified documents (signature valid but not covering the file)
SELECT file, field_name
FROM pdf_signatures('contracts/*.pdf')
WHERE verified AND NOT covers_whole_file;
```

## Transform & write

Document-level operations powered by qpdf are content-preserving structural transforms — no rasterization, no re-typesetting; page content streams are carried over byte-identically. Conventions shared by all of them: **local filesystem paths exactly as given**; an existing output file is overwritten (`COPY TO` semantics); a missing output *directory* is an error, never created; in-place operation (`input == output`) is refused because qpdf reads the source lazily during the write. Scalars return the output path, which makes them composable — the output of one is the input of the next.

### `pdf_merge` / `pdf_split` / `pdf_rotate` / `pdf_pages`

```sql
-- merge: pages concatenate in list order
SELECT pdf_merge(['a.pdf', 'b.pdf', 'c.pdf'], 'combined.pdf');

-- the glob recipe: build the input list in SQL
SELECT pdf_merge(list(DISTINCT filename ORDER BY filename), 'combined.pdf')
FROM read_pdf('docs/*.pdf');

-- split: one single-page PDF per page -> out/<stem>_p<N>.pdf (N zero-padded
-- to the page-count width); returns one row per emitted file
SELECT page, file FROM pdf_split('combined.pdf', 'out');

-- rotate: degrees must be a multiple of 90 (added to each page's existing
-- rotation); pages is 'all' (default) or a qpdf-style range ('1-3,7', 'z' = last)
SELECT pdf_rotate('scan.pdf', 'upright.pdf', 90);
SELECT pdf_rotate('scan.pdf', 'upright2.pdf', -90, '1');

-- pages: extract a subset into a new document, in range order (qpdf range
-- grammar: '1-3,7', 'z' = last page, 'r2' = second-to-last; repeats allowed)
SELECT pdf_pages('report.pdf', 'summary.pdf', '1');
```

### `pdf_compress` / `pdf_encrypt` / `pdf_decrypt`

```sql
-- compress: object-stream generation + stream recompression + linearization
-- ("fast web view"). This optimizes document STRUCTURE and stream encoding;
-- it does NOT downsample or re-encode images — a scan-heavy PDF will not
-- shrink much.
SELECT pdf_compress('report.pdf', 'report_small.pdf');

-- encrypt: AES-256 (R6, the only password scheme the PDF 2.0 spec still
-- endorses), all permissions allowed. Omitted owner password falls back to
-- the user password. Uses qpdf's built-in crypto — no external library.
SELECT pdf_encrypt('report.pdf', 'report_locked.pdf', 'user-secret');
SELECT pdf_encrypt('report.pdf', 'report_locked2.pdf', 'user-secret', 'owner-secret');

-- decrypt: open with the (user or owner) password, write a plain copy.
-- A wrong password raises an Invalid Input error ("invalid password").
SELECT pdf_decrypt('report_locked.pdf', 'report_plain.pdf', 'user-secret');
```

Encrypted files stay readable in place, too — every reader takes `password := '...'`:

```sql
SELECT page, text FROM read_pdf('report_locked.pdf', password := 'user-secret');
```

### `write_pdf` — native text-to-PDF (no LibreOffice)

`write_pdf(content [, output_path])` is a purely native, in-process scalar that authors a PDF from a VARCHAR using libharu — no external tools, no shell-out, always available once the extension is loaded. The 1-arg form writes to a temp file (platform `TMPDIR`/`TMP`/`TEMP` or `/tmp` + UUID); both forms return the output path.

```sql
-- 1-arg: temp file; 2-arg: explicit path
SELECT write_pdf('Line one.' || chr(10) || 'Line two.');
SELECT write_pdf('Hello native PDF from DuckDB.', 'out.pdf');

-- NULLs propagate
SELECT write_pdf(NULL);        -- NULL
SELECT write_pdf('x', NULL);   -- NULL

-- Self-contained roundtrip: read -> edit in SQL -> write -> read, all in-process
SELECT write_pdf(replace(pdf_to_text('report.pdf'), 'Q3', 'Q4'), 'report_q4.pdf');
SELECT page, text FROM read_pdf('report_q4.pdf');
```

Behavior and limits: Letter page size, 0.75 in margins, Helvetica 10 pt; splits on `\n`, expands tabs, word-wraps each logical line, auto-paginates; empty content produces a single blank (but valid) page. Only Base-14 Helvetica (WinAnsi/Latin-1 encoding) — non-Latin scripts and many UTF-8 glyphs will not render; use `to_pdf` + rich HTML for international text. The output path must be writable; failures raise a clear `IOException`.

### `COPY ... TO ... (FORMAT pdf)`

Write any query result directly to a PDF. Each result row is emitted as one text line with columns space-separated, typeset exactly as `write_pdf` (word-wrapped, paginated); row order is preserved (the copy runs serially).

```sql
COPY (SELECT page, text FROM read_pdf('report.pdf')) TO 'copy_out.pdf' (FORMAT pdf);
SELECT page, text FROM read_pdf('copy_out.pdf');
```

Supported options (case-insensitive; unknown options raise a binder error):

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `TITLE` | VARCHAR | (none) | PDF document metadata Title. |
| `AUTHOR` | VARCHAR | (none) | PDF document metadata Author. |
| `FONT_SIZE` | DOUBLE | 10 | Body font size in points (4–72). |
| `PAGE_SIZE` | VARCHAR | `'letter'` | `'letter'`, `'a4'`, or `'legal'`. |
| `MARGIN` | DOUBLE | 54 | Margin in points (0–216). |
| `HEADER` | VARCHAR | (none) | Centered line in the top margin band of every page (font_size−2, min 6). |
| `FOOTER` | VARCHAR | (none) | Centered line in the bottom margin band; supports a literal `{page}` placeholder. |

If `HEADER` or `FOOTER` is supplied, `MARGIN` must be ≥ 24 (to fit the band).

```sql
COPY (SELECT file, page_count, file_size FROM pdf_info('docs/*.pdf') ORDER BY file)
TO 'census.pdf' (FORMAT pdf,
                 TITLE 'Folder census',
                 AUTHOR 'duckdb-pdf',
                 PAGE_SIZE 'a4',
                 FONT_SIZE 11,
                 HEADER 'Internal',
                 FOOTER 'confidential - page {page}');
```

### `to_pdf` — office documents to PDF (LibreOffice)

`to_pdf(input_path [, output_path])` converts a rich document (`docx`, `odt`, `rtf`, `html`, `pptx`, `xlsx`, ...) to PDF and returns the output path. Unlike everything else in this extension, **`to_pdf` shells out to LibreOffice at runtime** — it is not a build/link dependency, so the extension installs and loads fine without LibreOffice; the function errors with an actionable message only when called with no converter present. The 1-arg form writes next to the input with the extension swapped to `.pdf`.

```bash
# Install LibreOffice once
brew install --cask libreoffice                 # macOS
sudo apt-get install libreoffice                # Debian / Ubuntu
# Windows: https://www.libreoffice.org/download
```

```sql
-- Convert next to the input; returns the new path
SELECT to_pdf('resume.docx');           -- -> 'resume.pdf'

-- ... then read the produced PDF straight back
SELECT page, text FROM read_pdf('resume.pdf');
```

The `soffice`/`libreoffice` binary is resolved in this order: **`LIBREOFFICE_PATH` env var → `soffice` on `$PATH` → `libreoffice` on `$PATH` → the macOS app bundle** (`/Applications/LibreOffice.app/Contents/MacOS/soffice`). `NULL` input yields `NULL` output.

## Recipes

### Folder census, one line

```sql
SELECT file, page_count, is_encrypted, file_size FROM pdf_info('docs/*.pdf');
```

### RAG over a folder of PDFs in three statements

The extension stays a deterministic reader/writer — **it ships no ML, does not embed, and does not call models**. But `pdf_chunks` plus DuckDB's core [`vss`](https://duckdb.org/docs/stable/core_extensions/vss) extension is a complete local RAG substrate; you bring the embedder. The full runnable recipe lives in [`examples/rag_pdf_vss.sql`](examples/rag_pdf_vss.sql).

```sql
-- 1. chunk every PDF in the folder (deterministic, section-aware)
CREATE TABLE rag_chunks AS FROM pdf_chunks('docs/*.pdf');

-- 2. add an embedding column and populate it with YOUR embedder of choice
--    (this is the shape, not the call — fill it from your embedding pipeline)
ALTER TABLE rag_chunks ADD COLUMN embedding FLOAT[384];
-- UPDATE rag_chunks SET embedding = <your_embedder>(text);

-- 3. index and search with the core vss extension
INSTALL vss; LOAD vss;
SET hnsw_enable_experimental_persistence = true;
CREATE INDEX chunks_hnsw ON rag_chunks USING HNSW (embedding);

SELECT file, heading, page_start, text
FROM rag_chunks
ORDER BY array_distance(embedding, [/* query embedding */]::FLOAT[384])
LIMIT 5;
```

Every hit comes back with its `file`, `page_start`/`page_end`, and the `heading` of the section it came from — citation-ready without a second lookup.

### Every form field from a folder of scanned government forms

Digital forms answer directly from the AcroForm dictionary; scanned (flattened) forms fall back to OCR word geometry:

```sql
-- digital forms: the fields are structured data
SELECT file, field_name, field_type, value
FROM pdf_form_fields('forms/*.pdf');

-- scanned forms: recover label/value words with positions via OCR
SELECT filename, page, word, x0, y0, confidence
FROM read_pdf_words('forms/*.pdf', ocr := true)
WHERE source = 'ocr'
ORDER BY filename, page, y1 DESC, x0;
```

### Search, extract, and shrink to email size

Find the pages that matter across a folder, pull just those pages into a new document, and compress it:

```sql
-- 1. find which page of which file mentions the contract
CREATE TABLE hits AS
SELECT filename, page
FROM read_pdf('docs/*.pdf')
WHERE text ILIKE '%revenue%';

-- 2. extract exactly those pages from the first matching file
SELECT pdf_pages(filename, 'excerpt.pdf', string_agg(page, ',' ORDER BY page))
FROM hits
GROUP BY filename
LIMIT 1;

-- 3. shrink it for email
SELECT pdf_compress('excerpt.pdf', 'excerpt_small.pdf');
```

## OCR support

Pages with no extractable text layer are OCR'd automatically (`auto_ocr`, on by default). Pass `ocr := true` to force OCR on every page, even ones that already have text. OCR applies to `read_pdf`, `read_pdf_lines`, `read_pdf_words`, and `read_pdf_tables`.

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
FROM read_pdf('scan.pdf', ocr := true, ocr_language := 'eng', ocr_dpi := 300);
```

## Scope

This extension targets the everyday PDF operations — reading at every grain, inspection, and document surgery — deterministically, using Poppler, Tesseract, qpdf, and libharu. It deliberately does **not** attempt ML-based layout analysis or table-structure recognition: table extraction combines ruled-line (lattice) detection with a precision-first geometric heuristic, so merged cells and borderless/sparse tables are out of scope. For state-of-the-art document understanding (complex tables, reading-order models), reach for purpose-built tools such as [docling](https://github.com/docling-project/docling), [marker](https://github.com/VikParuchuri/marker), or a cloud Document AI service. A Poppler/Tesseract expert can extend this extension's heuristics (e.g. Leptonica preprocessing) from the same building blocks it already exposes.

## Building from source

### Dependencies

```bash
# macOS
brew install poppler tesseract leptonica libharu qpdf openssl

# Ubuntu / Debian
sudo apt-get install libpoppler-dev libtesseract-dev libleptonica-dev libhpdf-dev libqpdf-dev libssl-dev
```

### Build

```bash
git clone --recurse-submodules https://github.com/asubbarao/duckdb-pdf.git
cd duckdb-pdf
make release
```

On Windows, dependencies are resolved through vcpkg (MSVC toolchain); see `.github/workflows/MainDistributionPipeline.yml` for the exact build configuration.

The compiled extension will be at `build/release/extension/pdf/pdf.duckdb_extension`:

```sql
LOAD 'build/release/extension/pdf/pdf.duckdb_extension';
```

## Platform support

| Platform | Supported |
|---|---|
| Linux (x86_64, arm64) | Yes |
| macOS (x86_64, arm64) | Yes |
| Windows x64 (MSVC) | Yes |
| Windows (mingw, rtools, arm64) | No — see note |
| WebAssembly (all) | No — see note |

All dependencies (Poppler, Tesseract, Leptonica, qpdf, libharu, and their transitive native libraries) are resolved through **vcpkg** and statically linked, so Linux, macOS, and Windows x64 (MSVC) are all first-class. The **mingw/rtools** Windows variants use a different toolchain than the MSVC static linkage this extension relies on, **windows_arm64** is untested, and **WebAssembly** cannot link Poppler/Tesseract — those targets are excluded. On Windows the parallel multi-file `read_pdf` scan runs serially (Poppler is not thread-safe across documents there); results are identical, only the parallelism differs.

## Function reference

| Function | Type | Description |
|---|---|---|
| `read_pdf(files)` | Table | One row per page: text plus page dimensions. Parallel multi-file scan; `ignore_errors` skips bad files. |
| `read_pdf_lines(files)` | Table | One row per layout-preserving line. |
| `read_pdf_words(files)` | Table | One row per word with bounding box, font, and OCR source/confidence. |
| `read_pdf_tables(files)` | Table | One row per detected table row; cells as `VARCHAR[]`. |
| `read_pdf_elements(files)` | Table | One row per layout element (`heading`/`paragraph`/`list_item`/`other`) with bbox. |
| `pdf_chunks(files)` | Table | Retrieval-ready chunks with section headings; `chunk_size`/`overlap` knobs. |
| `pdf_info(files)` | Table | Full per-file census: metadata, timestamps, dimensions, size, encryption. |
| `read_pdf_meta(files)` | Table | Legacy per-file metadata (subset of `pdf_info`). |
| `pdf_outline(files)` | Table | One row per bookmark, depth-first. |
| `pdf_attachments(files)` | Table | One row per embedded file, bytes as `BLOB`. |
| `pdf_form_fields(files)` | Table | One row per AcroForm field with type and value. |
| `pdf_annotations(files)` | Table | One row per annotation; `WHERE subtype = 'Link'` extracts hyperlinks. |
| `pdf_revisions(file)` | Table | One row per incremental-update revision, oldest first. |
| `pdf_signatures(files)` | Table | One row per digital signature: metadata + OpenSSL CMS verification. |
| `pdf_split(file, dir)` | Table | One single-page PDF per page; one row per emitted file. |
| `pdf_to_text(src [, layout])` | Scalar | Whole document as plain text. Path or `BLOB`. |
| `pdf_to_markdown(path)` | Scalar | Whole document as GitHub-flavoured Markdown. |
| `pdf_to_html(src)` | Scalar | Whole document as positioned HTML. Path or `BLOB`. |
| `pdf_to_xml(src)` | Scalar | Whole document as per-word-bbox XML. Path or `BLOB`. |
| `pdf_to_svg(src, page [, dpi])` | Scalar | One page as SVG (embedded PNG raster). Path or `BLOB`. |
| `pdf_to_png(src, page [, dpi])` | Scalar | One page as PNG bytes (`BLOB`). Path or `BLOB`. |
| `pdf_merge(files[], out)` | Scalar | Concatenate PDFs in list order. |
| `pdf_rotate(in, out, deg [, pages])` | Scalar | Rotate pages by multiples of 90°. |
| `pdf_pages(in, out, range)` | Scalar | Extract a page subset (`'1-3,7'`, `'z'`, `'r2'`). |
| `pdf_compress(in, out)` | Scalar | Structural compression + linearization. |
| `pdf_encrypt(in, out, userpw [, ownerpw])` | Scalar | AES-256 password protection. |
| `pdf_decrypt(in, out, pw)` | Scalar | Remove password protection. |
| `write_pdf(text [, out])` | Scalar | Native text-to-PDF via libharu. |
| `to_pdf(in [, out])` | Scalar | Office/markup document to PDF via LibreOffice (runtime shell-out). |
| `COPY ... (FORMAT pdf)` | Copy | Query result to a typeset PDF; `TITLE`/`AUTHOR`/`HEADER`/`FOOTER`/`FONT_SIZE`/`PAGE_SIZE`/`MARGIN`. |

## License

This extension is licensed under the **GNU General Public License v2.0 or later (GPL-2.0-or-later)**.

It statically links [Poppler](https://poppler.freedesktop.org/), which is distributed under the GPL-2.0 license. Any binary distribution of this extension is therefore subject to the GPL-2.0 terms: you must make the corresponding source code available to recipients.

See the [LICENSE](LICENSE) file for the full license text.
