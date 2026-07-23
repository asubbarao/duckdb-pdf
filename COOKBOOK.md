# duckdb-pdf Cookbook

Task-oriented recipes for the `pdf` community extension. Each recipe is a real
goal, a runnable SQL block, and the one gotcha that bites people. For the full
per-function reference (every column, every parameter), see [README.md](README.md).

Every recipe assumes the extension is loaded:

```sql
INSTALL pdf FROM community; LOAD pdf;
```

Every function takes a single path **or a glob**, so the natural unit of work is
a folder of PDFs. Recipes below use `docs/*.pdf`; substitute your own path.

---

## 1. RAG ingestion: a folder of PDFs → embeddable, section-aware chunks

**Goal:** turn a directory of PDFs into one table where every row is a
retrieval chunk that already knows its filename, page span, and section heading —
ready to embed.

```sql
-- One statement. pdf_chunks packs the layout-element grain into chunks,
-- keeps headings glued to their section, and overlaps chunk boundaries.
CREATE TABLE rag_chunks AS
FROM pdf_chunks('docs/*.pdf', chunk_size := 1200, overlap := 150);

SELECT file, chunk_idx, heading, page_start, page_end, n_chars
FROM rag_chunks
ORDER BY file, chunk_idx;
```

Every chunk carries `heading` (nearest preceding heading — `NULL` before the
first one) plus `page_start`/`page_end`, so a retrieved hit is citation-ready
without a second lookup. Add your embedding column and index with the core `vss`
extension:

```sql
ALTER TABLE rag_chunks ADD COLUMN embedding FLOAT[384];
-- UPDATE rag_chunks SET embedding = <your_embedder>(text);  -- you bring the model

INSTALL vss; LOAD vss;
SET hnsw_enable_experimental_persistence = true;
CREATE INDEX chunks_hnsw ON rag_chunks USING HNSW (embedding);
```

**Gotcha:** the extension ships **no ML** — it does not embed or call models.
`pdf_chunks` is deterministic (no tokenizer); it reads the native text layer
only. For scanned/image-only PDFs it produces empty chunks — OCR those first
(recipe 3) into a text column and chunk that instead. The full runnable RAG
script is in [`examples/rag_pdf_vss.sql`](examples/rag_pdf_vss.sql).

---

## 2. Full-text search over a document set

**Goal:** BM25 keyword search across a folder, returning file + page for each hit.

```sql
INSTALL fts; LOAD fts;

-- Materialize one row per page, with a stable integer document key
CREATE TABLE pages AS
SELECT row_number() OVER () AS id, filename, page, text
FROM read_pdf('docs/*.pdf');

-- Build the FTS index over the text column, keyed by id
PRAGMA create_fts_index('pages', 'id', 'text');

-- Query it: match_bm25 scores each doc; NULL means "no match"
SELECT filename, page, score
FROM (
  SELECT filename, page, fts_main_pages.match_bm25(id, 'liquor') AS score
  FROM pages
)
WHERE score IS NOT NULL
ORDER BY score DESC
LIMIT 10;
```

**Gotcha:** `create_fts_index` is a **static snapshot** — it does not update when
you insert new pages. Re-run the `PRAGMA` after changing the base table. The key
column (`id` here) must be a single column and unique per row. For a quick
one-off search without an index, skip FTS entirely and use
`WHERE text ILIKE '%revenue%'` on `read_pdf` (or `read_pdf_lines` to keep line
context).

---

## 3. Scanned-invoice extraction: OCR + table structure from image-only PDFs

**Goal:** pull line-item tables out of a scanned (no text layer) invoice.

```sql
-- read_pdf_tables runs OCR on image-only pages when ocr := true,
-- then detects tables over the OCR word geometry
SELECT page, table_index, row_index, cells
FROM read_pdf_tables('scanned.pdf', ocr := true)
ORDER BY page, table_index, row_index;

-- Words with confidence, for building your own extraction geometry
SELECT page, word, x0, y0, confidence
FROM read_pdf_words('scanned.pdf', ocr := true)
WHERE source = 'ocr'
ORDER BY page, y1 DESC, x0;
```

**English is bundled** (tessdata_fast eng, extracted on first OCR) — zero host
tessdata install for the default language. Other languages need a model on
disk (`brew install tesseract-lang`, `apt-get install tesseract-ocr-deu`, …)
or an explicit path:

```sql
SELECT page, text
FROM read_pdf('scan.pdf', ocr := true, tessdata_dir := '/opt/models/tessdata');
```

Resolution: `tessdata_dir` → `TESSDATA_PREFIX` → standard paths → **bundled eng**.
`auto_ocr` is on by default (OCR only pages with no text layer); `ocr := true`
forces OCR on every page.

---

## 4. Find and quarantine broken PDFs in a folder

**Goal:** scan a folder without one corrupt/password-protected file aborting
everything, then get the list of files that failed.

```sql
-- ignore_errors skips unopenable files instead of aborting the scan
SELECT filename, page, text
FROM read_pdf('docs/*.pdf', ignore_errors := true)
WHERE text ILIKE '%invoice%';

-- The skipped set stays recoverable: everything the glob saw, MINUS
-- everything the scan actually read = the broken files
SELECT file FROM glob('docs/*.pdf')
EXCEPT
SELECT DISTINCT filename FROM read_pdf('docs/*.pdf', ignore_errors := true);
```

**Gotcha:** without `ignore_errors := true`, **one bad file aborts the whole
scan** (the default). `ignore_errors` is accepted only by `read_pdf` and
`read_pdf_meta`. Encrypted files count as "unopenable" unless you pass the
`password`, so a password-protected file shows up in the quarantine list too.

---

## 5. Redaction & signature forensics

**Goal:** find out whether a signed contract was altered after signing, and
whether the signature itself is intact.

```sql
-- Was this file edited after it was first written? PDFs are append-only;
-- each edit adds an incremental-update revision (revision 0 = original).
SELECT count(*) - 1 AS updates_after_original
FROM pdf_revisions('contract.pdf');

-- Timeline of what each edit added, oldest first
SELECT revision_index, size_bytes, eof_offset, is_incremental
FROM pdf_revisions('contract.pdf')
ORDER BY revision_index;

-- Signature integrity: verified = the signed bytes are unaltered.
-- covers_whole_file = false means the file grew AFTER signing.
SELECT file, signer_name, signing_time, verified, covers_whole_file
FROM pdf_signatures('contracts/*.pdf');

-- The dangerous case: a valid signature that no longer covers the file
SELECT file, field_name
FROM pdf_signatures('contracts/*.pdf')
WHERE verified AND NOT covers_whole_file;
```

**Gotcha:** `verified` is a real OpenSSL CMS check of **integrity only** — it
does *not* validate certificate-chain trust (no CA store is consulted). A
self-signed or expired cert can still report `verified = true`. And a signature
can be `verified = true` while `covers_whole_file = false`: the signature is
valid over its own byte range, but later incremental revisions changed the
visible document. Cross-check with `pdf_revisions` to recover what the document
said before the last edit (truncate the file at an earlier `eof_offset`).

---

## 6. Bulk document surgery

**Goal:** the everyday Acrobat operations — merge, split, stamp — across a set.

```sql
-- Merge a whole folder into one PDF (build the list in SQL, then compress)
SELECT pdf_compress(
         pdf_merge(list(file ORDER BY file), 'combined.pdf'),
         'combined_small.pdf')
FROM glob('docs/*.pdf');

-- Split one PDF into single-page files (dir must EXIST — see gotcha)
SELECT page, file FROM pdf_split('combined.pdf', 'out');
```

**Split a scanned batch on blank separator pages** *(requires pdf ≥ 0.5)* — a
mailroom scan is often N documents divided by blank sheets:

```sql
-- Detects separators (no text AND a near-white raster) and writes each
-- logical document to out/<stem>_doc<K>.pdf, dropping the blanks.
SELECT document, first_page, last_page, page_count, file
FROM pdf_split_blank('batch.pdf', 'out');

-- Raise the threshold to demand a purer-white separator (default 0.995)
SELECT * FROM pdf_split_blank('batch.pdf', 'out', blank_threshold := 0.999);
```

**Stamp Bates numbers across a discovery set** *(requires pdf ≥ 0.5)*:

```sql
-- prefix + zero-padded (min 6 digits) sequential number, bottom-right,
-- incrementing per page: page 1 -> ACME000100, page 2 -> ACME000101, ...
SELECT pdf_bates('deposition.pdf', 'deposition_stamped.pdf', 'ACME', 100);
```

**Watermark drafts** *(requires pdf ≥ 0.5)*:

```sql
-- Large 45° diagonal gray text, centered on every page. Real selectable
-- text (searchable), not a flattened raster. 4-arg form sets fill alpha.
SELECT pdf_watermark('report.pdf', 'report_draft.pdf', 'DRAFT');
SELECT pdf_watermark('report.pdf', 'report_faint.pdf', 'CONFIDENTIAL', 0.15);
```

**Gotcha:** these qpdf writers **overwrite** an existing output file but treat a
missing output *directory* as an error (never created — `mkdir` first), and
**refuse in-place operation** (`input == output`) because qpdf reads the source
lazily during the write. Every scalar returns the output path, so they compose:
the output of `pdf_merge` is the input of `pdf_compress`, as above.

---

## 7. PDF → images and thumbnails

**Goal:** feed pages to a vision model, or pull the original embedded rasters
back out of a scan.

```sql
-- Render a page to PNG bytes for a vision model (DPI controls raster size)
SELECT pdf_to_png('report.pdf', 1)       AS page1_150dpi,
       pdf_to_png('report.pdf', 1, 300)  AS page1_300dpi;

-- BLOB per page (path or glob) for vision / llm pipelines
SELECT file, page, png FROM pdf_page_images('docs/*.pdf', dpi := 100);
```

Columns from `pdf_page_images`: `file`, `page` (1-based), `page_count`, `dpi`,
`width`, `height` (pixel size from the PNG IHDR), `png` (`BLOB`). DPI range
1–2400 (default 150). Feed `png` straight to a vision / llm extension, or keep
it as a column for downstream SQL.

### Closure-style static tree: `pages/<stem>/pN.png` (no `pdftoppm`)

Static UIs and file servers want a **deterministic on-disk tree** of page PNGs.
Use `pdf_write_page_images` — one call, multi-file, same raster + bundled
base-14 fonts as `pdf_page_images` / `pdf_to_png`:

```sql
-- Writes pages/<stem>/p1.png, p2.png, … (1-based, no zero-pad)
-- Creates out_dir and per-stem subdirs if missing.
SELECT file, page, out_path, width, height, bytes
FROM pdf_write_page_images('docs/*.pdf', 'pages', dpi := 100);

-- Page range + password (same named params as the readers)
SELECT *
FROM pdf_write_page_images('locked.pdf', 'pages', dpi := 100,
                           password := 'secret', first_page := 1, last_page := 3);
```

Columns: `file`, `page` (1-based), `out_path`, `width`, `height`, `bytes`.
Overwrite replaces existing PNGs. Side effects run at scan time (`EXPLAIN`
writes nothing). Requires pdf ≥ 0.7.7.

For a **single** page without a tree, the scalar + core `COPY` still works:

```sql
COPY (SELECT pdf_to_png('report.pdf', 1, 100))
TO 'pages/report/p1.png' (FORMAT BLOB);  -- mkdir parent first
```

### Render vs extract

```sql
-- Pull the ACTUAL stored rasters (the JPEG the scanner wrote), not a render
SELECT file, page, image_index, width, height, colorspace, format,
       octet_length(data) AS bytes
FROM pdf_images('scans/*.pdf')
ORDER BY file, page, image_index;
```

**Gotcha:** `pdf_page_images` / `pdf_write_page_images` / `pdf_to_png` **render**
the page (1-based page numbers, DPI 1–2400) — they are rasterizers. `pdf_images`
**extracts** the embedded image XObjects byte-for-byte and does not render
anything; files with no images yield zero rows. Check the `format` column
before using `data`: `jpeg`/`png` bytes drop straight into any image tool, but
`ccitt` (fax) and `raw` (CMYK/indexed/exotic depth) rows need the
`colorspace`/`bits_per_component`/`width`/`height` columns to interpret.
`pdf_images` requires pdf ≥ 0.5; `pdf_write_page_images` requires pdf ≥ 0.7.7.

### Fonts (pdf ≥ 0.7.3)

From **0.7.3**, community binaries **bundle URW base-14 substitute fonts**
(Helvetica / Times / Courier / Symbol / ZapfDingbats) and register them with
Poppler at load. Page previews, OCR rasters, `pdf_redact`, and
`pdf_write_page_images` should **not** come back blank on font-starved hosts
(vcpkg/community Poppler without fontconfig or system display fonts). Exotic
non-base-14 faces can still fail; standard office PDFs are the happy path.

---

## 8. Reporting: typeset a query result as a PDF

**Goal:** produce a formatted PDF from a query, or convert an Office doc.

```sql
-- Any query result -> a typeset PDF, with header/footer and page numbers
COPY (SELECT file, page_count, file_size FROM pdf_info('docs/*.pdf') ORDER BY file)
TO 'census.pdf' (FORMAT pdf,
                 TITLE 'Folder census',
                 AUTHOR 'duckdb-pdf',
                 PAGE_SIZE 'a4',
                 FONT_SIZE 11,
                 HEADER 'Internal',
                 FOOTER 'confidential - page {page}');

-- Author a PDF from a string, in-process (libharu, no external tools)
SELECT write_pdf('Hello native PDF from DuckDB.', 'out.pdf');

-- Convert a rich Office/markup document (docx, odt, xlsx, html, ...)
SELECT to_pdf('resume.docx');   -- -> 'resume.pdf'
```

**Gotcha:** `COPY ... (FORMAT pdf)` and `write_pdf` use libharu with **Base-14
Helvetica only** (WinAnsi/Latin-1) — non-Latin scripts and many UTF-8 glyphs
will not render; use `to_pdf` + HTML for international text. `FOOTER` supports a
literal `{page}` placeholder; if you set `HEADER`/`FOOTER`, `MARGIN` must be
≥ 24 to fit the band. `to_pdf` is the **only** function that shells out at
runtime — it needs LibreOffice installed (`brew install --cask libreoffice` /
`apt-get install libreoffice`) and errors with an actionable message if absent.

---

## 9. Compose with httpfs: render bytes fetched from a URL

**Goal:** process a PDF you never wrote to disk — straight from S3 or HTTPS.

```sql
INSTALL httpfs; LOAD httpfs;

-- Path form: the scalars route through DuckDB's FileSystem, so a URL works
SELECT pdf_to_text('https://example.com/report.pdf') AS body;

-- BLOB form: read_blob fetches bytes, the scalar consumes them directly
-- (no second filesystem round-trip). The BLOB overloads accept raw bytes.
SELECT pdf_to_text(content)          AS body,
       octet_length(pdf_to_png(content, 1)) AS page1_png_bytes
FROM read_blob('https://example.com/report.pdf');
```

**Gotcha:** the **scalar** readers (`pdf_to_text`/`html`/`xml`/`svg`/`png`)
accept a `BLOB`, so `read_blob(url) → scalar` needs no temp file. The **table**
functions (`read_pdf`, `pdf_chunks`, `read_pdf_tables`, ...) and the qpdf writers
take a **path or glob, not a BLOB** — for those, give the URL as the path (they
route through the VFS) rather than piping bytes. The qpdf writers additionally
require a **local** output path.
