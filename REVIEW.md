# Repository assessment — 2026-09-07

Reviewed baseline: `f67a767e4cc134f85eee6578124d251b72a84c73` in `asubbarao/duckdb-pdf`.
This is a source review, not a completed security audit or performance benchmark.

The extension has substantial breadth already: native extraction at multiple grains,
OCR, rendering, inspection, qpdf transforms, signing, redaction and PDF writing.
The next investment should make that surface more predictable and composable.
There is a useful SQLLogicTest suite with malformed-input, concurrency, OCR and
round-trip coverage. Several recent capabilities have outpaced the documentation.

## Immediate fixes selected

1. **Preserve Unicode at tiny chunk budgets.** `ChunkSplitLongText` backs up to a
   UTF-8 boundary, but when the budget is smaller than one codepoint it falls back
   to cutting that codepoint. This can emit invalid DuckDB VARCHAR values. Allow
   one complete codepoint to exceed the byte budget and cover 1/2/3-byte budgets.
2. **Reject ambiguous page-image exports before writing.** The exporter maps
   inputs to `<out_dir>/<stem>/pN.png` and truncates existing outputs. Distinct
   files with the same stem, or repeated list inputs, collide. Preflight the
   complete input set before filesystem mutations while retaining the current
   layout for unambiguous input.
3. **Document the actual registered API.** Inspection/repair/JSON surfaces exist
   beyond the main reference. Path, glob and BLOB support varies by overload;
   blanket statements about every function are inaccurate. Keep examples aligned
   with registrations and existing tests.

## Next engineering work, in priority order

The baseline's [latest CI run](https://github.com/asubbarao/duckdb-pdf/actions/runs/33291819318)
failed formatting and the Linux amd64 test job: 47 of 48 cases passed, with a
failure at `test/sql/pdf_sign.test:52` (custom signature field verification).
Linux arm64 succeeded; macOS and Windows jobs were skipped in that run. Restore
that baseline and confirm the actual platform matrix before making a new release.
The signing failure's root cause has not yet been established by this review.

| Opportunity | Evidence and impact | Suggested acceptance criterion |
|---|---|---|
| Stream page previews | `PdfPageImagesScan` fills a vector with every selected page's PNG before emitting rows. Memory and first-row latency grow with the whole document. | Keep a document and page cursor; bound buffered raster bytes; benchmark first-row latency and peak memory on a long scan. |
| Bound input/raster allocation | `ReadAllBytes` allocates the reported file size before `LoadDoc` enforces Poppler's ~2 GiB limit. Rendering checks DPI but has no visible pixel-area guard in `RenderPageToPngBytes`. | Reject unsupported byte sizes before allocation; checked pixel arithmetic and a documented raster budget; oversized-page regression fixtures. |
| Audit optimizer semantics for writers | File-writing scalar registrations such as `write_pdf` and `pdf_merge` use ordinary `ScalarFunction` constructors with no explicit volatility in the registration block. | Verify behavior under EXPLAIN, constant folding, repeated expressions and prepared queries; make side-effect semantics explicit using the pinned DuckDB API. |
| Consistent batch error reporting | `ignore_errors` is registered on `read_pdf` and `read_pdf_meta`, but not the other main extraction grains. Skipping files gives no structured reason. | Shared documented error policy across readers, with file/error diagnostics and strict mode preserved by default. |
| Smaller implementation modules | `src/pdf_extension.cpp` is over 300 KB and combines binders, layout heuristics, OCR integration, writers and registration. | Extract one subsystem at a time behind narrow headers; unchanged SQL schemas and passing regression suite. |
| Release/documentation consistency | The roadmap still describes the community cut as in progress while the README describes installation of the broad API. | Verify the published artifact's function inventory and version, then distinguish released features from main-only ones. |

## Feature buildout worth pursuing

- **OCR-aware elements and retrieval chunks.** Those readers currently use native
  text only, unlike page/word extraction. Reuse OCR word boxes with explicit source
  and confidence metadata, then test mixed native/scanned documents and heading
  behavior. This fills a practical ingestion gap without adding ML layout models.
- **Composable lateral readers.** The redaction API already has a dependent-join
  form. Apply that pattern selectively to document/element/chunk readers where
  file paths come from query rows. Preserve input identity and test multi-chunk,
  NULL and empty-input behavior.
- **Table-quality metadata.** Expose detector type and geometric evidence so users
  can route uncertain extractions for review; do not call heuristics calibrated
  confidence probabilities without an evaluation dataset.
- **Shared document access, after measurement.** Prefer deriving line text from
  the existing word/line grain first. If repeated parsing remains material, design
  a query-scoped cache with memory bounds, file identity and password isolation.

Content-stream redaction and PDF/A validation remain substantial separate projects.
They need dedicated conformance/forensic corpora and clearly defined guarantees;
they should not be treated as small additions to an already broad API.

## Additional legwork completed this pass

- `pdf_merge` now refuses output paths that match any input path. This aligns it
  with existing writer safety checks in `qpdf_ops` and prevents clobbering inputs.
  Commit: `90ef17a` (on `codex/pdf-review-improvements`)
  - Test result: `./build/release/test/unittest test/sql/qpdf_ops.test` run from the
    dedicated Grok branch passed with the added regression.

- `RenderPageToPngBytes` now encodes PNGs in memory rather than write/read temp
  files. This removes the per-page temp-file I/O path from render/write callers.
  Commit: `045fac9` (on `codex/pdf-review-improvements`)
  - Test result: `test/sql/pdf_to_png.test` and
    `test/sql/pdf_write_page_images.test` passed on the dedicated Grok branch.

- Feature scan result from Grok legwork: `write_pdf_table` / `COPY ... FORMAT pdf,
  LAYOUT 'table'` is the top shippable feature, followed by a scoped PDF/A
  validation surface and true content-stream redaction as a longer-term track.
  The quick table-path patch is now proposed as follow-up priority.

## Validation and delivery

The selected tasks are delegated to isolated Grok worktrees and then merged into
this branch. No public push, PR, issue, or other human-facing GitHub communication
is authorized by this local assessment alone.
