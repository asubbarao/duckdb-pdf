//===--------------------------------------------------------------------===//
// ocr_ops.hpp — plain-C++ interface to Tesseract + Leptonica OCR.
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
// the tesseract CLI; everything is in-process via TessBaseAPI.
//
// Error contract: hard failures (missing language model when best_effort is
// false) throw std::runtime_error with an actionable message; the caller
// wraps e.what() into duckdb::IOException. Soft/best-effort paths return
// empty results without throwing.
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

struct Options {
	std::string language = "eng";
	int dpi = 300;            // used for leptonica resolution + word PDF-point conversion
	int psm = 3;              // PSM_AUTO
	int oem = 3;              // OEM_DEFAULT
	std::string tessdata_dir; // explicit model dir, or empty for auto-detect
	bool preprocess = true;   // leptonica grayscale/deskew/binarize/despeckle
	bool best_effort = false; // missing model -> empty result, don't throw
};

struct TextResult {
	std::string text;
	int confidence = 0; // MeanTextConf 0..100
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
};

// Single-pass OCR of a rendered page bitmap. `bytes_per_row` is the stride.
// Confidence-based 2x-DPI retry lives in the caller (only the caller can
// re-render via poppler).
TextResult RecognizeText(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                         const Options &opt);

WordsResult RecognizeWords(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                           const Options &opt);

} // namespace pdf_ocr
