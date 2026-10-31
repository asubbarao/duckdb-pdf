//===--------------------------------------------------------------------===//
// vision_ocr.hpp — Apple Vision (VNRecognizeTextRequest) OCR backend, and
// Vision barcode detection (VNDetectBarcodesRequest) for image_barcodes.
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
#include <string>
#include <vector>

namespace pdf_ocr {

// True when this build has the Vision backend and the OS supports it.
bool VisionAvailable();

TextResult RecognizeVisionText(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                               const Options &opt);

WordsResult RecognizeVisionWords(const unsigned char *data, int width, int height, int bytes_per_row,
                                 ImageFormat format, const Options &opt);

// Encoded image BLOB (PNG/JPEG/TIFF/HEIC/... — anything ImageIO decodes).
TextResult RecognizeVisionImageBlob(const unsigned char *data, size_t size, const Options &opt);

//===--------------------------------------------------------------------===//
// Barcodes (VNDetectBarcodesRequest, the newest revision the OS offers).
//
// Symbology names drop Vision's "VNBarcodeSymbology" prefix: EAN13, EAN8,
// UPCE, QR, Code128, DataMatrix, ITF14, GS1DataBar, ... Vision reports UPC-A
// as EAN13 with a leading zero.
//
// Boxes are pixels of the upright image (EXIF orientation applied), origin
// top-left. Revision 4 may report boxes that extend past the image edge.
// Confidence is Vision's observation confidence scaled to 0..100.
//===--------------------------------------------------------------------===//
struct Barcode {
	std::string symbology;
	bool has_payload = false; // payloadStringValue
	std::string payload;
	// payloadData (macOS 14+): the symbol's data as Vision returns it, not the
	// decoded text. For QR it is the data codewords (mode and length bits,
	// padding); for EAN/UPC one byte per digit.
	bool has_payload_bytes = false;
	std::string payload_bytes;
	bool has_supplemental = false; // supplementalPayloadString (macOS 14+), e.g. EAN-5 add-ons
	std::string supplemental;
	bool gs1 = false;            // isGS1DataCarrier (macOS 14+)
	bool color_inverted = false; // isColorInverted (macOS 14+)
	double confidence = 0;       // 0..100
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Detects barcodes in an encoded image BLOB. `symbologies` restricts the
// search (names as above, case-insensitive); empty searches every symbology.
// Throws std::runtime_error when the BLOB does not decode, a symbology is
// unknown, or Vision fails.
std::vector<Barcode> DetectVisionBarcodes(const unsigned char *data, size_t size,
                                          const std::vector<std::string> &symbologies);

} // namespace pdf_ocr
