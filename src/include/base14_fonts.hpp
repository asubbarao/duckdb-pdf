//===----------------------------------------------------------------------===//
// Bundled PDF base-14 substitute fonts + poppler private-API helpers
//
// Community-extension binaries ship as a single .duckdb_extension file with no
// companion font data. The OTFs live as embedded byte arrays (generated from
// third_party/fonts/ by scripts/embed_base14_fonts.py) and are extracted to a
// temp directory once, then registered with poppler GlobalParams::addFontFile
// so page rasterization (pdf_redact, OCR, pdf_to_png, …) works without
// fontconfig or system display fonts.
//
// Private poppler headers (GlobalParams.h, Error.h) require C++20 on current
// poppler (std::span, string::starts_with) and must NOT be included from
// pdf_extension.cpp (DuckDB Linux CI drives -std=c++11 for the main TU — same
// ODR story as qpdf_ops.cpp). Implementations live in base14_font_data.cpp
// (generated) and poppler_private_ops.cpp, both forced to C++20 in CMakeLists.

//===----------------------------------------------------------------------===//
#pragma once

namespace duckdb {

// Idempotent. Safe to call under PopplerMutex. No-ops until poppler has created
// globalParams (first document open). Retries until registration succeeds.
void EnsurePdfBase14Fonts();

// Install setErrorCallback once (poppler private API). Safe under PopplerMutex.
void EnsurePopplerErrorCallback();

// Reset / read the missing-display-font flag set by the error callback.
void ResetPopplerMissingDisplayFont();
bool PopplerMissingDisplayFont();

} // namespace duckdb
