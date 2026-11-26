# pdf_ocr: OCR dash-only and short-caption layers over scans

## Body

`read_pdf` sees through a punctuation-only text layer since #50: automatic OCR
counts Unicode letter/number words and treats fewer than 10 as an unreadable
layer. `pdf_ocr` never got that gate — its skip decision checks only the
unreadable-glyph tally (control characters / ≥10% unreadable) and the stamp
probe. A scanned page whose native layer is only en-dashes, or a short
readable caption, reads as usable text, so `pdf_ocr` skips the page and the
searchable output keeps only the dashes (or only the caption) while the scan
underneath stays unsearchable.

The fix applies the same `AlphanumericWordCount < 10` gate to `pdf_ocr`'s
`native_not_usable` decision, alongside the two existing checks. The OCR-wins
guard below it is unchanged: OCR output still replaces the native layer only
when it reads more readable words, so a photo with a caption keeps the caption
when OCR reads nothing.

## False-first evidence

On the main binary (`f1eeb39`), the committed #50 fixture and a short-caption
fixture both fail:

- `pdf_ocr('test/data/dash_text_layer_over_scan.pdf', ...)`: `ocr_applied=false`,
  output text contains no `Totals 16,710.55`. `read_pdf` on the same file gives
  `used_ocr=true` and recovers the totals.
- `pdf_ocr` on a scan with a 24pt readable `Figure 1: Revenue` caption:
  `ocr_applied=false`, output text is the 17-character caption only.
- `pdf_ocr(..., force := true)` on the dash fixture recovers the totals, so the
  OCR path itself works — only the auto decision is wrong.

With the fix, the new regression test in `test/sql/pdf_ocr.test` (receipt
`1 true true true` plus the output containing `Totals 16,710.55`) is expected
to pass; it fails on the pre-fix binary as shown above.

## Scope

Only `src/pdf_extension.cpp` (one condition) and `test/sql/pdf_ocr.test` (one
case reusing the #50 fixture) change. No other decision site changes: every
other auto-OCR call site already goes through `AutoOcrWantsPage` /
`AutoOcrWantsPageText`, which #50 updated. This change is committed locally
only; it is not pushed and no PR was opened.
