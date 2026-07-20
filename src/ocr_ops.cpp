//===--------------------------------------------------------------------===//
// ocr_ops.cpp — Tesseract + Leptonica implementation (no duckdb, no poppler).
//
// See ocr_ops.hpp for the isolation rationale. Keep this file free of any
// duckdb/ or poppler- includes so it can compile at whatever C++ standard
// the host needs without ODR-colliding with libduckdb_static.
//===--------------------------------------------------------------------===//
#include "ocr_ops.hpp"

#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

#include <leptonica/allheaders.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace pdf_ocr {
namespace {

bool ModelExistsIn(const std::string &dir, const std::string &language) {
	if (dir.empty()) {
		return false;
	}
	std::string path = dir;
	char sep = path.back();
	if (sep != '/' && sep != '\\') {
		path += '/';
	}
	path += language + ".traineddata";
	std::ifstream f(path.c_str());
	return f.good();
}

// Resolve the tessdata directory that contains <language>.traineddata.
// Order: explicit tessdata_dir → TESSDATA_PREFIX → standard package-manager
// install locations. Returns empty if nothing was found.
std::string ResolveTessdataDir(const std::string &language, const std::string &explicit_dir) {
	if (!explicit_dir.empty()) {
		return explicit_dir;
	}
	const char *env = std::getenv("TESSDATA_PREFIX");
	if (env && *env && ModelExistsIn(env, language)) {
		return std::string(env);
	}
	static const char *kCandidates[] = {
	    "/opt/homebrew/share/tessdata",
	    "/usr/local/share/tessdata",
	    "/usr/share/tessdata",
	    "/usr/share/tesseract-ocr/5/tessdata",
	    "/usr/share/tesseract-ocr/4.00/tessdata",
	    "/usr/share/tesseract-ocr/tessdata",
	    "/usr/local/share/tesseract-ocr/tessdata",
	    "C:\\Program Files\\Tesseract-OCR\\tessdata",
	    "C:\\Program Files (x86)\\Tesseract-OCR\\tessdata",
	};
	for (auto candidate : kCandidates) {
		if (ModelExistsIn(candidate, language)) {
			return std::string(candidate);
		}
	}
	return std::string();
}

// Convert raw poppler-style pixels into a 32bpp leptonica Pix. Returns nullptr
// on any failure so the caller can fall back to feeding tesseract the raw
// buffer.
Pix *BufferToPix(const unsigned char *base, int w, int h, int bpr, ImageFormat format) {
	if (!base || w <= 0 || h <= 0 || bpr <= 0) {
		return nullptr;
	}
	Pix *pix = pixCreate(w, h, 32);
	if (!pix) {
		return nullptr;
	}
	l_uint32 *data = pixGetData(pix);
	const int wpl = pixGetWpl(pix);
	const bool is_rgb24 = (format == ImageFormat::RGB24);
	for (int y = 0; y < h; y++) {
		const unsigned char *row = base + static_cast<size_t>(y) * static_cast<size_t>(bpr);
		l_uint32 *line = data + static_cast<size_t>(y) * static_cast<size_t>(wpl);
		for (int x = 0; x < w; x++) {
			unsigned char r, g, b;
			if (is_rgb24) {
				const unsigned char *px = row + static_cast<size_t>(x) * 3;
				r = px[0];
				g = px[1];
				b = px[2];
			} else {
				// argb32 in little-endian memory order is B,G,R,A.
				const unsigned char *px = row + static_cast<size_t>(x) * 4;
				b = px[0];
				g = px[1];
				r = px[2];
			}
			composeRGBPixel(r, g, b, &line[x]);
		}
	}
	return pix;
}

// Conservative OCR preprocessing pipeline. Every leptonica call is null-guarded;
// on any failure we return the best Pix produced so far (or nullptr).
//   1. grayscale
//   2. deskew (if |angle| > 0.3°)
//   3. binarize (Otsu)
//   4. despeckle (light 2x2 close-open at >=300 dpi)
Pix *PreprocessPixForOcr(Pix *pixs, int dpi) {
	if (!pixs) {
		return nullptr;
	}
	const int w = pixGetWidth(pixs);
	const int h = pixGetHeight(pixs);
	if (w < 16 || h < 16) {
		return nullptr;
	}

	Pix *pixg = nullptr;
	const int depth = pixGetDepth(pixs);
	if (depth >= 24) {
		pixg = pixConvertRGBToGray(pixs, 0.0f, 0.0f, 0.0f);
	} else if (depth == 8) {
		pixg = pixClone(pixs);
	} else {
		pixg = pixConvertTo8(pixs, 0);
	}
	if (!pixg) {
		return nullptr;
	}

	Pix *pixskew = pixThresholdToBinary(pixg, 130);
	if (pixskew) {
		l_float32 angle = 0.0f;
		l_float32 conf = 0.0f;
		if (pixFindSkewSweepAndSearch(pixskew, &angle, &conf, 4, 2, 7.0f, 1.0f, 0.01f) == 0 && conf > 0.0f &&
		    fabsf(angle) > 0.3f) {
			const l_float32 kDeg2Rad = 3.14159265358979323846f / 180.0f;
			Pix *pixr = pixRotate(pixg, angle * kDeg2Rad, L_ROTATE_AREA_MAP, L_BRING_IN_WHITE, 0, 0);
			if (pixr) {
				pixDestroy(&pixg);
				pixg = pixr;
			}
		}
		pixDestroy(&pixskew);
	}

	Pix *pixb = nullptr;
	if (pixOtsuAdaptiveThreshold(pixg, pixGetWidth(pixg), pixGetHeight(pixg), 0, 0, 0.1f, nullptr, &pixb) != 0 ||
	    !pixb) {
		pixSetResolution(pixg, dpi, dpi);
		return pixg;
	}
	pixDestroy(&pixg);

	if (dpi >= 300) {
		char seq[] = "c2.2 o2.2";
		Pix *pixc = pixMorphCompSequence(pixb, seq, 0);
		if (pixc) {
			pixDestroy(&pixb);
			pixb = pixc;
		}
	}
	pixSetResolution(pixb, dpi, dpi);
	return pixb;
}

bool InitTesseract(tesseract::TessBaseAPI &api, const Options &opt) {
	std::string datadir = ResolveTessdataDir(opt.language, opt.tessdata_dir);
	const char *datapath = datadir.empty() ? nullptr : datadir.c_str();
	if (api.Init(datapath, opt.language.c_str(), static_cast<tesseract::OcrEngineMode>(opt.oem)) != 0) {
		if (opt.best_effort) {
			return false;
		}
		throw std::runtime_error(
		    "read_pdf OCR: could not load the Tesseract model for language '" + opt.language +
		    "'. Install a language model — macOS: `brew install tesseract-lang`; Debian/Ubuntu: "
		    "`apt-get install tesseract-ocr-" +
		    opt.language +
		    "`; Windows: the UB Mannheim installer; or download " + opt.language +
		    ".traineddata from https://github.com/tesseract-ocr/tessdata_fast. Standard install locations "
		    "are auto-detected; if yours is non-standard, pass `tessdata_dir := '/path/to/tessdata'` or set "
		    "the TESSDATA_PREFIX env var.");
	}
	api.SetPageSegMode(static_cast<tesseract::PageSegMode>(opt.psm));
	return true;
}

void SetOcrImage(tesseract::TessBaseAPI &api, const unsigned char *data, int width, int height, int bytes_per_row,
                 ImageFormat format, int dpi, bool preprocess, Pix *&base, Pix *&processed) {
	base = preprocess ? BufferToPix(data, width, height, bytes_per_row, format) : nullptr;
	processed = base ? PreprocessPixForOcr(base, dpi) : nullptr;
	Pix *use = processed ? processed : base;
	if (use) {
		api.SetImage(use);
	} else {
		// Fallback: feed raw ARGB32/RGB24 bytes (tesseract grayscales internally).
		const int bpp = (format == ImageFormat::RGB24) ? 3 : 4;
		api.SetImage(data, width, height, bpp, bytes_per_row);
	}
}

void DestroyPixPair(Pix *processed, Pix *base) {
	if (processed) {
		pixDestroy(&processed);
	}
	if (base) {
		pixDestroy(&base);
	}
}

} // namespace

TextResult RecognizeText(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                         const Options &opt) {
	TextResult out;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}

	tesseract::TessBaseAPI api;
	if (!InitTesseract(api, opt)) {
		return out;
	}

	Pix *base = nullptr;
	Pix *processed = nullptr;
	SetOcrImage(api, data, width, height, bytes_per_row, format, opt.dpi, opt.preprocess, base, processed);

	char *text = api.GetUTF8Text();
	out.text = text ? std::string(text) : std::string();
	delete[] text;
	out.confidence = api.MeanTextConf();
	api.End();
	DestroyPixPair(processed, base);
	return out;
}

WordsResult RecognizeWords(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                           const Options &opt) {
	WordsResult out;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}

	tesseract::TessBaseAPI api;
	if (!InitTesseract(api, opt)) {
		return out;
	}

	Pix *base = nullptr;
	Pix *processed = nullptr;
	SetOcrImage(api, data, width, height, bytes_per_row, format, opt.dpi, opt.preprocess, base, processed);

	int rec = api.Recognize(0);
	if (rec != 0) {
		api.End();
		DestroyPixPair(processed, base);
		return out;
	}
	out.confidence = api.MeanTextConf();
	const double dpi = static_cast<double>(opt.dpi > 0 ? opt.dpi : 300);
	tesseract::ResultIterator *ri = api.GetIterator();
	if (ri) {
		do {
			if (ri->Empty(tesseract::RIL_WORD)) {
				continue;
			}
			char *word_text = ri->GetUTF8Text(tesseract::RIL_WORD);
			if (!word_text) {
				continue;
			}
			float conf = ri->Confidence(tesseract::RIL_WORD);
			int left = 0, top = 0, right = 0, bottom = 0;
			ri->BoundingBox(tesseract::RIL_WORD, &left, &top, &right, &bottom);
			Word w;
			w.text = std::string(word_text);
			delete[] word_text;
			// Convert tesseract top-left pixel coords → PDF points, origin bottom-left.
			w.x0 = left * 72.0 / dpi;
			w.x1 = right * 72.0 / dpi;
			w.y0 = (height - bottom) * 72.0 / dpi;
			w.y1 = (height - top) * 72.0 / dpi;
			w.confidence = conf;
			if (!w.text.empty()) {
				out.words.push_back(std::move(w));
			}
		} while (ri->Next(tesseract::RIL_WORD));
	}
	api.End();
	DestroyPixPair(processed, base);
	return out;
}

} // namespace pdf_ocr
