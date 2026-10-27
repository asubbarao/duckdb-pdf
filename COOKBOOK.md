# duckdb-pdf Cookbook

Task-oriented recipes for the `pdf` community extension. Each recipe is a real
goal, a runnable SQL block, and the one gotcha that bites people. For the full
per-function reference (every column, every parameter), see [README.md](README.md).

These recipes target this source branch's 19 canonical function families; two
deprecated aliases bring the catalog to 21 names. Released community binaries
may expose the earlier API. Load the matching source build (see README), or use
the following once the matching version is published:

```sql
INSTALL pdf FROM community; LOAD pdf;
```

The multi-file readers accept a path or glob. Scalar readers consume one path
or BLOB; document writers take individual paths or an explicit input list.
Recipes below use `docs/*.pdf`; substitute your own path.

---

## 1. RAG ingestion: a folder of PDFs → embeddable, section-aware chunks

**Goal:** turn a directory of PDFs into one table where every row is a
retrieval chunk that already knows its filename, page span, and section heading —
ready to embed.

```sql
-- One statement. The chunk grain packs layout elements into chunks,
-- keeps headings glued to their section, and overlaps chunk boundaries.
CREATE TABLE rag_chunks AS
FROM read_pdf('docs/*.pdf', grain := 'chunk', chunk_size := 1200, overlap := 150);

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
`read_pdf(..., grain := 'chunk')` is deterministic (no tokenizer); by default it
reads the native text layer. Set `auto_ocr := true` to include image-only pages,
or persist OCR first (recipe 11). The RAG template in
[`examples/rag_pdf_vss.sql`](examples/rag_pdf_vss.sql) needs your embedder and query vector.

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
`WHERE text ILIKE '%revenue%'` on `read_pdf` (or `read_pdf(..., grain := 'line')` to keep line
context).

---

## 3. Scanned-invoice extraction: OCR + table structure from image-only PDFs

**Goal:** pull line-item tables out of a scanned (no text layer) invoice.

```sql
-- The table grain runs OCR on image-only pages when ocr := true,
-- then detects tables over the OCR word geometry
SELECT page, table_index, row_index, cells
FROM read_pdf('scanned.pdf', grain := 'table', ocr := true)
ORDER BY page, table_index, row_index;

-- Words with confidence, for building your own extraction geometry
SELECT page, word, x0, y0, confidence
FROM read_pdf_words('scanned.pdf', ocr := true)
WHERE source = 'ocr'
ORDER BY page, y0, x0;
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

### Expert escape hatches (vars / config / shellfs CLI)

Same doctrine as the rest of the extension: **add parameters, not functions**.

**In-process Tess `SetVariable` + config** (shared by `read_pdf*` and
scalar `ocr_image`):

```sql
-- Digits-only OCR for a scanned amount field
SELECT page, text
FROM read_pdf('invoice_scan.pdf', ocr := true, ocr_psm := 7,
  ocr_vars := MAP {
    'tessedit_char_whitelist': '0123456789.,$',
    'classify_bln_numeric_mode': '1'
  });

-- Named tess config (searches tessdata/configs) then override
SELECT page, word, confidence
FROM read_pdf_words('scan.pdf', ocr := true,
  ocr_config := 'digits',
  ocr_vars := MAP {'tessedit_char_blacklist': '|'});

-- Scalar OCR accepts a BLOB column and options as a STRUCT.
SELECT ocr_image(pdf_render(content, 1, {dpi: 200}),
  {language: 'eng', psm: 6, oem: 3, tessdata_dir: '', preprocess: true,
   vars: MAP {'tessedit_char_whitelist': 'ABC0123456789'}, config: ''})
FROM read_blob('scan.pdf');
```

**Shellfs CLI hatch** when you need the full Tesseract binary (env, obscure
flags, alternate builds) before they land as in-process params. One CTE pipe —
not a product `.sh`:

```sql
-- After writing a page PNG to a path the host tesseract can see:
SELECT content AS text
FROM read_text(
  'TESSDATA_PREFIX=/opt/tessdata tesseract /tmp/page.png stdout -l eng --psm 4 -c load_system_dawg=0 |'
);

SELECT * FROM read_text(
  'tesseract /tmp/page.png stdout -l deu+eng --tessdata-dir /usr/share/tessdata |'
);
```

Prefer `ocr_vars` / `ocr_config` when the knob exists; use shellfs only for
true CLI gaps (and leave a debt note if the gap should become a named param).

**`ocr_image` TVF** — named params + HOCR/TSV without leaving the process.
First arg must be foldable (no subqueries):

```sql
SET VARIABLE page_png = (
  SELECT pdf_render(content, 1, {dpi: 200}) FROM read_blob('scan.pdf')
);
SELECT text, confidence, format
FROM ocr_image(
  getvariable('page_png'),
  language := 'eng',
  psm := 6,
  format := 'hocr',   -- text | hocr | tsv
  vars := MAP {'tessedit_char_whitelist': '0123456789'}
);

-- page mean confidence on PDF OCR
SELECT page, used_ocr, ocr_confidence, left(text, 80)
FROM read_pdf('scan.pdf', ocr := true);
```

---

## 3b. One Poppler walk: words + lines + page size (mark / detect packs)

**Goal:** pin word boxes for highlights **and** line text for scoring without
two full opens and without DIY `round(y0)` line clustering or `max(bbox)` page
geometry.

```sql
-- Single pin. line is geometric (the same rule as the element grain).
CREATE TABLE pdf_words AS
SELECT filename, page AS page_no, line AS line_no,
       word, struct_pack(x0 := x0, y0 := y0, x1 := x1, y1 := y1) AS bbox, font_size,
       page_width AS width_pt, page_height AS height_pt
FROM read_pdf_words('docs/*.pdf');

-- Line grain free from the word pin (no second reader walk)
CREATE TABLE pdf_lines AS
SELECT filename, page_no, line_no,
       string_agg(word, ' ' ORDER BY bbox.x0) AS line_text
FROM pdf_words
GROUP BY filename, page_no, line_no;

-- True page geometry (not max word extent)
CREATE TABLE pages AS
SELECT DISTINCT filename, page_no, width_pt, height_pt
FROM pdf_words;
```

**Gotcha:** geometric `line` is the join key for bbox work. `read_pdf(..., grain := 'line')`
(geometry text for `layout := 'auto'`, Poppler text for other layouts) can differ on multi-column pages —
use it when you only need grep-style line context without boxes. Prefer
`pdf_objects(..., kind := 'pages')` when you need rotation/media box **without** extracting words.
Raster previews: `pdf_render('docs/*.pdf', output_dir := 'pages', dpi := 100)` —
do not shell `pdftoppm` (bundled base-14 fonts fix blank rasters on community
builds).

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
scan** (the default). `ignore_errors` is accepted by `read_pdf`,
`pdf_info`, `pdf_objects(..., kind := 'outline')`, and `pdf_objects(..., kind := 'structure')`. Encrypted files count as "unopenable" unless you pass the
`password`, so a password-protected file shows up in the quarantine list too.

---

## 5. Redaction & signature forensics

**Goal:** find out whether a signed contract was altered after signing, and
whether the signature itself is intact.

```sql
-- Count retained incremental updates (revision 0 = original).
-- A full rewrite can remove revision history; not every PDF edit is append-only.
SELECT count(*) - 1 AS updates_after_original
FROM pdf_objects('contract.pdf', kind := 'revisions');

-- Timeline of what each edit added, oldest first
SELECT revision_index, size_bytes, eof_offset, is_incremental
FROM pdf_objects('contract.pdf', kind := 'revisions')
ORDER BY revision_index;

-- Signature integrity: verified = the signed bytes are unaltered.
-- covers_whole_file = false means the file grew AFTER signing.
SELECT file, signer_name, signing_time, verified, covers_whole_file
FROM pdf_objects('contracts/*.pdf', kind := 'signatures');

-- The dangerous case: a valid signature that no longer covers the file
SELECT file, field_name
FROM pdf_objects('contracts/*.pdf', kind := 'signatures')
WHERE verified AND NOT covers_whole_file;
```

**Gotcha:** `verified` is a real OpenSSL CMS check of **integrity only** — it
does *not* validate certificate-chain trust (no CA store is consulted). A
self-signed or expired cert can still report `verified = true`. And a signature
can be `verified = true` while `covers_whole_file = false`: the signature is
valid over its own byte range, but later incremental revisions changed the
visible document. Cross-check with `pdf_objects(..., kind := 'revisions')` to recover what the document
said before the last edit (truncate the file at an earlier `eof_offset`).

---

## 6. Bulk document surgery

**Goal:** the everyday Acrobat operations — merge, split, stamp — across a set.

```sql
-- Merge a whole folder into one PDF (build the list in SQL, then compress)
SELECT pdf_rewrite(
         pdf_merge(list(file ORDER BY file), 'combined.pdf'),
         'combined_small.pdf', {mode: 'compress'})
FROM glob('docs/*.pdf');

-- Split one PDF into single-page files (dir must EXIST — see gotcha)
SELECT page, file FROM pdf_split('combined.pdf', 'out');
```

**Split a scanned batch on blank separator pages** — a
mailroom scan is often N documents divided by blank sheets:

```sql
-- Detects separators (no text AND a near-white raster) and writes each
-- logical document to out/<stem>_doc<K>.pdf, dropping the blanks.
SELECT document, first_page, last_page, page_count, file
FROM pdf_split('batch.pdf', 'out', mode := 'blank');

-- Raise the threshold to demand a purer-white separator (default 0.995)
SELECT * FROM pdf_split('batch.pdf', 'out', mode := 'blank', blank_threshold := 0.999);
```

**Stamp Bates numbers across a discovery set**:

```sql
-- prefix + zero-padded (min 6 digits) sequential number, bottom-right,
-- incrementing per page: page 1 -> ACME000100, page 2 -> ACME000101, ...
SELECT pdf_stamp('deposition.pdf', 'deposition_stamped.pdf', {mode: 'bates', prefix: 'ACME', start: 100});
```

**Watermark drafts**:

```sql
-- Large 45° diagonal gray text, centered on every page. Real selectable
-- text (searchable), not a flattened raster. Opacity sets fill alpha.
SELECT pdf_stamp('report.pdf', 'report_draft.pdf', {mode: 'watermark', text: 'DRAFT'});
SELECT pdf_stamp('report.pdf', 'report_faint.pdf', {mode: 'watermark', text: 'CONFIDENTIAL', opacity: 0.15});
```

**Gotcha:** these qpdf writers **overwrite** an existing output file but treat a
missing output *directory* as an error (never created — `mkdir` first), and
**refuse in-place operation** (`input == output`) because qpdf reads the source
lazily during the write. Every scalar returns the output path, so they compose:
the output of `pdf_merge` is the input of `pdf_rewrite(..., {mode: 'compress'})`, as above.

---

## 7. PDF → images and thumbnails

**Goal:** feed pages to a vision model, or pull the original embedded rasters
back out of a scan.

```sql
-- Render a page to PNG bytes for a vision model (DPI controls raster size)
SELECT pdf_render('report.pdf', 1)       AS page1_150dpi,
       pdf_render('report.pdf', 1, {dpi: 300})  AS page1_300dpi;

-- BLOB per page (path or glob) for vision / llm pipelines
SELECT file, page, png FROM pdf_render('docs/*.pdf', dpi := 100);
```

Columns from `pdf_render`: `file`, `page` (1-based), `page_count`, `dpi`,
`width`, `height` (pixel size from the PNG IHDR), `png` (`BLOB`). DPI range
1–2400 (default 150). Feed `png` straight to a vision / llm extension, or keep
it as a column for downstream SQL.

### Closure-style static tree: `pages/<stem>/pN.png` (no `pdftoppm`)

Static UIs and file servers want a **deterministic on-disk tree** of page PNGs.
Use `pdf_render(..., output_dir := 'pages')` to write multiple files using the
same raster and bundled base-14 fonts as the in-memory and scalar forms:

```sql
-- Writes pages/<stem>/p1.png, p2.png, … (1-based, no zero-pad)
-- Creates out_dir and per-stem subdirs if missing.
SELECT file, page, out_path, width, height, bytes
FROM pdf_render('docs/*.pdf', output_dir := 'pages', dpi := 100);

-- Page range + password (same named params as the readers)
SELECT *
FROM pdf_render('locked.pdf', output_dir := 'pages', dpi := 100,
                           password := 'secret', first_page := 1, last_page := 3);
```

Columns: `file`, `page` (1-based), `out_path`, `width`, `height`, `bytes`.
Overwrite replaces existing PNGs. Side effects run at scan time (`EXPLAIN`
writes nothing). Use `format := 'svg'` to write SVG pages instead.

For a **single** page without a tree, the scalar + core `COPY` still works:

```sql
COPY (SELECT pdf_render('report.pdf', 1, {dpi: 100}))
TO 'pages/report/p1.png' (FORMAT BLOB);  -- mkdir parent first
```

### Render vs extract

```sql
-- Pull the ACTUAL stored rasters (the JPEG the scanner wrote), not a render
SELECT file, page, image_index, width, height, colorspace, format,
       octet_length(data) AS bytes
FROM pdf_objects('scans/*.pdf', kind := 'images')
ORDER BY file, page, image_index;
```

**Gotcha:** The scalar and table forms of `pdf_render` **render**
the page (1-based page numbers, DPI 1–2400) — they are rasterizers. `pdf_objects(..., kind := 'images')`
**extracts** the embedded image XObjects byte-for-byte and does not render
anything; files with no images yield zero rows. Check the `format` column
before using `data`: `jpeg`/`png` bytes drop straight into any image tool, but
`ccitt` (fax) and `raw` (CMYK/indexed/exotic depth) rows need the
`colorspace`/`bits_per_component`/`width`/`height` columns to interpret.

### Fonts (pdf ≥ 0.7.3)

From **0.7.3**, community binaries **bundle URW base-14 substitute fonts**
(Helvetica / Times / Courier / Symbol / ZapfDingbats) and register them with
Poppler at load. Page previews, OCR rasters, `pdf_redact`, and
`pdf_render(..., output_dir := 'pages')` should **not** come back blank on font-starved hosts
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
≥ 24 to fit the band. `to_pdf` is the **only** function that launches an external
application at runtime (without a shell). On macOS it prefers the matching
Microsoft app, then Numbers/Pages/Keynote, and finally LibreOffice. Other
platforms use LibreOffice (`brew install --cask libreoffice` /
`apt-get install libreoffice`). Failed exports fall back, with all backend errors
retained if none succeeds. Native macOS apps require Automation permission.

### Native document content with panduck

For a newly typeset text PDF, compose panduck's native readers with `write_pdf`.
This path needs no Office application. It extracts content rather than preserving
the source document's pagination, fonts, images or spreadsheet/presentation layout.
The native writer's character/font limitations above still apply.

```sql
INSTALL panduck FROM community;
LOAD panduck;
INSTALL duck_block_utils FROM community;
LOAD duck_block_utils;
SELECT write_pdf(doc_render('report.docx', 'text'), 'report-content.pdf');
```

The same composition works for panduck-supported ODT, RTF and EPUB inputs.
Panduck is an optional companion, not a dependency of `pdf` or a rendering
backend for `to_pdf`. It does not replace XLSX/PPTX conversion.

---

## 9. Compose with httpfs: render bytes fetched from a URL

**Goal:** process a PDF you never wrote to disk — straight from S3 or HTTPS.

```sql
INSTALL httpfs; LOAD httpfs;

-- Path form: the scalars route through DuckDB's FileSystem, so a URL works
SELECT pdf_text('https://example.com/report.pdf') AS body;

-- BLOB form: read_blob fetches bytes, the scalar consumes them directly
-- (no second filesystem round-trip). The BLOB overloads accept raw bytes.
SELECT pdf_text(content)          AS body,
       octet_length(pdf_render(content, 1)) AS page1_png_bytes
FROM read_blob('https://example.com/report.pdf');
```

**Gotcha:** the **scalar** readers (`pdf_text` and `pdf_render`)
accept a `BLOB`, so `read_blob(url) → scalar` needs no temp file. Markdown output
from `pdf_text` currently requires a path. The **table**
functions (`read_pdf`, `read_pdf(..., grain := 'chunk')`, `read_pdf(..., grain := 'table')`, ...) and the qpdf writers
take a **path or glob, not a BLOB** — for those, give the URL as the path (they
route through the VFS) rather than piping bytes. The qpdf writers additionally
require a **local** output path.

---

## 10. Digest a long document: its contents, section by section

**Goal:** before reading a 100-page manual, see its shape: every section in
order, where it starts, how big it is, and how it opens. One statement, built
from the word grain and the bookmarks.

```sql
WITH words AS (
  FROM read_pdf_words('https://blobs.duckdb.org/docs/ducklake-docs.pdf')
), lines AS (       -- read_pdf_words numbers lines per page, already in column order
  SELECT page, line, string_agg(word, ' ' ORDER BY x0) AS text,
         len(list(DISTINCT page) OVER ()) AS n_pages
  FROM words GROUP BY page, line
), furniture AS (   -- running headers and footers: the same text on over half the pages
  SELECT text FROM lines GROUP BY text, n_pages HAVING len(list(DISTINCT page)) > n_pages / 2
), body AS (
  SELECT page, line, text FROM lines ANTI JOIN furniture USING (text)
), anchors AS (     -- a bookmark starts at the line on its page that prints its title
  SELECT o.ord, o.depth, o.title, o.page, coalesce(min(b.line), 0) AS line
  FROM pdf_objects('https://blobs.duckdb.org/docs/ducklake-docs.pdf', kind := 'outline') o
  LEFT JOIN body b ON b.page = o.page AND b.text = o.title
  GROUP BY o.ord, o.depth, o.title, o.page
), sections AS (    -- each line belongs to the deepest bookmark at or before it
  SELECT a.ord, a.depth, a.title, a.page, b.page AS at_page, b.line, b.text
  FROM body b ASOF JOIN anchors a
    ON (b.page, b.line, 2147483647) >= (a.page, a.line, a.ord)
)
SELECT repeat('  ', depth) || title AS contents, page,
       len(list(text)) AS lines, sum(length(text)) AS chars,
       list(text ORDER BY at_page, line) FILTER (WHERE length(text) > 40)[1] AS opening
FROM sections GROUP BY ord, depth, title, page ORDER BY ord;
```

```
contents                          page  lines  chars  opening
  Summary                            6      9    467  This document contains DuckLake's documentation in a single-file …
    Introduction                     9     18    900  This page contains the specification for the DuckLake format, version 1.0.
    Data Types                      10    272  10942  DuckLake specifies multiple different data types for field values, …
    Queries                         17    404  16284  This page explains the queries issued to the DuckLake catalog database …
      ducklake_column_mapping       30     13    587  Mappings contain the information used to map Parquet fields to column ids …
```

`pdf_objects(..., kind := 'outline').page` is the page each bookmark points at, so the contents join
straight onto the text. Anchoring a bookmark to the line that prints its title,
not just its page, keeps two sections that share a page apart. Keying the ASOF
join on `ord` as well gives a shared line to the deeper bookmark.

**Gotcha:** a document without bookmarks returns no rows from `pdf_objects(..., kind := 'outline')`.
Take the anchors from `read_pdf(..., grain := 'element')` instead
(`WHERE element_type = 'heading'`, with `page_number` and the element's text).
For the printed page number rather than the physical index, join
`pdf_objects(file, kind := 'pages')` on `page` and use its `label`.
The runnable script is [`examples/digest_long_document.sql`](examples/digest_long_document.sql).

---

## 11. Scanned documents: OCR once, then read the result natively

**Goal:** turn a scan into a PDF with a real text layer, so every reader works on it
quickly and no query pays for OCR twice.

```sql
-- Supported controls include force, first_page/last_page, password,
-- ocr_language/dpi/psm/oem/preprocess/retry, tessdata_dir, ocr_vars/config.
-- One row per page: page, had_text_layer, ocr_applied, words, mean_confidence, seconds, out_path
FROM pdf_ocr('scans/invoice.pdf', 'scans/invoice.searchable.pdf');

-- The copy is an ordinary text PDF: no OCR option needed from here on.
SELECT page_number, element_type, text FROM read_pdf('scans/invoice.searchable.pdf', grain := 'element');
SELECT chunk_idx, page_start, n_chars    FROM read_pdf('scans/invoice.searchable.pdf', grain := 'chunk');
```

Pages that already have a text layer retain their content (`ocr_applied = false`); `force := true`
OCRs every selected page. `first_page` / `last_page` limit OCR selection while status
rows still cover the whole document. The document is rewritten, not byte-preserved.
The compatibility options `ocr`, `auto_ocr` and `parse_tables` do not control this writer.
OCR runs through Tesseract and the text layer is Tesseract's own invisible one,
spliced into the original with qpdf, so links, outline and metadata survive.

**Gotcha:** this is best-effort OCR for clean printed text. On the repository's scanned invoice
fixture the run takes about a quarter of a second and reads every row, but "Item" comes back as
"ltem": check `mean_confidence` and do not treat the output as authoritative for handwriting or
anything where a misread is costly. For a one-off look without writing a file,
`read_pdf_words(..., ocr := true)` still works, and `read_pdf(..., grain := 'element')` takes the same OCR options.

---

## 12. Tagged PDFs: read the author's own structure instead of guessing it

**Goal:** many PDFs (PowerPoint and Word exports, anything PDF/UA) carry a structure tree that
says what each piece is: heading, paragraph, list item, table cell, figure. `read_pdf(..., grain := 'element')`
infers those from font size and position; `pdf_objects(..., kind := 'structure')` reads what the file declares.

```sql
-- pdf_objects(files, kind := 'structure', password, first_page, last_page, ignore_errors)
-- One row per structure element, depth-first: file, ord, depth, parent_ord, tag, role, page,
--   alt, actual_text, lang, mcids INTEGER[]   (role = tag after the document's /RoleMap)
SELECT role, len(list(ord)) AS n
FROM pdf_objects('https://blobs.duckdb.org/events/duckcon7/floyd-berndsen-ducklake-on-hetzner.pdf', kind := 'structure')
GROUP BY role ORDER BY n DESC;
```

An untagged file returns no rows, so the same query tells you which documents have a tree. Where
one exists it is exact: on a tagged conference deck the tree has far more headings (`H1`) per page
than the geometric guess finds, because a slide title is often no larger than the type around it.

**Gotcha:** `pdf_objects(..., kind := 'structure')` returns the structure, not its text. `mcids` are the marked-content
ids that belong to each element, which is what a later step needs to attach text; until then, join
the tags to `read_pdf(..., grain := 'element')` by page. Compare the two on your own documents before choosing one.
