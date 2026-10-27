# Document search and highlight pipeline

This example composes the current source build's `read_pdf` and
`read_pdf_words` with FTS, VSS and reciprocal rank fusion. It produces page
text, word boxes, search results and UI highlight rectangles as Parquet files.
The embedding is a deterministic hashed-token stub that exercises the vector
pipeline; it does not provide semantic understanding.

## Run from the repository root

Build with `make release`, then prepare a small valid corpus and output directory.
Do not point the default example at all of `test/data`: that directory includes
deliberately malformed and encrypted regression fixtures.

```bash
mkdir -p build/docpipeline-corpus build/docpipeline-artifacts
cp test/data/hello.pdf test/data/two_pages.pdf test/data/redact_secret.pdf \
   test/data/scanned.pdf test/data/table.pdf build/docpipeline-corpus/
./build/release/duckdb -bail -f examples/docpipeline/pipeline.sql
```

The built CLI links the PDF extension. The script installs and loads matching
`fts` and `vss` artifacts, so first use needs network access. English OCR is
bundled; other languages require the selected models on disk.

Edit the parameter block in [pipeline.sql](pipeline.sql) to select another
`folder`, an existing writable `out_dir`, and search terms. Values set before
running the file are overwritten by that block. An unreadable file is skipped
by the page reader's `ignore_errors`; the word reader still fails on it.
Use a corpus whose files both readers can open.

## Stages and outputs

| Stage | Result |
|---|---|
| Ingest | `docs` / `pages` hold text, page dimensions, OCR provenance and nested word boxes; `words` holds positioned words. |
| FTS | `q_fts` / `q_fts_ocr` rank keyword hits using BM25. |
| VSS | `chunks` contains page-sized text and normalized `FLOAT[32]` hashed-token vectors; `q_vss` ranks distances. |
| Hybrid | `q_hybrid` combines keyword and vector ranks with reciprocal rank fusion, using k = 60. |
| Highlight | `q_coords` and `highlight_payload` locate matched words for UI overlays. |
| Export | Nine Parquet snapshots are written under `out_dir`; reruns replace them. |

The script synthesizes `word_boxes` by joining the separate word reader onto
page rows; `read_pdf` itself does not return a nested word-box column.
Automatic OCR reads image-only pages, and `ocr := true` can force OCR when
needed. Misrecognition remains possible; inspect confidence and source text.

FTS indexes are snapshots. This script uses `overwrite=1` when rebuilding.
Page keys are assigned in file/page order. Hybrid fusion uses a full outer join,
so a result can come from either ranking channel.

## Coordinates

`x0/y0/x1/y1`, `width_pt/height_pt` and `highlight_rect` use displayed-page
**points with a top-left origin**. A viewer scales them by its pixels-per-point
ratio. They are not directly valid `pdf_redact` inputs: that function expects
original PDF user-space points with a bottom-left origin. Crop offsets and page
rotation must also be handled before redaction. This example writes no redacted PDF.

## Replace the embedding stub

Replace the chunk and query embedding computations together, keeping the
dimension consistent in the `FLOAT[32]` casts and HNSW index. Supply real model
vectors via your own UDF or an external batch joined by `chunk_id`.
The script has no dependency on an `llm_embed` function or an external model.
Token hashing is a demonstration of data flow, not a semantic retrieval benchmark.

## Files and scope

Only this README and `pipeline.sql` are tracked here. The quick start creates
its corpus and snapshots under ignored `build/`; no stored test transcript or
historical benchmark is shipped. The host baseline is DuckDB v2.0.0.
