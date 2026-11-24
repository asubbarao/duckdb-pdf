# fix: make automatic OCR reject stamp-only text and honor OCR opt-out

## Body

`read_pdf` treated any readable glyph as useful native text, so a short layer of en-dashes could hide the scanned page beneath it. The unreadable-layer check handled garbled glyphs but did not reject punctuation-only content. Automatic OCR now counts Unicode letter/number words and treats fewer than 10 as an unreadable layer.

`ocr := false` previously cleared only the forced-OCR flag. Since `auto_ocr` defaults to true, scan-only pages were still sent to OCR. An explicit opt-out now disables automatic OCR too, unless `auto_ocr := true` is explicitly supplied.

Automatic OCR already used the common OCR options conversion path, which passes `ocr_psm`, DPI, OEM, language, preprocessing, retry and tessdata settings through. The regression test supplies `ocr_psm := 1`.

## False-first evidence

Both new cases were run against the requested base, `d359f96`:

- Dash-only text layer over a scanned page: expected `true, true`; actual `false, false` for `(used_ocr, contains(scanned_totals))`.
- Scan-only page with `ocr := false`: expected `false, true`; actual `true, false` for `(used_ocr, text_is_empty)`.

With the fix, `read_pdf_ocr_auto_options.test` passes all 6 assertions using the freshly built linked extension. The full repository test suite passes: 70 passed, 4 skipped (`LIBREOFFICE_PATH` and `PDF_NETWORK_TESTS` were not enabled).

## ICW before and after

Before, on the base reproduction, pages 16, 19, 32, 43, 53 and 62 kept their dash-only text layer with `used_ocr=false`; the other 56 pages used OCR. Passing `ocr := false` still allowed automatic OCR on scan-only pages.

After, the requested `auto_ocr := true, ocr_psm := 1` query returned 62 rows and `used_ocr=true` for every page. The six formerly missed pages now have text lengths 369, 325, 310, 332, 342 and 287 respectively.

After, `ocr := false` returned `used_ocr=false` for all 62 pages. The six native dash layers remained (lengths 7, 7, 3, 7, 11 and 7); the other pages returned empty text.

## Scope

Only `src/pdf_extension.cpp` and OCR SQLLogicTest inputs/fixture were changed. Poppler mutex and page rendering code were not changed. No installed extensions or dev services were touched. This change is committed locally only; it is not pushed and no PR was opened.
