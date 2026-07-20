# read_pdf document-intelligence pipeline

**Generalizable, versioned reference recipe:** folder of PDFs → hybrid search + redaction coordinates, in pure DuckDB.

```
read_pdf (digital + OCR)
    → docs / words
    → FTS (BM25)  +  VSS (HNSW over embeddings)
    → hybrid RRF ranking
    → term → pixel rectangles (redact / highlight)
```

Not hardcoded to any domain. Point `folder` at legal, medical, invoices, or any mixed corpus.

---

## Requirements

| Piece | Detail |
|-------|--------|
| Binary | `./build/release/duckdb` from the duckdb-read_pdf repo root (pdf extension + tesseract OCR linked) |
| Extensions | `fts`, `vss` (core). The `pdf` / `read_pdf` extension is linked into the custom binary (or `LOAD 'build/release/extension/pdf/pdf.duckdb_extension'`). |
| OCR runtime | Tesseract language data (`eng` etc.) on the host (`brew install tesseract tesseract-lang`) |
| Embeddings | Real model via `llm` / UDF preferred; recipe ships a **documented stub** so HNSW + hybrid still run |

This recipe lives under `examples/docpipeline/` in the duckdb-read_pdf tree (SQL + README only).

---

## Quick start

```bash
# From the duckdb-read_pdf repo root (after a release build):
DUCK=./build/release/duckdb

# 1. Put PDFs in a folder (or use shipped fixtures)
ls test/data/*.pdf | head

# 2. Run the full pipeline
$DUCK -c ".read examples/docpipeline/pipeline.sql"
```

### Point at ANY folder

Edit the parameter block at the top of `pipeline.sql`:

```sql
SET VARIABLE folder = '/path/to/your/pdfs';   -- only required change for a new corpus
SET VARIABLE q_keyword  = 'some-PII-or-ID';
SET VARIABLE q_ocr_term = 'SomeName';
SET VARIABLE q_semantic = 'natural language question about the corpus';
SET VARIABLE q_hybrid   = 'mixed keyword + intent terms';
```

Or override from the CLI:

```bash
$DUCK -c "
  SET VARIABLE folder = '/data/contracts';
  .read examples/docpipeline/pipeline.sql
"
```

(`SET VARIABLE` before `.read` is overridden by the script’s own `SET` — for one-shot overrides, edit the script or factor the parameter block into `params.sql`.)

---

## Pipeline stages

### 1. INGEST — `read_pdf` + `read_pdf_words`

```sql
FROM read_pdf(folder || '/*.pdf', auto_ocr := true, ignore_errors := true)
FROM read_pdf_words(folder || '/*.pdf', auto_ocr := true)
```

| Column | Meaning |
|--------|---------|
| `has_text_layer` | Page has a born-digital text layer |
| `used_ocr` | Page was OCR’d (image-only / empty text layer) |
| `word_boxes` | Nested list of `{word, x0, y0, x1, y1, source, confidence}` |
| `source` on words | `'text'` (digital) or `'ocr'` |

**OCR notes**

- `auto_ocr := true` (default behavior in this recipe) OCRs **only** pages without a usable text layer.
- `ocr := true` forces OCR on every page (slower; use for damaged text layers).
- OCR needs tessdata at runtime (`tessdata_dir` param or `TESSDATA_PREFIX` or system paths).
- Scanned OCR can misread glyphs (e.g. `Item` → `ltem`); FTS still matches OCR’d tokens as emitted.

### 2. FTS — BM25 keyword / PII search

```sql
PRAGMA create_fts_index('pages', 'id', 'text', overwrite=1);
SELECT ..., fts_main_pages.match_bm25(id, 'SECRET-CODE-42') AS score ...
```

**When to use FTS**

- Exact or near-exact tokens: names, IDs, invoice numbers, code strings.
- High precision on known strings; ranking is classical BM25.
- Index is a **snapshot** — rebuild after re-ingesting pages.

### 3. VSS — chunk + embed + HNSW

Chunks are page-level by default (fine for short pages; sub-chunk long docs). Embeddings:

1. **Preferred:** real model (`llm` extension, Python UDF, offline batch → `UPDATE`).
2. **Shipped stub:** L2-normalized bag-of-token hashed into `FLOAT[32]` so the index and hybrid SQL are real end-to-end without a model.

```sql
CREATE INDEX chunks_hnsw ON chunks USING HNSW (embedding);
ORDER BY array_distance(embedding, qemb) LIMIT k;
```

**When to use vector search**

- Paraphrase / topical queries (“fruit line items”, “payment terms”).
- Quality is dominated by the embedder — swap the stub for production.

### 4. HYBRID — Reciprocal Rank Fusion (headline)

```text
RRF(d) = 1/(k + rank_fts(d)) + 1/(k + rank_vss(d))   with k = 60
```

One SQL query fuses BM25 ranks and vector ranks at `(file, page)` grain via `FULL OUTER JOIN`. Hits that only appear in one channel still score; hits in both rise to the top.

**When to use hybrid**

- Default retrieval mode for mixed corpora.
- Keyword-ish queries lean FTS; vague queries lean VSS; RRF merges without score calibration.

### 5. REDACTION-COORD JOIN

```sql
SELECT word, x0, y0, x1, y1, source, confidence,
       struct_pack(page := page, x := x0, y := y0, w := x1-x0, h := y1-y0) AS redact_rect
FROM words
WHERE lower(word) = lower(:term);
```

Join any matched term (from FTS/hybrid hit) back to word geometry. Rects are ready for `pdf_redact` or UI highlight overlays. Works for digital `source='text'` **and** OCR `source='ocr'` words.

---

## Layout

```
examples/docpipeline/
  pipeline.sql          # parameterized, commented pipeline (this recipe)
  README.md             # how it works (this file)
  RECIPE.md             # results + yes/partial verdict from the test run
  run_transcript.txt    # clean stage outputs from the end-to-end run
  corpus/               # small mixed digital + scanned test set
  artifacts/            # parquet snapshots (pages, words, chunks, query results)
```

---

## Test corpus

Assembled under `corpus/` from `duckdb-read_pdf/test/data/` fixtures (reproducible, no network):

| File | Kind |
|------|------|
| `01_hello_digital.pdf` | Born-digital |
| `02_two_pages_digital.pdf` | Born-digital, 2 pages |
| `03_redact_secret_digital.pdf` | Born-digital, contains `SECRET-CODE-42` |
| `04_scanned_invoice_ocr.pdf` | **Image-only scan** → `used_ocr=true` |
| `05_table_digital.pdf` | Born-digital table |

---

## Swapping in a real embedder

Replace the stub block in `pipeline.sql` (section 3) with e.g.:

```sql
-- After LOAD llm (when http_request + model are available):
UPDATE chunks SET embedding = llm_embed(chunk_text);  -- shape must match FLOAT[N]
-- or export chunks → external model → reimport parquet and JOIN on chunk_id
```

Keep dimension `N` consistent for query vectors and `CREATE INDEX ... HNSW (embedding)`.

---

## Version

| Field | Value |
|-------|-------|
| Recipe | `quackapi_docpipeline` |
| Version | `1.0.0` |
| DuckDB | v1.5.4 (read_pdf build) |
| Date | 2026-07-19 |
