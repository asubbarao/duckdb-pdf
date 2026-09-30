//===--------------------------------------------------------------------===//
// C++14-only adapter for Tesseract's PDF renderer.
//
// renderer.h uses deduced return types, while the rest of the extension keeps
// DuckDB's C++11 baseline.
//===--------------------------------------------------------------------===//
#include "ocr_renderer.hpp"

#include <tesseract/baseapi.h>
#include <tesseract/renderer.h>

namespace pdf_ocr {

bool RenderTesseractPdf(tesseract::TessBaseAPI &api, const std::string &output_base, const std::string &datadir) {
	tesseract::TessPDFRenderer renderer(output_base.c_str(), datadir.c_str(), true /* textonly */);
	if (!renderer.BeginDocument("DuckDB PDF OCR")) {
		return false;
	}
	if (api.Recognize(0) != 0) {
		return false;
	}
	if (!renderer.AddImage(&api)) {
		return false;
	}
	return renderer.EndDocument() && renderer.happy();
}

} // namespace pdf_ocr
