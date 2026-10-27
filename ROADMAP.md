# Roadmap

This file describes the current source branch, not a published release.
See [README.md](README.md) for the callable API and migration contracts.

## Implemented: 48 to 19 public names

The previous four-name cleanup (52 to 48) is the base of this change.
The broader consolidation now exposes 19 names without retired aliases:

| Family | Selection |
|---|---|
| `read_pdf` | grain: page, line, element, table, chunk |
| `read_pdf_words` | positioned words |
| `pdf_info` | detail: basic or qpdf |
| `pdf_objects` | kind: outline, structure, destinations, fonts, form_fields, annotations, attachments, images, signatures, revisions, pages |
| `pdf_text` | scalar options format: text, markdown, html, xml, json |
| `pdf_render` | scalar and table forms; png/svg, in memory or page-image export |
| `pdf_merge` | concatenate documents |
| `pdf_pages` | extract/reorder, rotate a complete document, or rotate extracted pages |
| `pdf_split` | mode: page or blank |
| `pdf_stamp` | scalar options mode: watermark or bates |
| `pdf_rewrite` | scalar options mode: compress, repair, encrypt, decrypt |
| `pdf_redact` | constant-input raster redaction |
| `pdf_redact_lateral` | per-row raster redaction |
| `pdf_sign` | detached CMS signing |
| `write_pdf` | native text-to-PDF |
| `to_pdf` | LibreOffice conversion |
| `ocr_image` | scalar per-row OCR and table output with confidence |
| `pdf_ocr` | persist searchable OCR layers |
| `poppler_version` | runtime version |

This corrects omissions in the earlier 19-family sketch: tagged structure and
persisted OCR remain available. It does not add the proposed sniff_pdf or
pdf_help helpers. Existing catalog descriptions, parameter names and examples
provide discoverable help without another public registration.

Table selectors choose schemas at bind time and dispatch directly to the native
reader callbacks, retaining projection, file scheduling and page bounds.
Unsupported options are rejected for the selected mode. Scalar STRUCT options
are checked for valid names and types; transform side effects remain volatile
and outputs are staged atomically.

Scalar pdf_render returns encoded BLOB bytes for both formats; SVG callers use
decode for VARCHAR. The table form streams PNG BLOBs or SVG strings and can
export either format to disk. Scalar ocr_image preserves row-wise image OCR;
its table counterpart retains confidence reporting. Whole-document rotation is
separate from page extraction within pdf_pages, so rotating does not discard
the document catalog, metadata or bookmarks.

The earlier column-normalization sketch is not bundled into these removals:
existing per-grain schemas remain explicit in the README, including file versus
filename, page versus page_number, and object-specific bbox column spellings.
The existing read_pdf page/text and first_page/last_page contract stays intact.
The scalar path/BLOB text contract moves to pdf_text.

## Planned

- **Content-stream redaction**: remove selected text/vector operators while
  retaining unaffected vector content. Current redaction replaces affected
  pages with rasters and removes their underlying text.
- **PDF/A validation**: validate conformance beyond reading pdfaid metadata.
- **Table PDF authoring**: typeset a query result as a multi-column PDF table.
- **Standalone OCR extension**: factor generic image OCR after the surface
  stabilizes.
- **Shared document open for mixed output**: avoid reopening a document when a
  caller needs page images and text grains together. Text grains already share
  the word-walk implementation where appropriate.

## Non-goals

- ML document understanding, semantic layout models and entity extraction.
- Reliable merged-cell or complex borderless-table reconstruction.
- Deprecation aliases that keep retired public function names alive.
- Claiming source changes are available in community binaries before release.
