-- Digest a long document: its contents, section by section (COOKBOOK.md recipe 10).
-- Change the path (or URL); the document needs bookmarks. No bookmarks -> no rows: anchor on the
-- headings of read_pdf_elements instead.
-- Result: one row per bookmark in document order:
--   contents VARCHAR (indented by depth), page INTEGER (the bookmark's page),
--   lines BIGINT, chars BIGINT (size of the section), opening VARCHAR (its first line longer than 40
--   characters, so page numbers and headers are skipped; NULL for a section with no such line).
-- Needs the pdf version that has pdf_outline.page.
INSTALL pdf FROM community; LOAD pdf;
INSTALL httpfs; LOAD httpfs;

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
  FROM pdf_outline('https://blobs.duckdb.org/docs/ducklake-docs.pdf') o
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
