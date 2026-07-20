-- =============================================================================
-- read_pdf document-intelligence pipeline (examples/docpipeline) v1.0.0
-- GENERALIZABLE folder-of-PDFs document intelligence in pure DuckDB + read_pdf
--
-- Stages:
--   1. INGEST   read_pdf (digital + auto-OCR) + word boxes
--   2. FTS      BM25 full-text index
--   3. VSS      chunk + embed (llm OR documented stub) + HNSW
--   4. HYBRID   Reciprocal Rank Fusion (BM25 + vector)  << headline
--   5. REDACT   matched term -> pixel rectangles via word_boxes
--
-- Parameterization: set :folder (or the SET below) to ANY corpus directory.
-- Not hardcoded to a domain (legal / medical / invoices / …).
--
-- Binary (read_pdf extension + tesseract OCR built in):
--   ./build/release/duckdb   (from the duckdb-read_pdf repo root after `make`)
--
-- Run (from repo root):
--   ./build/release/duckdb -c ".read examples/docpipeline/pipeline.sql"
--   # or open the binary and: .read examples/docpipeline/pipeline.sql
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 0. EXTENSIONS + PARAMETERS
-- -----------------------------------------------------------------------------
-- read_pdf is linked into the custom build; no INSTALL needed for that binary.
-- fts / vss ship as core extensions.
LOAD fts;
LOAD vss;

-- Optional: real embeddings via llm (skipped when unavailable — see stub below).
-- INSTALL llm FROM community; LOAD llm;

-- >>> PARAMETER: point this at any folder of PDFs <<<
SET VARIABLE folder = 'test/data';  -- any folder of PDFs; shipped fixtures work out of the box
-- Output artifact root (versioned recipe lives alongside this script)
SET VARIABLE out_dir = '/tmp/read_pdf_docpipeline_artifacts';  -- change to a writable path

-- Embedding dimension for the stub (change if you swap in a real model)
SET VARIABLE emb_dim = 32;
-- RRF constant (classic Cormack et al. default)
SET VARIABLE rrf_k = 60;
-- Demo query strings (edit for your corpus)
SET VARIABLE q_keyword = 'SECRET-CODE-42';
SET VARIABLE q_ocr_term = 'Cherries';
SET VARIABLE q_semantic = 'fruit invoice line items apples bananas cherries prices';
SET VARIABLE q_hybrid = 'Cherries secret liquor';

-- Persist HNSW into on-disk DBs (safe no-op for in-memory)
SET hnsw_enable_experimental_persistence = true;

-- -----------------------------------------------------------------------------
-- 1. INGEST: folder glob -> docs + words
--    Digital pages extract the text layer; image-only pages auto-OCR.
-- -----------------------------------------------------------------------------
CREATE OR REPLACE TABLE docs AS
SELECT
  filename                                              AS file,
  page,
  page_count,
  text,
  width,
  height,
  has_text_layer,
  used_ocr,
  -- per-page aggregate confidence from word boxes (filled after words join)
  CAST(NULL AS DOUBLE)                                  AS avg_confidence
FROM read_pdf(getvariable('folder') || '/*.pdf',
              auto_ocr := true,
              ignore_errors := true)
ORDER BY file, page;

CREATE OR REPLACE TABLE words AS
SELECT
  filename                                              AS file,
  page,
  word,
  x0, y0, x1, y1,
  font_name,
  font_size,
  source,                                               -- 'text' | 'ocr'
  confidence
FROM read_pdf_words(getvariable('folder') || '/*.pdf',
                    auto_ocr := true)
ORDER BY file, page, y1 DESC, x0;

-- Nest word boxes onto each page (generic redaction/highlight surface)
CREATE OR REPLACE TABLE docs AS
SELECT
  d.file,
  d.page,
  d.page_count,
  d.text,
  d.width,
  d.height,
  d.has_text_layer,
  d.used_ocr,
  list(
    struct_pack(
      word       := w.word,
      x0         := w.x0,
      y0         := w.y0,
      x1         := w.x1,
      y1         := w.y1,
      source     := w.source,
      confidence := w.confidence
    )
  ) FILTER (WHERE w.word IS NOT NULL)                   AS word_boxes,
  avg(w.confidence)                                     AS avg_confidence
FROM docs d
LEFT JOIN words w USING (file, page)
GROUP BY ALL
ORDER BY file, page;

-- Stable integer key for FTS (required by create_fts_index)
CREATE OR REPLACE TABLE pages AS
SELECT
  row_number() OVER (ORDER BY file, page)::BIGINT       AS id,
  file,
  page,
  text,
  has_text_layer,
  used_ocr,
  word_boxes,
  avg_confidence,
  width,
  height
FROM docs;

-- Digital vs OCR split (always print for the corpus under :folder)
CREATE OR REPLACE TABLE ingest_stats AS
SELECT
  count(*)                                              AS n_pages,
  count(*) FILTER (WHERE used_ocr)                      AS n_ocr_pages,
  count(*) FILTER (WHERE has_text_layer AND NOT used_ocr) AS n_digital_pages,
  count(DISTINCT file)                                  AS n_files,
  count(*) FILTER (WHERE used_ocr AND length(trim(text)) > 0) AS n_ocr_pages_with_text
FROM pages;

SELECT '=== 1. INGEST: digital vs OCR split ===' AS stage;
SELECT * FROM ingest_stats;
SELECT file, page, has_text_layer, used_ocr,
       length(text) AS n_chars,
       left(replace(text, chr(10), ' '), 80) AS preview
FROM pages
ORDER BY file, page;

-- -----------------------------------------------------------------------------
-- 2. FTS: BM25 over page text
-- -----------------------------------------------------------------------------
-- overwrite=1 makes re-runs idempotent (no drop needed)
PRAGMA create_fts_index('pages', 'id', 'text', overwrite=1);

CREATE OR REPLACE TABLE q_fts AS
SELECT
  file,
  page,
  id,
  score,
  used_ocr,
  left(replace(text, chr(10), ' '), 100) AS snippet
FROM (
  SELECT
    p.*,
    fts_main_pages.match_bm25(p.id, getvariable('q_keyword')) AS score
  FROM pages p
)
WHERE score IS NOT NULL
ORDER BY score DESC
LIMIT 10;

SELECT '=== 2. FTS (BM25) keyword/PII: ' || getvariable('q_keyword') || ' ===' AS stage;
SELECT * FROM q_fts;

-- Also prove OCR text is FTS-searchable
CREATE OR REPLACE TABLE q_fts_ocr AS
SELECT file, page, score, used_ocr,
       left(replace(text, chr(10), ' '), 100) AS snippet
FROM (
  SELECT p.*, fts_main_pages.match_bm25(p.id, getvariable('q_ocr_term')) AS score
  FROM pages p
)
WHERE score IS NOT NULL
ORDER BY score DESC
LIMIT 10;

SELECT '=== 2b. FTS over OCR text: ' || getvariable('q_ocr_term') || ' ===' AS stage;
SELECT * FROM q_fts_ocr;

-- -----------------------------------------------------------------------------
-- 3. VSS: chunk -> embeddings (stub or llm) -> HNSW
--
-- EMBEDDING STRATEGY
--   Prefer a real model (llm extension / external UDF). When unavailable, use
--   stub_hash_embed: deterministic bag-of-token hashed into FLOAT[emb_dim].
--   This is NOT semantic quality — it is a drop-in shape for HNSW so the
--   pipeline, index, and hybrid SQL stay identical when you swap models.
-- -----------------------------------------------------------------------------

-- Page-level chunks (corpus pages are short; for long docs, sub-chunk text).
-- General pattern: one retrieval unit = one row with (file, page_start, page_end, text).
CREATE OR REPLACE TABLE chunks AS
SELECT
  row_number() OVER (ORDER BY file, page)::BIGINT       AS chunk_id,
  file,
  page                                                  AS page_start,
  page                                                  AS page_end,
  text                                                  AS chunk_text,
  length(text)                                          AS n_chars,
  used_ocr
FROM pages
WHERE length(trim(coalesce(text, ''))) > 0;

-- --- STUB EMBEDDER (documented; replace with real model) -------------------
-- tokenize -> hash(token)%dim -> bag-of-counts -> L2-normalize -> FLOAT[emb_dim]
-- Swap point: UPDATE chunks SET embedding = real_model(chunk_text);
CREATE OR REPLACE TABLE chunk_embeddings AS
WITH tokens AS (
  SELECT
    c.chunk_id,
    unnest(regexp_split_to_array(lower(coalesce(c.chunk_text, '')), '[^a-z0-9]+')) AS tok
  FROM chunks c
),
bins AS (
  SELECT
    chunk_id,
    (hash(tok) % getvariable('emb_dim'))::BIGINT AS bin,
    count(*)::DOUBLE AS c
  FROM tokens
  WHERE tok <> ''
  GROUP BY 1, 2
),
maps AS (
  SELECT chunk_id, map(list(bin), list(c)) AS mp
  FROM bins
  GROUP BY chunk_id
),
raw AS (
  SELECT
    chunk_id,
    list_transform(
      range(0, getvariable('emb_dim')::BIGINT),
      lambda i : coalesce(element_at(mp, i)[1], 0.0)::DOUBLE
    ) AS raw_vec
  FROM maps
),
normed AS (
  SELECT
    chunk_id,
    raw_vec,
    greatest(
      sqrt(list_aggregate(list_transform(raw_vec, lambda x : x * x), 'sum')),
      1e-9
    ) AS l2
  FROM raw
)
SELECT
  chunk_id,
  list_transform(raw_vec, lambda x, i : (x / l2)::FLOAT)::FLOAT[32] AS embedding
FROM normed;

-- Zero-fill any chunk that produced no tokens (full emb_dim zeros)
CREATE OR REPLACE TABLE chunk_embeddings AS
WITH present AS (
  SELECT * FROM chunk_embeddings
),
missing AS (
  SELECT c.chunk_id
  FROM chunks c
  ANTI JOIN present p USING (chunk_id)
)
SELECT * FROM present
UNION ALL BY NAME
SELECT
  chunk_id,
  list_transform(range(0, getvariable('emb_dim')::BIGINT), lambda i : 0.0::FLOAT)::FLOAT[32] AS embedding
FROM missing;

CREATE OR REPLACE TABLE chunks AS
SELECT c.*, e.embedding
FROM chunks c
JOIN chunk_embeddings e USING (chunk_id);

-- HNSW index (vss)
DROP INDEX IF EXISTS chunks_hnsw;
CREATE INDEX chunks_hnsw ON chunks USING HNSW (embedding);

-- Query embedding for the semantic question (same stub + L2-norm)
CREATE OR REPLACE TABLE query_vec AS
WITH tokens AS (
  SELECT unnest(regexp_split_to_array(lower(getvariable('q_semantic')), '[^a-z0-9]+')) AS tok
),
bins AS (
  SELECT (hash(tok) % getvariable('emb_dim'))::BIGINT AS bin, count(*)::DOUBLE AS c
  FROM tokens WHERE tok <> '' GROUP BY 1
),
m AS (SELECT map(list(bin), list(c)) AS mp FROM bins),
raw AS (
  SELECT list_transform(
           range(0, getvariable('emb_dim')::BIGINT),
           lambda i : coalesce(element_at(mp, i)[1], 0.0)::DOUBLE
         ) AS raw_vec
  FROM m
),
n AS (
  SELECT raw_vec,
         greatest(sqrt(list_aggregate(list_transform(raw_vec, lambda x : x * x), 'sum')), 1e-9) AS l2
  FROM raw
)
SELECT list_transform(raw_vec, lambda x, i : (x / l2)::FLOAT)::FLOAT[32] AS qemb
FROM n;

CREATE OR REPLACE TABLE q_vss AS
SELECT
  c.chunk_id,
  c.file,
  c.page_start AS page,
  c.used_ocr,
  array_distance(c.embedding, (SELECT qemb FROM query_vec)) AS distance,
  left(replace(c.chunk_text, chr(10), ' '), 100) AS snippet
FROM chunks c
ORDER BY distance ASC
LIMIT 10;

SELECT '=== 3. VSS semantic NN: ' || getvariable('q_semantic') || ' ===' AS stage;
SELECT * FROM q_vss;

-- -----------------------------------------------------------------------------
-- 4. HYBRID: Reciprocal Rank Fusion (BM25 rank + vector rank)  << HEADLINE
--
--   RRF(d) = Σ  1 / (k + rank_i(d))
--   Here ranks are over (file, page). FTS contributes BM25 order; VSS
--   contributes nearest-neighbor order (mapped chunk -> page).
-- -----------------------------------------------------------------------------
CREATE OR REPLACE TABLE hybrid_q_vec AS
WITH tokens AS (
  SELECT unnest(regexp_split_to_array(lower(getvariable('q_hybrid')), '[^a-z0-9]+')) AS tok
),
bins AS (
  SELECT (hash(tok) % getvariable('emb_dim'))::BIGINT AS bin, count(*)::DOUBLE AS c
  FROM tokens WHERE tok <> '' GROUP BY 1
),
m AS (SELECT map(list(bin), list(c)) AS mp FROM bins),
raw AS (
  SELECT list_transform(
           range(0, getvariable('emb_dim')::BIGINT),
           lambda i : coalesce(element_at(mp, i)[1], 0.0)::DOUBLE
         ) AS raw_vec
  FROM m
),
n AS (
  SELECT raw_vec,
         greatest(sqrt(list_aggregate(list_transform(raw_vec, lambda x : x * x), 'sum')), 1e-9) AS l2
  FROM raw
)
SELECT list_transform(raw_vec, lambda x, i : (x / l2)::FLOAT)::FLOAT[32] AS qemb
FROM n;

CREATE OR REPLACE TABLE q_hybrid AS
WITH
fts_ranked AS (
  SELECT
    file,
    page,
    score AS bm25,
    row_number() OVER (ORDER BY score DESC NULLS LAST) AS fts_rank
  FROM (
    SELECT file, page, fts_main_pages.match_bm25(id, getvariable('q_hybrid')) AS score
    FROM pages
  )
  WHERE score IS NOT NULL
),
vss_ranked AS (
  SELECT
    file,
    page_start AS page,
    min(array_distance(embedding, (SELECT qemb FROM hybrid_q_vec))) AS distance,
    row_number() OVER (
      ORDER BY min(array_distance(embedding, (SELECT qemb FROM hybrid_q_vec))) ASC
    ) AS vss_rank
  FROM chunks
  GROUP BY file, page_start
),
fused AS (
  SELECT
    coalesce(f.file, v.file) AS file,
    coalesce(f.page, v.page) AS page,
    f.bm25,
    v.distance,
    f.fts_rank,
    v.vss_rank,
    coalesce(1.0 / (getvariable('rrf_k') + f.fts_rank), 0.0)
      + coalesce(1.0 / (getvariable('rrf_k') + v.vss_rank), 0.0) AS rrf_score
  FROM fts_ranked f
  FULL OUTER JOIN vss_ranked v USING (file, page)
)
SELECT
  h.*,
  p.used_ocr,
  left(replace(p.text, chr(10), ' '), 100) AS snippet
FROM fused h
JOIN pages p USING (file, page)
ORDER BY rrf_score DESC
LIMIT 10;

SELECT '=== 4. HYBRID RRF: ' || getvariable('q_hybrid') || ' ===' AS stage;
SELECT file, page, rrf_score, fts_rank, vss_rank, bm25, distance, used_ocr, snippet
FROM q_hybrid;

-- -----------------------------------------------------------------------------
-- 5. REDACTION-COORD JOIN: term -> pixel rectangles
--    Generic "locate term on page -> x/y box" for redact / highlight / UI.
-- -----------------------------------------------------------------------------
CREATE OR REPLACE TABLE q_coords AS
SELECT
  w.file,
  w.page,
  w.word,
  w.x0, w.y0, w.x1, w.y1,
  (w.x1 - w.x0) AS width_px,
  (w.y1 - w.y0) AS height_px,
  w.source,
  w.confidence,
  p.used_ocr,
  -- ready for pdf_redact rectangles: STRUCT(page, x, y, w, h)
  struct_pack(
    page := w.page,
    x    := w.x0,
    y    := w.y0,
    w    := (w.x1 - w.x0),
    h    := (w.y1 - w.y0)
  ) AS redact_rect
FROM words w
JOIN pages p USING (file, page)
WHERE lower(w.word) = lower(getvariable('q_ocr_term'))
   OR w.word ILIKE '%' || getvariable('q_keyword') || '%'
   OR lower(w.word) LIKE '%secret%'
ORDER BY w.file, w.page, w.y1 DESC, w.x0;

SELECT '=== 5. REDACTION-COORD JOIN (term -> boxes) ===' AS stage;
SELECT * FROM q_coords;

-- Bonus: pack all rects for a single file (call pdf_redact with this list)
CREATE OR REPLACE TABLE redact_payload AS
SELECT
  file,
  list(redact_rect) AS rects
FROM q_coords
GROUP BY file;

SELECT '=== 5b. redact_payload (per-file rect lists) ===' AS stage;
SELECT * FROM redact_payload;

-- -----------------------------------------------------------------------------
-- 6. EXPORT ARTIFACTS (parquet snapshots for reproducibility)
-- -----------------------------------------------------------------------------
COPY pages            TO (getvariable('out_dir') || '/pages.parquet')            (FORMAT PARQUET);
COPY words            TO (getvariable('out_dir') || '/words.parquet')            (FORMAT PARQUET);
COPY chunks           TO (getvariable('out_dir') || '/chunks.parquet')           (FORMAT PARQUET);
COPY ingest_stats     TO (getvariable('out_dir') || '/ingest_stats.parquet')     (FORMAT PARQUET);
COPY q_fts            TO (getvariable('out_dir') || '/q_fts.parquet')            (FORMAT PARQUET);
COPY q_fts_ocr        TO (getvariable('out_dir') || '/q_fts_ocr.parquet')        (FORMAT PARQUET);
COPY q_vss            TO (getvariable('out_dir') || '/q_vss.parquet')            (FORMAT PARQUET);
COPY q_hybrid         TO (getvariable('out_dir') || '/q_hybrid.parquet')         (FORMAT PARQUET);
COPY q_coords         TO (getvariable('out_dir') || '/q_coords.parquet')         (FORMAT PARQUET);

SELECT '=== pipeline complete ===' AS stage;
SELECT 'folder=' || getvariable('folder') AS param,
       'emb=stub_hash_embed dim=' || getvariable('emb_dim') AS embedding_mode,
       'rrf_k=' || getvariable('rrf_k') AS rrf;
