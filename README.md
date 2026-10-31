# duckdb-pdf

**Everything PDF, in SQL.** A DuckDB community extension (`pdf`) that turns PDFs into tables and tables into PDFs: read text at every grain (page, line, word, layout element, retrieval chunk), inspect metadata / outlines / attachments / form fields / annotations / revisions / digital signatures, and do the everyday document operations people open Adobe Acrobat for — merge, split, rotate, compress, encrypt, decrypt, extract pages, write new PDFs. Built on [Poppler](https://poppler.freedesktop.org/) (parsing/rendering), [Tesseract](https://github.com/tesseract-ocr/tesseract) (OCR for scanned documents), [qpdf](https://qpdf.sourceforge.io/) (document surgery), and [libharu](https://github.com/libharu/libharu) (native PDF writing) — in the DuckDB process. Office conversion through `to_pdf` uses installed Microsoft or Apple apps on macOS, with LibreOffice as a fallback.


This branch consolidates the API into **19 canonical function families**.
Two deprecated compatibility aliases, `read_pdf_elements` and `pdf_to_text`,
remain for downstream migration: the catalog therefore contains **21 names**.
Other retired names are removed. The community registry and released binaries
may lag this source tree.

```sql
INSTALL pdf FROM community;
LOAD pdf;

SELECT filename, page, text FROM read_pdf('docs/*.pdf');
SELECT * FROM read_pdf('report.pdf', grain := 'table');
SELECT * FROM pdf_objects('report.pdf', kind := 'outline');
SELECT pdf_text('report.pdf', {format: 'markdown'});
SELECT pdf_render('report.pdf', 1, {format: 'png', dpi: 150});
SELECT page, png FROM pdf_render('report.pdf', first_page := 1, last_page := 2);
```

See [COOKBOOK.md](COOKBOOK.md) for complete task recipes.

## Public API

Selectors on table functions are constant named arguments, resolved at bind time.
They select a typed schema before planning. Unknown selectors and options that do
not apply to the selected mode fail rather than being silently ignored.
Scalar options are dynamically shaped STRUCTs: the binder checks recognized field
names and field types, and execution checks which fields apply to the mode.

| Name | Form | Purpose |
|---|---|---|
| `read_pdf` | Table | Page, line, element, table or chunk grain. |
| `read_pdf_words` | Table | Positioned words and OCR provenance. |
| `pdf_info` | Table | Basic metadata/permissions or detailed qpdf statistics. |
| `pdf_objects` | Table | Eleven kinds of typed document objects. |
| `pdf_text` | Scalar | Text, Markdown, HTML, XML or qpdf JSON. |
| `pdf_render` | Scalar and table | One encoded page, streamed pages, or page-image export. |
| `pdf_merge` | Scalar | Concatenate documents in list order. |
| `pdf_pages` | Scalar | Extract/reorder pages or rotate a document. |
| `pdf_split` | Table | Split by page or blank separator. |
| `pdf_stamp` | Scalar | Watermark or Bates numbering. |
| `pdf_rewrite` | Scalar | Compress, repair, encrypt or decrypt. |
| `pdf_redact` | Table | Raster redaction with constant boxes and named options. |
| `pdf_redact_lateral` | In-out table | Raster redaction with per-row inputs. |
| `pdf_sign` | Table | Apply a detached CMS signature. |
| `write_pdf` | Scalar | Native text-to-PDF writer. |
| `to_pdf` | Scalar | Office conversion through installed desktop apps or LibreOffice. |
| `ocr_image` | Scalar and table | Image OCR as text or a row with confidence. |
| `pdf_ocr` | Table | Persist searchable OCR layers. |
| `poppler_version` | Scalar | Linked Poppler version. |

`COPY ... TO ... (FORMAT pdf)` remains available in addition to these names.

## Reading

`read_pdf(files, grain := 'page', ...)` accepts a path or glob through DuckDB
filesystems and secrets. The default page schema and `first_page`/`last_page`
contract remain intact. Existing grain-specific column names are preserved;
this consolidation does not silently rename `file` to `filename` or
`page_number` to `page`.

| Selection | Columns |
|---|---|
| `page` | `filename` VARCHAR, `page` INTEGER, `page_count` INTEGER, `text` VARCHAR, `width` DOUBLE, `height` DOUBLE, `has_text_layer` BOOLEAN, `used_ocr` BOOLEAN, `ocr_confidence` DOUBLE, `label` VARCHAR |
| `line` | `filename` VARCHAR, `page` INTEGER, `line` INTEGER, `text` VARCHAR, `label` VARCHAR |
| `element` | `file` VARCHAR, `page_number` INTEGER, `element_idx` INTEGER, `element_type` VARCHAR, `text` VARCHAR, `font_size` DOUBLE, `bbox_x0` DOUBLE, `bbox_y0` DOUBLE, `bbox_x1` DOUBLE, `bbox_y1` DOUBLE, `font_name` VARCHAR |
| `table` | `filename` VARCHAR, `page` INTEGER, `table_index` INTEGER, `row_index` INTEGER, `cells` VARCHAR[] |
| `chunk` | `file` VARCHAR, `chunk_idx` INTEGER, `text` VARCHAR, `page_start` INTEGER, `page_end` INTEGER, `n_chars` INTEGER, `heading` VARCHAR |
| `words` | `filename` VARCHAR, `page` INTEGER, `word` VARCHAR, `x0` DOUBLE, `y0` DOUBLE, `x1` DOUBLE, `y1` DOUBLE, `font_name` VARCHAR, `font_size` DOUBLE, `source` VARCHAR, `confidence` DOUBLE, `line` INTEGER, `column_index` INTEGER, `page_width` DOUBLE, `page_height` DOUBLE, `label` VARCHAR |

The `words` row above belongs to the separate `read_pdf_words(files, ...)`
function. Its boxes use displayed-page points with a top-left origin; its
`line` and `column_index` fields group words geometrically. `label` is a
printed page label, which can be NULL.

| Parameter | Type | Default | Description |
|---|---|---|---|
| `password` | VARCHAR | — | Password for encrypted files. |
| `first_page` / `last_page` | INTEGER | full document | 1-based inclusive page range. |
| `layout` | VARCHAR | `'auto'` | page/line grains only: geometry-based `'auto'`, or Poppler `'reading'`, `'physical'` (column-preserving), or `'raw'` (content-stream order). |
| `parse_tables` | BOOLEAN | false | On `read_pdf`, select physical layout. Accepted for compatibility but ignored by other readers. Use `read_pdf(..., grain := 'table')` for table cells. |
| `force` | BOOLEAN | false | table grain only: return whitespace grids that fail its confidence gate. |
| `ocr` | BOOLEAN | false | Force OCR on every page, even pages with a text layer. |
| `auto_ocr` | BOOLEAN | true | OCR pages that have no extractable text layer. |
| `ocr_language` | VARCHAR | `'eng'` | Tesseract model or `+`-separated model combination, such as `'eng+fra'`; all selected models must exist in the resolved model directory. |
| `ocr_dpi` | INTEGER | 300 | Render DPI for the OCR raster (1–2400). |
| `ocr_psm` / `ocr_oem` | INTEGER | 3 / 3 | Page segmentation mode (automatic) / OCR engine mode (default). |
| `ocr_backend` | VARCHAR | `'tesseract'` | `'tesseract'`, `'vision'` (Apple Vision, macOS only; see [OCR](#ocr)) or `'external'`; external requires a plugin. Persisted `pdf_ocr` requires Tesseract. |
| `ocr_plugin` | VARCHAR | — | External OCR shared library implementing the C ABI in `src/ocr_ops.hpp`. |
| `ocr_endpoint` | VARCHAR | — | Reserved; does not send HTTP requests. |
| `ocr_preprocess` | BOOLEAN | true | Run the Leptonica preprocessing pipeline (grayscale → deskew → binarize → despeckle) on the rendered page before OCR. |
| `ocr_retry` | BOOLEAN | true | Re-render low-confidence pages (mean confidence < 55) at 2× DPI when the render was below 400 DPI, keeping whichever pass scores higher. |
| `tessdata_dir` | VARCHAR | auto-detected | Explicit Tesseract model directory (see [OCR](#ocr)). |
| `ocr_vars` | MAP(VARCHAR, VARCHAR) | — | Tess `SetVariable` key/values applied after Init (same surface as CLI `-c name=value`). |
| `ocr_config` | VARCHAR | — | Path or tessdata config name for `ReadConfigFile` (applied after Init, before `ocr_vars`). |
| `ignore_errors` | BOOLEAN | false | page grain, basic census, outline and structure kinds: skip files that fail their reader's guarded load/extraction stage. |

Element and chunk grains accept the OCR options above, plus `password`, `first_page`, and `last_page` (chunk grain also has `chunk_size` / `overlap`). Their legacy default remains native-only; set `auto_ocr := true` or `ocr := true` to include OCR words.



Grain selection keeps each reader's behavior. Page/line/word/table readers can
auto-OCR blank text layers by default; element/chunk OCR is opt-in.
`chunk_size` (default 1200) and `overlap` (default 150) are byte budgets;
`n_chars` counts UTF-8 codepoints. Table extraction combines ruling-line and
word-spacing heuristics; `force` bypasses its whitespace-table confidence gate.
Element types are heading, paragraph, list_item and other.

The DuckDB 2.0 native multi-file scan opens and reads files through async
read-ahead, then decodes on query workers. Projection on the page reader and
basic census skips unnecessary work. `async_threads = 0` or
`read_ahead_depth = 0` selects synchronous reading. Interrupted reads check the
native deadline between bounded byte reads; an in-flight HTTP request follows
its transport timeout and retries. Windows serializes Poppler critical sections.

## Inspection

`pdf_info(files, detail := 'basic', password := '', ignore_errors := false)`
returns the 33-column census. Missing metadata is NULL, and basic projection
only inspects requested fields:

```text
file, title, author, subject, keywords, creator, producer, creation_date, mod_date,
page_count, is_encrypted, is_linearized, pdf_version, width, height, file_size,
pdfa_part, pdfa_conformance, is_locked, perm_print, perm_print_high_resolution,
perm_change, perm_copy, perm_add_notes, perm_fill_forms, perm_accessibility,
perm_assemble, has_javascript, form_type, page_mode, page_layout, permanent_id, update_id
```

`detail := 'qpdf'` returns qpdf's document statistics: page/object/xref counts,
encryption methods and permission flags, password match flags, linearization
checks and warning count. It accepts `password`; it rejects `ignore_errors`,
which this implementation does not support.

The qpdf schema is:

```text
file VARCHAR, is_linearized BOOLEAN, linearized_ok BOOLEAN, page_count INTEGER,
object_count BIGINT, xref_total BIGINT, xref_free BIGINT, is_encrypted BOOLEAN,
enc_R INTEGER, enc_P INTEGER, stream_method VARCHAR, string_method VARCHAR,
file_method VARCHAR, owner_password_matched BOOLEAN, user_password_matched BOOLEAN,
allow_accessibility BOOLEAN, allow_extract BOOLEAN, allow_print_low BOOLEAN,
allow_print_high BOOLEAN, allow_modify_assembly BOOLEAN, allow_modify_form BOOLEAN,
allow_modify_annotation BOOLEAN, allow_modify_other BOOLEAN, allow_modify_all BOOLEAN,
warning_count BIGINT
```

`pdf_objects(files, kind := ...)` requires one of the following kinds.
All use native file scheduling and retain the prior object-specific schema:

| Selection | Columns |
|---|---|
| `outline` | `file` VARCHAR, `ord` INTEGER, `depth` INTEGER, `title` VARCHAR, `page` INTEGER |
| `structure` | `file` VARCHAR, `ord` INTEGER, `depth` INTEGER, `parent_ord` INTEGER, `tag` VARCHAR, `role` VARCHAR, `page` INTEGER, `alt` VARCHAR, `actual_text` VARCHAR, `lang` VARCHAR, `mcids` INTEGER[] |
| `destinations` | `file` VARCHAR, `name` VARCHAR, `type` VARCHAR, `page` INTEGER, `left` DOUBLE, `bottom` DOUBLE, `right` DOUBLE, `top` DOUBLE, `zoom` DOUBLE, `change_left` BOOLEAN, `change_top` BOOLEAN, `change_zoom` BOOLEAN |
| `fonts` | `file` VARCHAR, `page` INTEGER, `name` VARCHAR, `type` VARCHAR, `embedded` BOOLEAN, `subset` BOOLEAN, `font_file` VARCHAR |
| `form_fields` | `file` VARCHAR, `page` INTEGER, `field_name` VARCHAR, `field_type` VARCHAR, `value` VARCHAR, `is_required` BOOLEAN |
| `annotations` | `file` VARCHAR, `page` INTEGER, `subtype` VARCHAR, `contents` VARCHAR, `uri` VARCHAR, `rect_x0` DOUBLE, `rect_y0` DOUBLE, `rect_x1` DOUBLE, `rect_y1` DOUBLE |
| `attachments` | `file` VARCHAR, `name` VARCHAR, `description` VARCHAR, `size` BIGINT, `mime_type` VARCHAR, `data` BLOB |
| `images` | `file` VARCHAR, `page` INTEGER, `image_index` INTEGER, `name` VARCHAR, `width` INTEGER, `height` INTEGER, `bits_per_component` INTEGER, `colorspace` VARCHAR, `format` VARCHAR, `data` BLOB |
| `signatures` | `file` VARCHAR, `field_name` VARCHAR, `subfilter` VARCHAR, `signing_time` TIMESTAMP, `signer_name` VARCHAR, `reason` VARCHAR, `location` VARCHAR, `covers_whole_file` BOOLEAN, `verified` BOOLEAN |
| `revisions` | `revision_index` INTEGER, `startxref_offset` BIGINT, `eof_offset` BIGINT, `size_bytes` BIGINT, `is_incremental` BOOLEAN, `filename` VARCHAR |
| `pages` | `file` VARCHAR, `page` INTEGER, `page_count` INTEGER, `width` DOUBLE, `height` DOUBLE, `media_width` DOUBLE, `media_height` DOUBLE, `crop_width` DOUBLE, `crop_height` DOUBLE, `rotation` INTEGER, `orientation` VARCHAR, `label` VARCHAR, `duration` DOUBLE |

Password is supported for outline, structure, destinations, fonts, attachments,
images, signatures, revisions and pages. Outline/structure accept `ignore_errors`;
structure also accepts page bounds. Annotations and form_fields currently do not
accept a password. A missing form value or widget page is NULL.
`verified` checks CMS integrity, not certificate trust; whole-file coverage is
reported separately. Revisions run oldest first. Image `format` describes the
encoded or raw `data`; page rendering is a different operation.

## Scalar conversion and rendering

```sql
SELECT pdf_text('document.pdf');
SELECT pdf_text(content, {format: 'text', layout: 'physical'}) FROM read_blob('document.pdf');
SELECT pdf_text('document.pdf', {format: 'markdown'});
SELECT pdf_text('document.pdf', {format: 'json', password: 'secret'});
SELECT pdf_render('document.pdf', 1, {dpi: 200});
SELECT decode(pdf_render('document.pdf', 1, {format: 'svg'}));
```

`pdf_text(input [, options]) -> VARCHAR` accepts paths and BLOBs. The default
format is text, with layout reading. Text layout accepts auto, reading, physical
or raw. HTML/XML/JSON also accept password; layout is text-only. Markdown uses
font/table geometry, performs no OCR and currently requires a path without a
password option.

`pdf_render(input, page [, options]) -> BLOB` accepts paths or BLOBs. Pages are
one-based; options are `format` (png or svg, default png) and `dpi` (1–2400,
default 150). Both formats return encoded bytes. SVG embeds a raster PNG rather
than vector page operators; `decode(...)` turns those UTF-8 SVG bytes into text.

The table form uses the same public name:

```sql
SELECT page, png FROM pdf_render('docs/*.pdf', dpi := 150);
SELECT page, svg FROM pdf_render('document.pdf', format := 'svg');
SELECT * FROM pdf_render(['a.pdf', 'b.pdf'], output_dir := 'previews', format := 'png');
```

In-memory rows contain `file, page, page_count, dpi, width, height, png`
(`png BLOB`), or a final `svg VARCHAR` column for SVG. Page bounds and
password are supported. Projection skips rendering if only page metadata is
selected, and rendered rows stream one page at a time.

With `output_dir`, rows are `file, page, out_path, width, height, bytes`.
Output is written beneath `output_dir/<stem>/pN.png` or `pN.svg`; missing
directories are created and duplicate input stems are rejected. The list input
overload requires `output_dir`. Export side effects run during execution, so
EXPLAIN does not write files. Width/height report raster pixels.

## Document operations

Writers act on local paths, check external-access permissions and publish each
output atomically for qpdf document operations, redaction, signing and persisted
OCR. Native text/COPY authoring writes directly. Required NULL scalar arguments propagate NULL. A scalar
option field set to NULL also propagates NULL after its name/mode is checked,
except image OCR optional NULL fields retain their default behavior.

```sql
SELECT pdf_merge(['a.pdf', 'b.pdf'], 'merged.pdf');
SELECT pdf_pages('merged.pdf', 'excerpt.pdf', '1-3,7');
SELECT pdf_pages('document.pdf', 'rotated.pdf', {rotate: 90});
SELECT pdf_pages('document.pdf', 'part.pdf', '1-3', {rotate: 90, rotate_pages: '1'});
SELECT * FROM pdf_split('document.pdf', 'pages');
SELECT * FROM pdf_split('batch.pdf', 'parts', mode := 'blank', blank_threshold := 0.995);
SELECT pdf_stamp('document.pdf', 'draft.pdf', {mode: 'watermark', text: 'DRAFT', opacity: 0.3});
SELECT pdf_stamp('document.pdf', 'numbered.pdf', {mode: 'bates', prefix: 'DOC-', start: 1});
SELECT pdf_rewrite('document.pdf', 'small.pdf', {mode: 'compress'});
SELECT pdf_rewrite('document.pdf', 'locked.pdf', {mode: 'encrypt', password: 'reader', owner_password: 'owner'});
SELECT pdf_rewrite('locked.pdf', 'plain.pdf', {mode: 'decrypt', password: 'reader'});
SELECT pdf_rewrite('damaged.pdf', 'repaired.pdf', {mode: 'repair'});
```

Whole-document `pdf_pages(input, output, {rotate: ...})` preserves the original
document catalog, metadata and bookmarks. Optional `rotate_pages` defaults to
all. The ranges overload extracts/reorders pages and then applies any rotation
in the extracted output's page coordinates. Rotation must be a multiple of 90.
Ranges use qpdf syntax, including `z` for the last page and `r2` for the
second-to-last page.

Page splitting returns `page, file`; blank splitting omits separators and
returns `document, first_page, last_page, page_count, file`. The output
directory must exist. Blank threshold must be in (0, 1].
Compression is structural, without image downsampling. Encryption uses AES-256.
Repair accepts an optional password. Watermark opacity defaults to 0.30 and must
be in (0, 1]. Bates identifiers use a prefix and at least six decimal digits.

`pdf_redact(input, output, boxes, dpi := 200, password := '')` accepts a list of
`{page, x, y, w, h}` boxes in original PDF user-space points with a bottom-left
origin. This differs from word-reader coordinates. It replaces affected pages
with raster content and removes their underlying text; any redaction also drops
forms, annotations, attachments, encryption and document metadata. It is not a
content-stream redaction/sanitization engine. `pdf_redact_lateral` supports
per-row inputs with fixed dpi 200 and empty password. Both return per-page rows.
Their columns are `page INTEGER, redacted BOOLEAN, boxes_applied INTEGER`.

`pdf_sign(input, output, certificate, key, ...)` uses PEM files and supports
`key_password, reason, location, signer_name, field_name, password`. Existing
signed documents are rejected because incremental signing is not implemented.
It returns `output VARCHAR, field_name VARCHAR`.

`write_pdf(content [, output_path])` typesets text in-process with libharu;
without a path it returns a temporary file path. `COPY ... (FORMAT pdf)`
typesets query rows with TITLE, AUTHOR, HEADER, FOOTER, FONT_SIZE, PAGE_SIZE and
MARGIN options. FONT_SIZE/MARGIN must be finite and pass their range checks.

`to_pdf(input_path [, output_path])` converts through installed applications,
without a shell. On macOS it tries Excel → Numbers → LibreOffice for spreadsheets,
Word → Pages → LibreOffice for documents, and PowerPoint → Keynote → LibreOffice
for presentations. Native `.numbers`, `.pages` and `.key` inputs go directly to
their Apple application before LibreOffice; other formats and other platforms use
LibreOffice. Missing applications and failed exports advance to the next backend.
If all attempts fail, the error retains each backend's failure.

macOS discovers apps by bundle identifier through Launch Services. Desktop export
uses AppleScript and may require Automation permission and an interactive login.
It opens a private input copy, closes only that copy, and never quits the app.
Each native export has a 110-second AppleScript timeout and the converter process
has a 120-second timeout. A valid PDF header is required before atomic publication.
Omitted output replaces the input suffix with .pdf. PDF reading, rendering and
native PDF writing do not require an Office application.

## OCR

`ocr_image(image_blob [, options]) -> VARCHAR` is the scalar form for per-row
OCR. Options are language (eng), psm, oem, tessdata_dir, preprocess (true), vars
(a MAP(VARCHAR,VARCHAR)), config, dpi (300), format (text, hocr or tsv), and
backend (tesseract or vision).
The table form `ocr_image(image_blob, language := 'eng', ...)` returns one row
`text, confidence, format` and accepts named options, including its existing
lang/ocr_language and ocr_* spellings. Its input must be foldable; use the scalar
form when the BLOB comes from a row.

```sql
SELECT ocr_image(content, {language: 'eng', psm: 6}) FROM read_blob('scan.png');
SELECT * FROM pdf_ocr('scan.pdf', 'searchable.pdf', first_page := 2, last_page := 5);
```

### Apple Vision (macOS)

`ocr_backend := 'vision'` on the readers, and `backend: 'vision'` on
`ocr_image`, use Apple's on-device text recognizer (`VNRecognizeTextRequest`,
accurate level, language correction on) instead of Tesseract. It needs no model
files. Vision picks its own compute device (CPU, GPU or Neural Engine), so
there is no device option.

Speed depends on threads. Vision handles one page at a time per machine, so it
runs at about 210 ms per page (300 dpi, render included) whatever `threads` is.
Tesseract is about five times slower per page but scales with threads: in one
137-page run it took 1,060 ms per page on 1 thread, 329 on 4 and 218 on 15,
level with Vision. Choose Vision for fast OCR on a few threads, for languages
without installed Tesseract models, or to leave the cores free for other work.
Accuracy was similar: Vision found more words (97% against 96%) with slightly
more character errors (9.5% against 8.5%).

```sql
SELECT page, text FROM read_pdf('scan.pdf', ocr := true, ocr_backend := 'vision');
SELECT word, x0, y0, x1, y1, confidence
FROM read_pdf_words('scan.pdf', ocr := true, ocr_backend := 'vision');
SELECT ocr_image(content, {backend: 'vision'}) FROM read_blob('photo.jpg');
```

- `ocr_language` takes Tesseract codes (`eng`, `fra`, `deu`, `chi_sim`, ...),
  which map to Vision's languages, or Vision's own names (`en-US`, `fr-FR`);
  `+` selects several. An unsupported language is an error that lists the ones
  this system supports.
- Confidence is the mean of Vision's per-line confidence on a 0–100 scale. It is
  not comparable with Tesseract's, so `ocr_retry` does not re-render Vision pages.
- `psm`, `oem`, `tessdata_dir`, `ocr_preprocess`, `ocr_vars` and `ocr_config`
  are Tesseract settings and have no effect. `format` must be `text`, and
  `pdf_ocr` (the searchable-PDF writer) needs Tesseract.
- Image BLOBs are decoded by ImageIO (PNG, JPEG, TIFF, HEIC, ...), and EXIF
  orientation is honored.
- On other platforms, choosing `vision` is an error.

English tessdata is bundled. Resolution is explicit tessdata_dir, then
TESSDATA_PREFIX, package-manager paths, and bundled English. An invalid explicit
directory does not fall back. Combined languages such as eng+fra need every
selected model. Explicit OCR fails when its model is missing; implicit reader
auto-OCR is best-effort. Other languages need installed models.

Reader OCR uses preprocessing and a low-confidence retry. Positioned-word and
persisted text layers avoid deskew so coordinates remain aligned with the page.
`ocr_vars` overrides Tesseract settings after config loading; unknown variables
are ignored by Tesseract. `ocr_endpoint` is reserved and sends no HTTP request.

`pdf_ocr` honors page bounds but returns one status row per document page.
Existing text pages are skipped unless force is true. Force adds another layer
alongside original text and can produce duplicated extracted text. It requires
Tesseract; reader compatibility knobs ocr, auto_ocr and parse_tables do not
control this writer. Its output is a rewrite, not an incremental byte update.
Its status schema is `page INTEGER, had_text_layer BOOLEAN, ocr_applied BOOLEAN,
words INTEGER, mean_confidence DOUBLE, seconds DOUBLE, out_path VARCHAR`.

## Migration from 48 names

`read_pdf_elements(files, ...)` and `pdf_to_text(input [, layout])` are deprecated
compatibility aliases. The former preserves the element schema, streaming file
reader, password, page bounds and OCR options; the latter preserves all four
path/BLOB overloads, the optional layout string and NULL propagation. Both share
the canonical implementations. Prefer `read_pdf(files, grain := 'element', ...)`
and `pdf_text(input [, {layout: ...}])` in new code.

These two aliases remain throughout 0.9.x. Removal is planned no earlier than
1.0, after downstream releases have migrated, and will be announced in that
release's migration notes. Catalog descriptions mark every overload deprecated.

| Previous names | Replacement |
|---|---|
| `read_pdf_lines`, `read_pdf_elements`, `read_pdf_tables`, `pdf_chunks` | `read_pdf(..., grain := 'line'/'element'/'table'/'chunk')` |
| `pdf_qpdf_info` | `pdf_info(..., detail := 'qpdf')` |
| `pdf_outline`, `pdf_structure`, `pdf_destinations`, `pdf_fonts`, `pdf_form_fields`, `pdf_annotations`, `pdf_attachments`, `pdf_images`, `pdf_signatures`, `pdf_revisions`, `pdf_pages_info` | `pdf_objects(..., kind := ...)`; see the kind/schema table. |
| `pdf_to_text`, `pdf_to_markdown`, `pdf_to_html`, `pdf_to_xml`, `pdf_json` | `pdf_text(input, {format: ...})`; BLOB support retained except Markdown. |
| `pdf_to_png`, `pdf_to_svg` | Scalar `pdf_render(input, page, {format: ... , dpi: ...})`; SVG now returns BLOB bytes, so use decode for VARCHAR. |
| `pdf_page_images`, `pdf_write_page_images` | Table `pdf_render(files, ...)`; export directory is now the named output_dir option. |
| `pdf_rotate` | `pdf_pages(input, output, {rotate: degrees, rotate_pages: range})` |
| `pdf_split_blank` | `pdf_split(..., mode := 'blank')` |
| `pdf_watermark`, `pdf_bates` | `pdf_stamp(input, output, {mode: ... , ...})` |
| `pdf_compress`, `pdf_repair`, `pdf_encrypt`, `pdf_decrypt` | `pdf_rewrite(input, output, {mode: ... , ...})` |
| `tesseract_ocr` | Scalar `ocr_image(blob, {language: ... , ...})` |

The preceding 52-to-48 change also removed read_pdf_layout (use read_pdf_words),
poppler_render_page (use scalar pdf_render), and read_pdf_meta/pdf_permissions
(use pdf_info). For read_pdf_meta callers: filename became file, pages became
page_count, encrypted became is_encrypted, and absent metadata became NULL.

No sniff_pdf or pdf_help was added. Queryable help is already in the catalog:

```sql
SELECT function_name, function_type, parameters, parameter_types,
       return_type, description, examples
FROM duckdb_functions()
WHERE function_name IN ('read_pdf', 'read_pdf_words', 'pdf_info', 'pdf_objects', 'pdf_text', 'pdf_render', 'pdf_merge', 'pdf_pages', 'pdf_split', 'pdf_stamp', 'pdf_rewrite', 'pdf_redact', 'pdf_redact_lateral', 'pdf_sign', 'write_pdf', 'to_pdf', 'ocr_image', 'pdf_ocr', 'poppler_version')
ORDER BY function_name, function_type, len(parameters);
```

Each registered overload has named positional parameters, descriptions and
examples. Table options are typed catalog parameters. Scalar STRUCT fields are
validated but are not expanded into individual catalog parameters.

## Installation and building

Build this tree with **DuckDB v2.0.0**. Signed community artifacts depend on the
registry's published version and can expose an older API. Unsigned source builds:

```bash
git clone --recurse-submodules https://github.com/asubbarao/duckdb-pdf.git
cd duckdb-pdf
make release
make test
```

The built CLI links `pdf` into the process. When using a separate matching
DuckDB CLI, enable unsigned extensions at startup and load the build explicitly:

```bash
duckdb -unsigned
```

```sql
LOAD 'build/release/extension/pdf/pdf.duckdb_extension';
```

macOS dependencies: poppler, tesseract, leptonica, libharu, qpdf, openssl.
Linux packages: libpoppler-dev, libpoppler-cpp-dev, libtesseract-dev,
libleptonica-dev, libhpdf-dev, libqpdf-dev, libssl-dev. CI distribution builds
use vcpkg for native dependencies and static linking. Supported build targets
are linux_amd64/arm64, osx_amd64/arm64 and windows_amd64 (MSVC).
WebAssembly, mingw/rtools and Windows ARM64 are not supported targets.

## Scope and license

This extension provides deterministic extraction, rendering, OCR and document
operations. It does not provide ML layout understanding, reliable merged-cell or
complex borderless-table reconstruction, PDF/A conformance validation, or
content-stream redaction. PDF/A metadata detection is not validation.

GPL-2.0-or-later. Distribution of binaries linked with Poppler must provide the
corresponding source. See [LICENSE](LICENSE).
