# Testing this extension

SQLLogic tests under `sql/` are the public-behavior pin. DuckDB prefers this
format (statements + expected result hashes). Root makefile targets:

```bash
make test
make test_debug
```

## Public API pins (start here)

| Test | What it pins |
|---|---|
| `sql/pdf_full_api.test` | `read_pdf_meta`, `pdf_pages_info`, `pdf_permissions`, `pdf_fonts`, `pdf_destinations`, `pdf_page_images`, `pdf_qpdf_info`, `pdf_json`, `pdf_repair`, `ocr_backend` |
| `sql/qpdf_suite.test` | `pdf_merge` / `pdf_compress` / `pdf_encrypt` / `pdf_decrypt` / `pdf_pages`, `pdf_form_fields` / `pdf_annotations` (incl. glob), in-place refusal |
| `sql/pdf_lowlevel_api.test` | `poppler_version`, `poppler_render_page`, `tesseract_ocr` overloads, `ocr_image` formats |
| `sql/pdf_write_page_images.test` | path/glob + `LIST(VARCHAR)` overloads, dpi range, `p{N}.png` names |
| `sql/read_pdf_*.test` | reader grains, OCR, ignore_errors, parallel scan |

`ocr_image` is a table function: its BLOB argument must be **foldable**. The
low-level test uses `SET VARIABLE` / `getvariable` because SQLLogic needs a
constant at bind. That is harness-only — product SQL should use `tesseract_ocr`
on a column (`pdf_page_images` / `read_blob`) or pass a BLOB literal.

Readers/inspectors that take a VARCHAR generally glob (`ResolveFiles`).
Scalars and writers (`pdf_merge`, `pdf_compress`, `pdf_json`, `pdf_repair`,
`pdf_split`, …) take one path or an explicit `LIST` — they do not glob.
`pdf_form_fields` / `pdf_annotations` glob but register **no** `password`
named parameter.
