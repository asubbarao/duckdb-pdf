//===--------------------------------------------------------------------===//
// vision_ocr.hpp — Apple Vision (VNRecognizeTextRequest) OCR backend.
//
// Same isolation rule as ocr_ops.hpp: plain std types only. vision_ocr.mm is
// the only translation unit that includes Vision / CoreGraphics headers; on
// other platforms vision_ocr_stub.cpp provides the same symbols and reports
// that the backend is unavailable.
//
// ocr_ops.cpp dispatches here when Options::backend == Backend::Vision.
// Vision chooses its own compute device (CPU / GPU / Neural Engine): setting
// one has no measurable effect, so no such option is exposed.
//
// Languages: Tesseract codes (eng, fra, deu, chi_sim, ...) are mapped to
// Vision's BCP-47 identifiers; BCP-47 identifiers (en-US, fr-FR) pass
// through. '+' joins several languages, as in Tesseract, and 'auto' turns on
// Vision's language detection. Unknown or unsupported languages throw with
// the list Vision supports.
//
// Options::vars carries Vision's request options: recognition_level
// (accurate|fast), language_correction (true|false), custom_words
// (comma-separated) and minimum_text_height (0..1). Other keys throw.
//
// Confidence is the mean of Vision's per-line confidence, scaled to 0..100.
// Word boxes come from VNRecognizedText boundingBoxForRange per whitespace
// token, converted to PDF points (origin bottom-left, like Vision's own
// normalized coordinates).
//===--------------------------------------------------------------------===//
#pragma once

#include "ocr_ops.hpp"

#include <cstddef>

namespace pdf_ocr {

// True when this build has the Vision backend and the OS supports it.
bool VisionAvailable();

TextResult RecognizeVisionText(const unsigned char *data, int width, int height, int bytes_per_row,
                               ImageFormat format, const Options &opt);

WordsResult RecognizeVisionWords(const unsigned char *data, int width, int height, int bytes_per_row,
                                 ImageFormat format, const Options &opt);

// Encoded image BLOB (PNG/JPEG/TIFF/HEIC/... — anything ImageIO decodes).
TextResult RecognizeVisionImageBlob(const unsigned char *data, size_t size, const Options &opt);

} // namespace pdf_ocr
