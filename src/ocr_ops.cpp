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

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pdf_ocr {
namespace {

// C ABI function pointers for an external OCR plugin (see ocr_ops.hpp).
using PluginRecognizeFn = int (*)(const unsigned char *, int, int, int, int, const char *, char **, int *);
using PluginFreeFn = void (*)(void *);

struct PluginHandle {
	void *lib = nullptr;
	PluginRecognizeFn recognize = nullptr;
	PluginFreeFn free_fn = nullptr;
	std::string path;
};

std::mutex &PluginMutex() {
	static std::mutex m;
	return m;
}

// Cached last-loaded plugin so repeated pages do not re-dlopen. Path-keyed:
// changing external_plugin reloads.
PluginHandle &CachedPlugin() {
	static PluginHandle h;
	return h;
}

void ClosePlugin(PluginHandle &h) {
	if (!h.lib) {
		return;
	}
#ifdef _WIN32
	FreeLibrary(static_cast<HMODULE>(h.lib));
#else
	dlclose(h.lib);
#endif
	h.lib = nullptr;
	h.recognize = nullptr;
	h.free_fn = nullptr;
	h.path.clear();
}

PluginHandle LoadPlugin(const std::string &path) {
	std::lock_guard<std::mutex> guard(PluginMutex());
	auto &cached = CachedPlugin();
	if (cached.lib && cached.path == path && cached.recognize) {
		return cached; // shallow copy of function pointers is fine
	}
	ClosePlugin(cached);
	if (path.empty()) {
		return cached;
	}
#ifdef _WIN32
	HMODULE lib = LoadLibraryA(path.c_str());
	if (!lib) {
		throw std::runtime_error("read_pdf OCR external: could not LoadLibrary '" + path + "'");
	}
	auto recognize = reinterpret_cast<PluginRecognizeFn>(GetProcAddress(lib, "pdf_ocr_plugin_recognize"));
	auto free_fn = reinterpret_cast<PluginFreeFn>(GetProcAddress(lib, "pdf_ocr_plugin_free"));
#else
	void *lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		const char *err = dlerror();
		throw std::runtime_error(std::string("read_pdf OCR external: could not dlopen '") + path +
		                         "': " + (err ? err : "unknown error"));
	}
	// Clear any prior error, then resolve symbols.
	dlerror();
	auto recognize = reinterpret_cast<PluginRecognizeFn>(dlsym(lib, "pdf_ocr_plugin_recognize"));
	const char *sym_err = dlerror();
	if (sym_err || !recognize) {
		dlclose(lib);
		throw std::runtime_error(std::string("read_pdf OCR external: missing symbol "
		                                     "pdf_ocr_plugin_recognize in '") +
		                         path + "': " + (sym_err ? sym_err : "null"));
	}
	auto free_fn = reinterpret_cast<PluginFreeFn>(dlsym(lib, "pdf_ocr_plugin_free"));
	// free_fn is optional — if missing we leak plugin-allocated strings (bad)
	// but still function; require it for a clean contract.
	sym_err = dlerror();
	if (sym_err || !free_fn) {
		dlclose(lib);
		throw std::runtime_error(std::string("read_pdf OCR external: missing symbol "
		                                     "pdf_ocr_plugin_free in '") +
		                         path + "': " + (sym_err ? sym_err : "null"));
	}
#endif
	if (!recognize || !free_fn) {
#ifdef _WIN32
		FreeLibrary(lib);
#else
		dlclose(lib);
#endif
		throw std::runtime_error("read_pdf OCR external: plugin '" + path +
		                         "' is missing pdf_ocr_plugin_recognize / pdf_ocr_plugin_free");
	}
	cached.lib = lib;
	cached.recognize = recognize;
	cached.free_fn = free_fn;
	cached.path = path;
	return cached;
}

TextResult RecognizeExternal(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                             const Options &opt) {
	TextResult out;
	out.backend_used = Backend::External;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}
	if (opt.external_plugin.empty()) {
		// No plugin, no shell: the SQL pipeline is the supported path.
		// Message is actionable — points at pdf_page_images / pdf_to_png + llm.
		if (opt.best_effort) {
			return out;
		}
		std::string msg = "read_pdf OCR external: no plugin loaded. Either (a) pass "
		                  "ocr_plugin := '/path/to/libyour_ocr.so' implementing the "
		                  "pdf_ocr_plugin_recognize C ABI (see ocr_ops.hpp), or (b) render "
		                  "pages with pdf_page_images / pdf_to_png and send the PNG BLOB to a "
		                  "vision-LLM via the DuckDB llm/ai extension (SOTA path without "
		                  "recompiling read_pdf).";
		if (!opt.external_endpoint.empty()) {
			msg += " Note: ocr_endpoint was set but in-process HTTP is not wired "
			       "(no shell-out); use the SQL image pipeline instead.";
		}
		throw std::runtime_error(msg);
	}
	PluginHandle plugin = LoadPlugin(opt.external_plugin);
	char *text_ptr = nullptr;
	int conf = -1;
	int rc = plugin.recognize(data, width, height, bytes_per_row, static_cast<int>(format), opt.language.c_str(),
	                          &text_ptr, &conf);
	if (rc != 0) {
		if (text_ptr && plugin.free_fn) {
			plugin.free_fn(text_ptr);
		}
		if (opt.best_effort) {
			return out;
		}
		throw std::runtime_error("read_pdf OCR external: plugin '" + opt.external_plugin + "' returned error code " +
		                         std::to_string(rc));
	}
	if (text_ptr) {
		out.text = std::string(text_ptr);
		plugin.free_fn(text_ptr);
	}
	if (conf >= 0) {
		out.confidence = conf;
	}
	return out;
}

TextResult RecognizeTesseractText(const unsigned char *data, int width, int height, int bytes_per_row,
                                  ImageFormat format, const Options &opt);

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

// True if <dir>/<lang>.traineddata OR <dir>/tessdata/<lang>.traineddata exists.
// Returns the directory to pass to TessBaseAPI::Init (parent of .traineddata).
std::string DirWithModel(const std::string &dir, const std::string &language) {
	if (dir.empty()) {
		return std::string();
	}
	if (ModelExistsIn(dir, language)) {
		return dir;
	}
	// TESSDATA_PREFIX is often the *parent* of tessdata/ (tesseract CLI convention).
	std::string nested = dir;
	char sep = nested.back();
	if (sep != '/' && sep != '\\') {
		nested += '/';
	}
	nested += "tessdata";
	if (ModelExistsIn(nested, language)) {
		return nested;
	}
	return std::string();
}

// Resolve the tessdata directory that contains <language>.traineddata.
// Order: explicit tessdata_dir → TESSDATA_PREFIX → standard package-manager
// install locations → bundled models extracted from this binary (eng).
// Returns empty if nothing was found.
std::string ResolveTessdataDir(const std::string &language, const std::string &explicit_dir) {
	if (!explicit_dir.empty()) {
		auto hit = DirWithModel(explicit_dir, language);
		if (!hit.empty()) {
			return hit;
		}
		// Caller forced a path — pass through so Init can emit a clear error
		// (do NOT fall through to bundled/system; explicit path is sticky).
		return explicit_dir;
	}
	const char *env = std::getenv("TESSDATA_PREFIX");
	if (env && *env) {
		auto hit = DirWithModel(env, language);
		if (!hit.empty()) {
			return hit;
		}
	}
	static const char *kCandidates[] = {
	    "/opt/homebrew/share/tessdata",
	    "/opt/homebrew/share", // parent form of TESSDATA_PREFIX
	    "/usr/local/share/tessdata",
	    "/usr/local/share",
	    "/usr/share/tessdata",
	    "/usr/share/tesseract-ocr/5/tessdata",
	    "/usr/share/tesseract-ocr/4.00/tessdata",
	    "/usr/share/tesseract-ocr/tessdata",
	    "/usr/local/share/tesseract-ocr/tessdata",
	    "C:\\Program Files\\Tesseract-OCR\\tessdata",
	    "C:\\Program Files (x86)\\Tesseract-OCR\\tessdata",
	};
	for (auto candidate : kCandidates) {
		auto hit = DirWithModel(candidate, language);
		if (!hit.empty()) {
			return hit;
		}
	}
	// Zero host install: eng.traineddata is compiled into the extension.
	return EnsureBundledTessdataDir(language);
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

// Silence tesseract/leptonica tprintf spam during Init only. Root cause of
// "Failed loading language" on stderr is calling Init when traineddata is
// missing or datapath is wrong — we never Init without a verified model file
// (see EnsureTesseract). This guard only covers residual library chatter
// (version banners, non-fatal path probes) so the DuckDB CLI stays clean.
struct StderrSilence {
#ifdef _WIN32
	int saved = -1;
	StderrSilence() {
		fflush(stderr);
		int devnull = _open("NUL", _O_WRONLY);
		if (devnull >= 0) {
			saved = _dup(_fileno(stderr));
			if (saved >= 0) {
				_dup2(devnull, _fileno(stderr));
			}
			_close(devnull);
		}
	}
	~StderrSilence() {
		if (saved >= 0) {
			fflush(stderr);
			_dup2(saved, _fileno(stderr));
			_close(saved);
		}
	}
#else
	int saved = -1;
	StderrSilence() {
		fflush(stderr);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			saved = dup(fileno(stderr));
			if (saved >= 0) {
				dup2(devnull, fileno(stderr));
			}
			close(devnull);
		}
	}
	~StderrSilence() {
		if (saved >= 0) {
			fflush(stderr);
			dup2(saved, fileno(stderr));
			close(saved);
		}
	}
#endif
	StderrSilence(const StderrSilence &) = delete;
	StderrSilence &operator=(const StderrSilence &) = delete;
};

static std::string MissingModelMessage(const Options &opt) {
	std::string msg = "read_pdf OCR: could not load the Tesseract model for language '" + opt.language + "'.";
	if (HasBundledTessdata(opt.language)) {
		msg += " English (eng) is bundled in this extension; this usually means tessdata_dir pointed "
		       "at a broken path, or the temp extract of the bundled model failed.";
	} else {
		msg += " Install a language model — macOS: `brew install tesseract-lang`; Debian/Ubuntu: "
		       "`apt-get install tesseract-ocr-" +
		       opt.language + "`; Windows: the UB Mannheim installer; or download " + opt.language +
		       ".traineddata from https://github.com/tesseract-ocr/tessdata_fast.";
	}
	msg += " Resolution order: tessdata_dir → TESSDATA_PREFIX → standard install paths → "
	       "bundled eng (when available).";
	return msg;
}

// Thread-local TessBaseAPI lifecycle (the #1 OCR performance fix).
//
// TessBaseAPI is NOT safe to share across threads, but Init() reloads traineddata
// from disk and rebuilds language models — doing that per page destroys throughput.
// DuckDB table-function workers map 1:1 onto OS threads for a scan, so a
// thread_local engine is the natural match for TableFunctionLocalState without
// pulling tesseract types across the ocr_ops isolation boundary into pdf_extension.
//
// Reuse rules:
//   - Init once per (thread, language, oem, datapath, config, vars)
//   - SetPageSegMode only when psm changes
//   - Clear() between pages (free page bitmap/results) — never End() in the hot loop
//   - Never call Init unless <datapath>/<lang>.traineddata exists on disk
//   - After Init: ReadConfigFile (if set) then SetVariable for each vars entry
struct ThreadTessEngine {
	tesseract::TessBaseAPI api;
	bool ready = false;
	std::string language;
	int oem = -1;
	std::string datapath;
	int psm = -1;
	std::string vars_key; // config path + serialized vars (Init-boundary knobs)

	~ThreadTessEngine() {
		if (ready) {
			api.End();
			ready = false;
		}
	}
};

ThreadTessEngine &TlsTess() {
	// thread_local: one persistent engine per worker thread
	thread_local ThreadTessEngine eng;
	return eng;
}

// Fingerprint of post-Init Tess knobs so a changed config/vars forces re-Init
// (SetVariable state is not rolled back without End).
std::string VarsConfigKey(const Options &opt) {
	std::string key = opt.config;
	key.push_back('\0');
	for (const auto &kv : opt.vars) {
		key.append(kv.first);
		key.push_back('=');
		key.append(kv.second);
		key.push_back('\0');
	}
	return key;
}

void ApplyTessConfigAndVars(tesseract::TessBaseAPI &api, const Options &opt) {
	if (!opt.config.empty()) {
		api.ReadConfigFile(opt.config.c_str());
	}
	for (const auto &kv : opt.vars) {
		if (kv.first.empty()) {
			continue;
		}
		// Tess returns false for unknown names; ignore so typos don't abort scans.
		// (Known-good keys still take effect; shellfs CLI hatch remains for full CLI.)
		(void)api.SetVariable(kv.first.c_str(), kv.second.c_str());
	}
}

bool EnsureTesseract(const Options &opt) {
	// Resolve + verify the model file BEFORE any TessBaseAPI::Init call.
	// Init with a missing/wrong path is what prints "Failed loading language"
	// and "Please make sure TESSDATA_PREFIX…" to stderr — fix at source.
	std::string datadir = ResolveTessdataDir(opt.language, opt.tessdata_dir);
	if (datadir.empty() || !ModelExistsIn(datadir, opt.language)) {
		if (opt.best_effort) {
			return false;
		}
		throw std::runtime_error(MissingModelMessage(opt));
	}

	auto &eng = TlsTess();
	const std::string vars_key = VarsConfigKey(opt);
	const bool need_init = !eng.ready || eng.language != opt.language || eng.oem != opt.oem ||
	                       eng.datapath != datadir || eng.vars_key != vars_key;
	if (need_init) {
		if (eng.ready) {
			eng.api.End();
			eng.ready = false;
		}
		int init_rc;
		{
			// Verified path only — silence residual library chatter during Init.
			StderrSilence silence;
			init_rc =
			    eng.api.Init(datadir.c_str(), opt.language.c_str(), static_cast<tesseract::OcrEngineMode>(opt.oem));
		}
		if (init_rc != 0) {
			if (opt.best_effort) {
				return false;
			}
			throw std::runtime_error(MissingModelMessage(opt));
		}
		// Config then vars — matches Tess CLI: config file, then -c overrides.
		ApplyTessConfigAndVars(eng.api, opt);
		eng.ready = true;
		eng.language = opt.language;
		eng.oem = opt.oem;
		eng.datapath = datadir;
		eng.vars_key = vars_key;
		eng.psm = -1; // force SetPageSegMode below
	}
	if (eng.psm != opt.psm) {
		eng.api.SetPageSegMode(static_cast<tesseract::PageSegMode>(opt.psm));
		eng.psm = opt.psm;
	}
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

// Pull UTF-8 / HOCR / TSV from a TessBaseAPI that already has SetImage.
// HOCR/TSV use page_number 0 (single image). Unknown format → text.
void FillTextFromApi(tesseract::TessBaseAPI &api, const std::string &fmt, TextResult &out) {
	out.format = fmt.empty() ? "text" : fmt;
	char *buf = nullptr;
	if (out.format == "hocr") {
		buf = api.GetHOCRText(0);
	} else if (out.format == "tsv") {
		buf = api.GetTSVText(0);
	} else {
		out.format = "text";
		buf = api.GetUTF8Text();
	}
	out.text = buf ? std::string(buf) : std::string();
	delete[] buf;
	out.confidence = api.MeanTextConf();
}

TextResult RecognizeTesseractText(const unsigned char *data, int width, int height, int bytes_per_row,
                                  ImageFormat format, const Options &opt) {
	TextResult out;
	out.backend_used = Backend::Tesseract;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}

	if (!EnsureTesseract(opt)) {
		return out;
	}
	auto &api = TlsTess().api;

	Pix *base = nullptr;
	Pix *processed = nullptr;
	SetOcrImage(api, data, width, height, bytes_per_row, format, opt.dpi, opt.preprocess, base, processed);

	// read_pdf page path always wants plain text; format on Options is for
	// RecognizeImageBlob / ocr_image TVF. Keep text-only here for stability.
	FillTextFromApi(api, "text", out);
	// Keep the engine warm for the next page on this thread; free only page state.
	api.Clear();
	DestroyPixPair(processed, base);
	return out;
}

WordsResult RecognizeTesseractWords(const unsigned char *data, int width, int height, int bytes_per_row,
                                    ImageFormat format, const Options &opt) {
	WordsResult out;
	out.backend_used = Backend::Tesseract;
	if (!data || width <= 0 || height <= 0) {
		return out;
	}

	if (!EnsureTesseract(opt)) {
		return out;
	}
	auto &api = TlsTess().api;

	Pix *base = nullptr;
	Pix *processed = nullptr;
	SetOcrImage(api, data, width, height, bytes_per_row, format, opt.dpi, opt.preprocess, base, processed);

	int rec = api.Recognize(0);
	if (rec != 0) {
		api.Clear();
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
	api.Clear();
	DestroyPixPair(processed, base);
	return out;
}

} // namespace

static std::string LowerAscii(const std::string &name) {
	std::string lower;
	lower.reserve(name.size());
	for (unsigned char c : name) {
		lower.push_back(static_cast<char>(std::tolower(c)));
	}
	return lower;
}

Backend BackendFromString(const std::string &name) {
	std::string lower = LowerAscii(name);
	if (lower.empty() || lower == "tesseract" || lower == "default") {
		return Backend::Tesseract;
	}
	if (lower == "external" || lower == "plugin" || lower == "sota") {
		return Backend::External;
	}
	throw std::runtime_error("read_pdf: ocr_backend must be 'tesseract' or 'external' (got '" + name + "')");
}

std::string NormalizeOutputFormat(const std::string &name) {
	std::string lower = LowerAscii(name);
	if (lower.empty() || lower == "text" || lower == "utf8" || lower == "plain") {
		return "text";
	}
	if (lower == "hocr" || lower == "html" || lower == "hocr_html") {
		return "hocr";
	}
	if (lower == "tsv" || lower == "tsvtext" || lower == "tsv_text") {
		return "tsv";
	}
	throw std::runtime_error("ocr format must be 'text', 'hocr', or 'tsv' (got '" + name + "')");
}

const char *BackendName(Backend b) {
	switch (b) {
	case Backend::External:
		return "external";
	case Backend::Tesseract:
	default:
		return "tesseract";
	}
}

TextResult RecognizeText(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                         const Options &opt) {
	if (opt.backend == Backend::External) {
		return RecognizeExternal(data, width, height, bytes_per_row, format, opt);
	}
	return RecognizeTesseractText(data, width, height, bytes_per_row, format, opt);
}

WordsResult RecognizeWords(const unsigned char *data, int width, int height, int bytes_per_row, ImageFormat format,
                           const Options &opt) {
	if (opt.backend == Backend::External) {
		// External plugins currently expose page-level text only. Word boxes
		// would require a richer ABI; return empty words with the page text
		// folded into a single synthetic word when text is available so table
		// paths degrade gracefully rather than hanging.
		WordsResult wr;
		wr.backend_used = Backend::External;
		TextResult tr = RecognizeExternal(data, width, height, bytes_per_row, format, opt);
		wr.confidence = tr.confidence;
		if (!tr.text.empty()) {
			Word w;
			w.text = tr.text;
			w.x0 = 0;
			w.y0 = 0;
			w.x1 = width * 72.0 / (opt.dpi > 0 ? opt.dpi : 300);
			w.y1 = height * 72.0 / (opt.dpi > 0 ? opt.dpi : 300);
			w.confidence = static_cast<float>(tr.confidence);
			wr.words.push_back(std::move(w));
		}
		return wr;
	}
	return RecognizeTesseractWords(data, width, height, bytes_per_row, format, opt);
}

TextResult RecognizeImageBlob(const unsigned char *data, size_t size, const Options &opt) {
	TextResult out;
	out.backend_used = Backend::Tesseract;
	if (!data || size == 0) {
		if (opt.best_effort) {
			return out;
		}
		// Shared by tesseract_ocr scalar and ocr_image TVF — keep name-neutral.
		throw std::runtime_error("empty image BLOB");
	}
	if (opt.backend == Backend::External) {
		if (opt.best_effort) {
			out.backend_used = Backend::External;
			return out;
		}
		throw std::runtime_error("external OCR backend is not supported for encoded image BLOBs "
		                         "(use tesseract, or decode externally and call the plugin C ABI)");
	}

	// leptonica owns the decoder (PNG/JPEG/TIFF/…); no poppler dependency here.
	// Silence residual "Error in pixReadMem: …" chatter on bad inputs (same pattern
	// as TessBaseAPI::Init — actionable errors go through our throw message).
	Pix *pix = nullptr;
	{
		StderrSilence silence;
		pix = pixReadMem(data, size);
	}
	if (!pix) {
		if (opt.best_effort) {
			return out;
		}
		throw std::runtime_error("could not decode image BLOB "
		                         "(expected PNG/JPEG/etc. readable by leptonica, e.g. from poppler_render_page)");
	}

	if (!EnsureTesseract(opt)) {
		pixDestroy(&pix);
		return out;
	}
	auto &api = TlsTess().api;

	const int dpi = opt.dpi > 0 ? opt.dpi : 300;
	// Prefer embedded resolution when present; otherwise stamp dpi for preprocess.
	{
		l_int32 xres = 0, yres = 0;
		pixGetResolution(pix, &xres, &yres);
		if (xres <= 0 || yres <= 0) {
			pixSetResolution(pix, dpi, dpi);
		}
	}

	Pix *processed = nullptr;
	if (opt.preprocess) {
		processed = PreprocessPixForOcr(pix, dpi);
	}
	Pix *use = processed ? processed : pix;
	api.SetImage(use);

	// Callers (ocr_image) validate format; empty / default → text.
	const std::string fmt = NormalizeOutputFormat(opt.format.empty() ? "text" : opt.format);
	FillTextFromApi(api, fmt, out);
	api.Clear();
	DestroyPixPair(processed, pix);
	return out;
}

} // namespace pdf_ocr
