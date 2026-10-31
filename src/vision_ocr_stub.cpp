//===--------------------------------------------------------------------===//
// vision_ocr_stub.cpp — non-Apple builds: the Vision backend is unavailable.
// See vision_ocr.hpp.
//===--------------------------------------------------------------------===//
#include "vision_ocr.hpp"

#include <stdexcept>

namespace pdf_ocr {
namespace {

const char *const kUnavailable ="ocr backend 'vision': Apple Vision is only available on macOS";

} // namespace

bool VisionAvailable() {
	return false;
}

TextResult RecognizeVisionText(const unsigned char *, int, int, int, ImageFormat, const Options &opt) {
	TextResult out;
	out.backend_used = Backend::Vision;
	if (!opt.best_effort) {
		throw std::runtime_error(kUnavailable);
	}
	return out;
}

WordsResult RecognizeVisionWords(const unsigned char *, int, int, int, ImageFormat, const Options &opt) {
	WordsResult out;
	out.backend_used = Backend::Vision;
	if (!opt.best_effort) {
		throw std::runtime_error(kUnavailable);
	}
	return out;
}

TextResult RecognizeVisionImageBlob(const unsigned char *, size_t, const Options &opt) {
	TextResult out;
	out.backend_used = Backend::Vision;
	if (!opt.best_effort) {
		throw std::runtime_error(kUnavailable);
	}
	return out;
}

} // namespace pdf_ocr
