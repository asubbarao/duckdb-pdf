-- =============================================================================
-- RAG over a folder of PDFs in three stages — chunk grain + core vss
-- =============================================================================
-- The pdf extension is a deterministic reader/writer: it does NOT embed and
-- does NOT call models. This recipe shows the composition: the chunk grain makes
-- retrieval-ready, section-aware chunks; YOU bring the embedder (any local
-- model, API, or DuckDB UDF that yields a FLOAT[N]); DuckDB's core vss
-- extension does the nearest-neighbor search.
--
-- TEMPLATE: supply your embedder and a 384-element query vector before running.
-- Run with a matching pdf build: ./build/release/duckdb -f examples/rag_pdf_vss.sql
-- Adjust the glob and embedding dimension N=384 to your setup.

-- The source-build CLI links pdf; signed community builds may have an older API.
LOAD pdf;

-- -----------------------------------------------------------------------------
-- Statement 1: chunk every PDF in the folder.
--   chunk_size / overlap are byte budgets; defaults are 1200 / 150.
--   n_chars counts UTF-8 codepoints. Set auto_ocr := true for scanned pages.
--   Every chunk carries file, page_start/page_end, and the text of the
--   nearest preceding heading — citation context for free.
-- -----------------------------------------------------------------------------
CREATE TABLE chunks AS
FROM read_pdf('docs/*.pdf', grain := 'chunk', chunk_size := 1200, overlap := 150);

-- Sanity peek: sections found and chunk sizes
SELECT file, heading, COUNT(*) AS chunks, MAX(n_chars) AS max_chars
FROM chunks
GROUP BY ALL
ORDER BY file, heading;

-- -----------------------------------------------------------------------------
-- Statement 2: add the embedding column and fill it with YOUR embedder.
--   The shape is a fixed-size FLOAT array; 384 matches e.g.
--   sentence-transformers/all-MiniLM-L6-v2 — use your model's dimension.
--
--   PLACEHOLDER — the pdf extension does not embed. Replace my_embedder()
--   with whatever produces the vectors, for example:
--     * a DuckDB scalar UDF registered from Python/Node
--       (con.create_function('my_embedder', ...) over the chunks table), or
--     * export `SELECT rowid, text FROM chunks` to parquet, embed offline,
--       re-import and UPDATE by rowid.
-- -----------------------------------------------------------------------------
ALTER TABLE chunks ADD COLUMN embedding FLOAT[384];

-- UPDATE chunks SET embedding = my_embedder(text);   -- << YOUR EMBEDDER HERE

-- -----------------------------------------------------------------------------
-- Statement 3: index with core vss and search.
-- -----------------------------------------------------------------------------
INSTALL vss;
LOAD vss;
SET hnsw_enable_experimental_persistence = true; -- needed for on-disk DBs

CREATE INDEX chunks_hnsw ON chunks USING HNSW (embedding);

-- Nearest-neighbor query: embed the question with the SAME embedder, then:
-- (the [ ... ] literal below stands in for my_embedder('your question'))
SELECT file,
       heading,
       page_start,
       page_end,
       text
FROM chunks
ORDER BY array_distance(embedding, [/* query embedding floats */]::FLOAT[384])
LIMIT 5;

-- Every hit is citation-ready: file + page span + the section heading it
-- came from, no second lookup required.
