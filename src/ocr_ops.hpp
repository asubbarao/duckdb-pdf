//===--------------------------------------------------------------------===//
// ocr_ops.hpp — plain-C++ interface to Tesseract + Leptonica OCR, plus a
// pluggable "external" backend seam for SOTA engines (vision-LLM / Paddle /
// Surya) that must NOT require recompiling this extension.
//
// TRANSLATION-UNIT ISOLATION (mirrors qpdf_ops.hpp): tesseract and leptonica
// headers are heavy and historically cause platform-specific include / ODR
// friction on the community-extensions CI. ocr_ops.cpp is the ONLY translation
// unit that includes <tesseract/*> or <leptonica/*>. This header is the
// boundary: plain std types in, plain std types out — no tesseract types, no
// leptonica types, no duckdb types, no poppler types.
//
// The caller (pdf_extension.cpp) owns poppler rendering under PopplerMutex
// and feeds the resulting ARGB32/RGB24 bitmap here. OCR never shells out to
// the tesseract CLI; everything is in-process via TessBaseAPI for the default
// backend.
//
// Backends
// --------
//   "tesseract" (default, linked): in-process TessBaseAPI + leptonica
//                                  preprocess. Built into this TU.
//   "external": NO shell-out. Resolves in this order:
//     1. If Options::external_plugin is non-empty, dlopen/LoadLibrary that
//        shared library and call the C ABI below (pdf_ocr_plugin_recognize).
//        This is how a Paddle/Surya/custom vision model plugs in without
//        recompiling read_pdf.
//     2. Otherwise return an empty result (best_effort) or throw with a
//        message pointing at pdf_page_images / pdf_to_png + the DuckDB llm/ai
//        extension as the SQL pipeline for vision-LLM OCR.
//
// External plugin C ABI (stable, C linkage):
//   extern "C" int pdf_ocr_plugin_recognize(
//       const unsigned char *data, int width, int height, int bytes_per_row,
//       int format,               // 0 = ARGB32, 1 = RGB24
//       const char *language,
//       char **out_text,          // malloc'd UTF-8; free with pdf_ocr_plugin_free
//       int *out_confidence);     // 0..100, or -1 if unknown
//     // return 0 on success, non-zero on failure
//   extern "C" void pdf_ocr_plugin_free(void *p);
//
// Error contract: hard failures (missing language model / missing plugin when
// best_effort is false) throw std::runtime_error with an actionable message;
// the caller wraps e.what() into duckdb::IOException. Soft/best-effort paths
// return empty results without throwing.
//===--------------------------------------------------------------------===//
#pragma once

#include <string>
#include <vector>

namespace pdf_ocr {

// Pixel layout of the buffer handed to Recognize*.
enum class ImageFormat : int {
	ARGB32 = 0, // poppler default: little-endian B,G,R,A in memory
	RGB24 = 1
};

// Which engine should Recognize* dispatch to.
enum class Backend : int {
	Tesseract = 0,
	External = 1
};

// Parse a user-facing backend name. Unknown values throw std::runtime_error.
Backend BackendFromString(const std::string &name);
const char *BackendName(Backend b);

struct Options {
	std::string language = "eng";
	int dpi = 300;            // used for leptonica resolution + word PDF-point conversion
	int psm = 3;              // PSM_AUTO
	int oem = 3;              // OEM_DEFAULT
	std::string tessdata_dir; // explicit model dir, or empty for auto-detect
	bool preprocess = true;   // leptonica grayscale/deskew/binarize/despeckle
	bool best_effort = false; // missing model / plugin -> empty result, don't throw
	Backend backend = Backend::Tesseract;
	// Absolute/relative path to a shared library implementing the C ABI above.
	// Only consulted when backend == External.
	std::string external_plugin;
	// Reserved for a future in-process HTTP client. Currently never contacted
	// from this TU (no shell, no network). Callers that set it get a clearer
	// error message pointing at the SQL pdf_page_images → llm pipeline instead
	// of a silent empty OCR result when no plugin is loaded.
	std::string external_endpoint;
};

struct TextResult {
	std::string text;
	int confidence = 0; // MeanTextConf 0..100
	// Which backend actually produced the text (or was attempted).
	Backend backend_used = Backend::Tesseract;
};

// Word boxes in PDF user-space points (origin bottom-left), converted from
// tesseract pixel coordinates using dpi and image height.
struct Word {
	std::string text;
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	float confidence = 0;
};

struct WordsResult {
	std::vector<Word> words;
	int confidence = 0; // MeanTextConf 0..100 of the whole page
	Backend backend_used = Backend::Tesseract;
};

// Single-pass OCR of a rendered page bitmap. `bytes_per_row` is the stride.
// Confidence-based 2x-DPI retry lives in the caller (only the caller can
// re-render via poppler). Dispatches on opt.backend.
TextResult RecognizeText(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                         const Options &opt);

WordsResult RecognizeWords(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                           const Options &opt);

} // namespace pdf_ocr
