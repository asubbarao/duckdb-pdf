//===--------------------------------------------------------------------===//
// C++14-only adapter for Tesseract's PDF renderer.
//===--------------------------------------------------------------------===//
#pragma once

#include <string>

namespace tesseract {
class TessBaseAPI;
}

namespace pdf_ocr {

bool RenderTesseractPdf(tesseract::TessBaseAPI &api, const std::string &output_base, const std::string &datadir);

} // namespace pdf_ocr
