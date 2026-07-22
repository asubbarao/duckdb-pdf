//===----------------------------------------------------------------------===//
// Bundled PDF base-14 substitute fonts (URW++ Nimbus / StandardSymbols / Dingbats)
//
// Community-extension binaries ship as a single .duckdb_extension file with no
// companion font data. The OTFs live as embedded byte arrays (generated from
// third_party/fonts/ by scripts/embed_base14_fonts.py) and are extracted to a
// temp directory once, then registered with poppler GlobalParams::addFontFile
// so page rasterization (pdf_redact, OCR, pdf_to_png, …) works without
// fontconfig or system display fonts.
//===----------------------------------------------------------------------===//
#pragma once

namespace duckdb {

// Idempotent. Safe to call under PopplerMutex. No-ops until poppler has created
// globalParams (first document open). Retries until registration succeeds.
void EnsurePdfBase14Fonts();

} // namespace duckdb
