#define DUCKDB_EXTENSION_MAIN

#include "pdf_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

// CLI/conversion engine (posix_spawn over pdftotext/pdftohtml/pdftocairo) — pure std C++
#include "pdf_cli.hpp"

// poppler-cpp — text extraction + page rendering + metadata
#include <poppler-document.h>
#include <poppler-page.h>
#include <poppler-page-renderer.h>
#include <poppler-image.h>

// tesseract — OCR for scanned / image-only PDFs
#include <tesseract/baseapi.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Shared helpers
//===--------------------------------------------------------------------===//
static poppler::page::text_layout_enum LayoutFromString(const string &s, bool parse_tables) {
	if (parse_tables) {
		// physical layout preserves column alignment — the portable approximation of
		// poppler core's table mode (true cell extraction is a core-engine follow-up).
		return poppler::page::physical_layout;
	}
	auto l = StringUtil::Lower(s);
	if (l == "physical") {
		return poppler::page::physical_layout;
	}
	if (l == "raw") {
		return poppler::page::raw_order_layout;
	}
	return poppler::page::non_raw_non_physical_layout; // 'reading'
}

static string UStringToUtf8(const poppler::ustring &u) {
	poppler::byte_array b = u.to_utf8();
	return string(b.begin(), b.end());
}

// A <lang>.traineddata model lives in some directory. Return that directory if
// the file is there, else empty string.
static bool ModelExistsIn(const string &dir, const string &language) {
	if (dir.empty()) {
		return false;
	}
	string path = dir;
	char sep = path.back();
	if (sep != '/' && sep != '\\') {
		path += '/';
	}
	path += language + ".traineddata";
	std::ifstream f(path.c_str());
	return f.good();
}

// Resolve the tessdata directory that contains <language>.traineddata, so OCR
// works out-of-the-box without the user setting TESSDATA_PREFIX. Resolution
// order (first hit wins):
//   1. explicit `tessdata_dir` named parameter (connect an existing install)
//   2. TESSDATA_PREFIX env var (Tesseract's own native mechanism)
//   3. probe the standard locations the OS package managers install models to
// Returns empty string if nothing was found; the caller then errors loudly.
static string ResolveTessdataDir(const string &language, const string &explicit_dir) {
	// 1. Explicit param — trust it even if the file probe fails (the user may
	//    have a non-standard naming or a model alias); only prefer it if present.
	if (!explicit_dir.empty()) {
		return explicit_dir;
	}
	// 2. TESSDATA_PREFIX — return it so we pass it explicitly to Init().
	const char *env = std::getenv("TESSDATA_PREFIX");
	if (env && *env && ModelExistsIn(env, language)) {
		return string(env);
	}
	// 3. Probe standard package-manager install locations.
	static const char *kCandidates[] = {
	    // macOS Homebrew (Apple Silicon / Intel)
	    "/opt/homebrew/share/tessdata",
	    "/usr/local/share/tessdata",
	    // Debian/Ubuntu apt + generic Linux
	    "/usr/share/tessdata",
	    "/usr/share/tesseract-ocr/5/tessdata",
	    "/usr/share/tesseract-ocr/4.00/tessdata",
	    "/usr/share/tesseract-ocr/tessdata",
	    "/usr/local/share/tesseract-ocr/tessdata",
	    // Windows (UB Mannheim installer default)
	    "C:\\Program Files\\Tesseract-OCR\\tessdata",
	    "C:\\Program Files (x86)\\Tesseract-OCR\\tessdata",
	};
	for (auto candidate : kCandidates) {
		if (ModelExistsIn(candidate, language)) {
			return string(candidate);
		}
	}
	return string();
}

// Render a poppler page and OCR it with tesseract, honoring engine knobs.
static string OcrPage(poppler::page *page, const string &language, int dpi, int psm, int oem,
                      const string &tessdata_dir) {
	poppler::page_renderer renderer;
	renderer.set_render_hint(poppler::page_renderer::antialiasing, true);
	renderer.set_render_hint(poppler::page_renderer::text_antialiasing, true);

	poppler::image img = renderer.render_page(page, dpi, dpi);
	if (!img.is_valid()) {
		return string();
	}

	tesseract::TessBaseAPI api;
	// Tesseract needs a <lang>.traineddata model at runtime. vcpkg/most package
	// managers do NOT ship language models, so OCR requires the user to install
	// one. We auto-detect the standard install dirs (so a plain `brew install
	// tesseract-lang` / `apt-get install tesseract-ocr-eng` Just Works with no
	// env var), honor an explicit `tessdata_dir` parameter, and fall back to
	// TESSDATA_PREFIX. If none has the model, fail loudly with instructions.
	string datadir = ResolveTessdataDir(language, tessdata_dir);
	const char *datapath = datadir.empty() ? nullptr : datadir.c_str();
	if (api.Init(datapath, language.c_str(), static_cast<tesseract::OcrEngineMode>(oem)) != 0) {
		throw IOException(
		    "read_pdf OCR: could not load the Tesseract model for language '%s'. Install a language model — "
		    "macOS: `brew install tesseract-lang`; Debian/Ubuntu: `apt-get install tesseract-ocr-%s`; Windows: "
		    "the UB Mannheim installer; or download %s.traineddata from "
		    "https://github.com/tesseract-ocr/tessdata_fast. Standard install locations are auto-detected; if "
		    "yours is non-standard, pass `tessdata_dir := '/path/to/tessdata'` or set the TESSDATA_PREFIX env var.",
		    language, language, language);
	}
	api.SetPageSegMode(static_cast<tesseract::PageSegMode>(psm));

	const int bytes_per_pixel = 4; // poppler renders argb32; tesseract grayscales internally
	api.SetImage(reinterpret_cast<const unsigned char *>(img.const_data()), img.width(), img.height(), bytes_per_pixel,
	             img.bytes_per_row());

	char *out = api.GetUTF8Text();
	string text = out ? string(out) : string();
	delete[] out;
	api.End();
	return text;
}

//===--------------------------------------------------------------------===//
// Common options bag (shared by read_pdf / read_pdf_words)
//===--------------------------------------------------------------------===//
struct PdfOptions {
	bool force_ocr = false;
	bool auto_ocr = true;
	string ocr_language = "eng";
	int32_t ocr_dpi = 300;
	int32_t ocr_psm = 3; // PSM_AUTO
	int32_t ocr_oem = 3; // OEM_DEFAULT
	string tessdata_dir; // optional: directory containing <lang>.traineddata
	string layout = "reading";
	bool parse_tables = false;
	string password;
	int32_t first_page = 1;
	int32_t last_page = -1; // -1 => through end
};

static void ParseNamed(const named_parameter_map_t &params, PdfOptions &o) {
	for (auto &kv : params) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "ocr") {
			o.force_ocr = BooleanValue::Get(kv.second);
		} else if (key == "auto_ocr") {
			o.auto_ocr = BooleanValue::Get(kv.second);
		} else if (key == "ocr_language") {
			o.ocr_language = StringValue::Get(kv.second);
		} else if (key == "ocr_dpi") {
			o.ocr_dpi = IntegerValue::Get(kv.second);
		} else if (key == "ocr_psm") {
			o.ocr_psm = IntegerValue::Get(kv.second);
		} else if (key == "ocr_oem") {
			o.ocr_oem = IntegerValue::Get(kv.second);
		} else if (key == "tessdata_dir") {
			o.tessdata_dir = StringValue::Get(kv.second);
		} else if (key == "layout") {
			o.layout = StringValue::Get(kv.second);
		} else if (key == "parse_tables") {
			o.parse_tables = BooleanValue::Get(kv.second);
		} else if (key == "password") {
			o.password = StringValue::Get(kv.second);
		} else if (key == "first_page") {
			o.first_page = IntegerValue::Get(kv.second);
		} else if (key == "last_page") {
			o.last_page = IntegerValue::Get(kv.second);
		}
	}
}

static void AddCommonNamedParams(TableFunction &fn) {
	fn.named_parameters["ocr"] = LogicalType::BOOLEAN;
	fn.named_parameters["auto_ocr"] = LogicalType::BOOLEAN;
	fn.named_parameters["ocr_language"] = LogicalType::VARCHAR;
	fn.named_parameters["ocr_dpi"] = LogicalType::INTEGER;
	fn.named_parameters["ocr_psm"] = LogicalType::INTEGER;
	fn.named_parameters["ocr_oem"] = LogicalType::INTEGER;
	fn.named_parameters["tessdata_dir"] = LogicalType::VARCHAR;
	fn.named_parameters["layout"] = LogicalType::VARCHAR;
	fn.named_parameters["parse_tables"] = LogicalType::BOOLEAN;
	fn.named_parameters["password"] = LogicalType::VARCHAR;
	fn.named_parameters["first_page"] = LogicalType::INTEGER;
	fn.named_parameters["last_page"] = LogicalType::INTEGER;
}

static vector<string> ResolveFiles(ClientContext &context, const string &pattern) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto globbed = fs.GlobFiles(pattern, FileGlobOptions::ALLOW_EMPTY);
	vector<string> files;
	if (globbed.empty()) {
		files.push_back(pattern);
	} else {
		for (auto &f : globbed) {
			files.push_back(f.path);
		}
	}
	return files;
}

// Read all bytes of a path through DuckDB's FileSystem (works with any fs).
static void ReadAllBytes(ClientContext &context, const string &path, string &out) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	int64_t size = fs.GetFileSize(*handle);
	out.resize(size);
	// loop until fully read — fs.Read may return a short count (P0-4)
	int64_t total = 0;
	while (total < size) {
		int64_t n = fs.Read(*handle, &out[total], size - total);
		if (n <= 0) {
			break;
		}
		total += n;
	}
	if (total != size) {
		throw IOException("read_pdf: short read on '%s' (%lld of %lld bytes)", path, (long long)total, (long long)size);
	}
}

static unique_ptr<poppler::document> LoadDoc(const string &bytes, const string &password, const string &path) {
	auto doc = unique_ptr<poppler::document>(
	    poppler::document::load_from_raw_data(bytes.data(), (int)bytes.size(), password, password));
	if (!doc || doc->is_locked()) {
		throw IOException("read_pdf: could not open '%s' (corrupt, empty, or wrong password)", path);
	}
	return doc;
}

//===--------------------------------------------------------------------===//
// read_pdf  -> (file, page, page_count, text)
//===--------------------------------------------------------------------===//
struct ReadPdfBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};

struct ReadPdfGlobalState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	int page_idx = 0; // 0-based
	int page_count = 0;
	int last_page_0 = 0; // exclusive upper bound (0-based)
	string file_bytes;
	unique_ptr<poppler::document> doc;
	string current_file;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadPdfBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);

	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::DOUBLE,  LogicalType::DOUBLE};
	names = {"filename", "page", "page_count", "text", "width", "height"};
	return std::move(result);
}

static void OpenForRead(ClientContext &context, const ReadPdfBindData &bind, ReadPdfGlobalState &g) {
	g.current_file = bind.files[g.file_idx];
	ReadAllBytes(context, g.current_file, g.file_bytes);
	g.doc = LoadDoc(g.file_bytes, bind.opt.password, g.current_file);
	g.page_count = g.doc->pages();
	g.page_idx = bind.opt.first_page > 0 ? bind.opt.first_page - 1 : 0;
	g.last_page_0 = bind.opt.last_page < 0 ? g.page_count : MinValue<int>(bind.opt.last_page, g.page_count);
}

static unique_ptr<GlobalTableFunctionState> ReadPdfInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadPdfBindData>();
	auto g = make_uniq<ReadPdfGlobalState>();
	if (!bind.files.empty()) {
		OpenForRead(context, bind, *g);
	}
	return std::move(g);
}

static void ReadPdfScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadPdfBindData>();
	auto &g = data_p.global_state->Cast<ReadPdfGlobalState>();
	auto layout = LayoutFromString(bind.opt.layout, bind.opt.parse_tables);

	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		while (g.doc && g.page_idx >= g.last_page_0) {
			g.file_idx++;
			if (g.file_idx >= bind.files.size()) {
				g.doc.reset();
				break;
			}
			OpenForRead(context, bind, g);
		}
		if (!g.doc) {
			break;
		}

		unique_ptr<poppler::page> page(g.doc->create_page(g.page_idx));
		string text;
		double width = 0.0;
		double height = 0.0;
		if (page) {
			auto rect = page->page_rect();
			width = rect.width();
			height = rect.height();
			if (!bind.opt.force_ocr) {
				text = UStringToUtf8(page->text(poppler::rectf(), layout));
			}
			bool blank = text.find_first_not_of(" \t\r\n\f\v") == string::npos;
			if (bind.opt.force_ocr || (bind.opt.auto_ocr && blank)) {
				string ocr = OcrPage(page.get(), bind.opt.ocr_language, bind.opt.ocr_dpi, bind.opt.ocr_psm,
				                     bind.opt.ocr_oem, bind.opt.tessdata_dir);
				if (!ocr.empty()) {
					text = ocr;
				}
			}
		}

		output.SetValue(0, count, Value(g.current_file));
		output.SetValue(1, count, Value::INTEGER(g.page_idx + 1));
		output.SetValue(2, count, Value::INTEGER(g.page_count));
		output.SetValue(3, count, Value(text));
		output.SetValue(4, count, Value::DOUBLE(width));
		output.SetValue(5, count, Value::DOUBLE(height));
		g.page_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_meta  -> one row per file
//===--------------------------------------------------------------------===//
struct ReadPdfMetaBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};
struct ReadPdfMetaState : public GlobalTableFunctionState {
	idx_t idx = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadPdfMetaBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfMetaBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::BOOLEAN};
	names = {"filename", "title",    "author", "subject",     "keywords",
	         "creator",  "producer", "pages",  "pdf_version", "encrypted"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ReadPdfMetaInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ReadPdfMetaState>();
}

static void ReadPdfMetaScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadPdfMetaBindData>();
	auto &st = data_p.global_state->Cast<ReadPdfMetaState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.idx < bind.files.size()) {
		auto &path = bind.files[st.idx];
		string bytes;
		ReadAllBytes(context, path, bytes);
		auto doc = LoadDoc(bytes, bind.opt.password, path);

		int major = 0, minor = 0;
		doc->get_pdf_version(&major, &minor);

		output.SetValue(0, count, Value(path));
		output.SetValue(1, count, Value(UStringToUtf8(doc->info_key("Title"))));
		output.SetValue(2, count, Value(UStringToUtf8(doc->info_key("Author"))));
		output.SetValue(3, count, Value(UStringToUtf8(doc->info_key("Subject"))));
		output.SetValue(4, count, Value(UStringToUtf8(doc->info_key("Keywords"))));
		output.SetValue(5, count, Value(UStringToUtf8(doc->info_key("Creator"))));
		output.SetValue(6, count, Value(UStringToUtf8(doc->info_key("Producer"))));
		output.SetValue(7, count, Value::INTEGER(doc->pages()));
		output.SetValue(8, count, Value(to_string(major) + "." + to_string(minor)));
		output.SetValue(9, count, Value::BOOLEAN(doc->is_encrypted()));
		st.idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_words  -> one row per word with bbox + font (layout/table substrate)
//===--------------------------------------------------------------------===//
struct ReadPdfWordsBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};

static unique_ptr<FunctionData> ReadPdfWordsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfWordsBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::DOUBLE,
	                LogicalType::DOUBLE,  LogicalType::VARCHAR, LogicalType::DOUBLE};
	names = {"filename", "page", "word", "x0", "y0", "x1", "y1", "font_name", "font_size"};
	return std::move(result);
}

struct ReadPdfWordsState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	int page_idx = 0;
	int page_count = 0;
	int last_page_0 = 0;
	idx_t word_idx = 0;
	string file_bytes;
	unique_ptr<poppler::document> doc;
	std::vector<poppler::text_box> boxes;
	string current_file;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static void WordsOpenFile(ClientContext &context, const ReadPdfWordsBindData &bind, ReadPdfWordsState &g) {
	g.current_file = bind.files[g.file_idx];
	ReadAllBytes(context, g.current_file, g.file_bytes);
	g.doc = LoadDoc(g.file_bytes, bind.opt.password, g.current_file);
	g.page_count = g.doc->pages();
	g.page_idx = bind.opt.first_page > 0 ? bind.opt.first_page - 1 : 0;
	g.last_page_0 = bind.opt.last_page < 0 ? g.page_count : MinValue<int>(bind.opt.last_page, g.page_count);
	g.boxes.clear();
	g.word_idx = 0;
}

static bool WordsLoadPage(ReadPdfWordsState &g) {
	g.boxes.clear();
	g.word_idx = 0;
	if (g.page_idx >= g.last_page_0) {
		return false;
	}
	unique_ptr<poppler::page> page(g.doc->create_page(g.page_idx));
	if (page) {
		g.boxes = page->text_list(poppler::page::text_list_include_font);
	}
	return true;
}

static unique_ptr<GlobalTableFunctionState> ReadPdfWordsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadPdfWordsBindData>();
	auto g = make_uniq<ReadPdfWordsState>();
	if (!bind.files.empty()) {
		WordsOpenFile(context, bind, *g);
		WordsLoadPage(*g);
	}
	return std::move(g);
}

static void ReadPdfWordsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadPdfWordsBindData>();
	auto &g = data_p.global_state->Cast<ReadPdfWordsState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (!g.doc) {
			break;
		}
		if (g.word_idx >= g.boxes.size()) {
			// advance page, then file
			g.page_idx++;
			if (g.page_idx >= g.last_page_0) {
				g.file_idx++;
				if (g.file_idx >= bind.files.size()) {
					g.doc.reset();
					break;
				}
				WordsOpenFile(context, bind, g);
			}
			WordsLoadPage(g);
			continue;
		}
		auto &b = g.boxes[g.word_idx];
		auto r = b.bbox();
		output.SetValue(0, count, Value(g.current_file));
		output.SetValue(1, count, Value::INTEGER(g.page_idx + 1));
		output.SetValue(2, count, Value(UStringToUtf8(b.text())));
		output.SetValue(3, count, Value::DOUBLE(r.x()));
		output.SetValue(4, count, Value::DOUBLE(r.y()));
		output.SetValue(5, count, Value::DOUBLE(r.x() + r.width()));
		output.SetValue(6, count, Value::DOUBLE(r.y() + r.height()));
		output.SetValue(7, count, b.has_font_info() ? Value(b.get_font_name()) : Value(LogicalType::VARCHAR));
		output.SetValue(8, count, b.has_font_info() ? Value::DOUBLE(b.get_font_size()) : Value(LogicalType::DOUBLE));
		g.word_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_lines  -> one row per layout-preserving line of text
// (analog to the community `read_lines` extension, but PDF-aware). Splits each
// page's extracted text on newlines; honors the same `layout` knob as read_pdf.
//===--------------------------------------------------------------------===//
struct ReadPdfLinesBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};

static unique_ptr<FunctionData> ReadPdfLinesBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfLinesBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR};
	names = {"filename", "page", "line", "text"};
	return std::move(result);
}

struct ReadPdfLinesState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	int page_idx = 0; // 0-based
	int page_count = 0;
	int last_page_0 = 0;
	idx_t line_idx = 0;
	string file_bytes;
	unique_ptr<poppler::document> doc;
	vector<string> lines;
	string current_file;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static void LinesOpenFile(ClientContext &context, const ReadPdfLinesBindData &bind, ReadPdfLinesState &g) {
	g.current_file = bind.files[g.file_idx];
	ReadAllBytes(context, g.current_file, g.file_bytes);
	g.doc = LoadDoc(g.file_bytes, bind.opt.password, g.current_file);
	g.page_count = g.doc->pages();
	g.page_idx = bind.opt.first_page > 0 ? bind.opt.first_page - 1 : 0;
	g.last_page_0 = bind.opt.last_page < 0 ? g.page_count : MinValue<int>(bind.opt.last_page, g.page_count);
	g.lines.clear();
	g.line_idx = 0;
}

static bool LinesLoadPage(ReadPdfLinesState &g, const PdfOptions &opt) {
	g.lines.clear();
	g.line_idx = 0;
	if (g.page_idx >= g.last_page_0) {
		return false;
	}
	auto layout = LayoutFromString(opt.layout, false);
	unique_ptr<poppler::page> page(g.doc->create_page(g.page_idx));
	if (page) {
		string text = UStringToUtf8(page->text(poppler::rectf(), layout));
		size_t start = 0;
		while (start <= text.size()) {
			size_t nl = text.find('\n', start);
			if (nl == string::npos) {
				g.lines.push_back(text.substr(start));
				break;
			}
			g.lines.push_back(text.substr(start, nl - start));
			start = nl + 1;
		}
		// Drop trailing empty/whitespace-only lines so a page does not emit a
		// run of blank rows; interior blank lines are preserved as-is.
		while (!g.lines.empty() && g.lines.back().find_first_not_of(" \t\r\f\v") == string::npos) {
			g.lines.pop_back();
		}
	}
	return true;
}

static unique_ptr<GlobalTableFunctionState> ReadPdfLinesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadPdfLinesBindData>();
	auto g = make_uniq<ReadPdfLinesState>();
	if (!bind.files.empty()) {
		LinesOpenFile(context, bind, *g);
		LinesLoadPage(*g, bind.opt);
	}
	return std::move(g);
}

static void ReadPdfLinesScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadPdfLinesBindData>();
	auto &g = data_p.global_state->Cast<ReadPdfLinesState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (!g.doc) {
			break;
		}
		if (g.line_idx >= g.lines.size()) {
			g.page_idx++;
			if (g.page_idx >= g.last_page_0) {
				g.file_idx++;
				if (g.file_idx >= bind.files.size()) {
					g.doc.reset();
					break;
				}
				LinesOpenFile(context, bind, g);
			}
			LinesLoadPage(g, bind.opt);
			continue;
		}
		output.SetValue(0, count, Value(g.current_file));
		output.SetValue(1, count, Value::INTEGER(g.page_idx + 1));
		output.SetValue(2, count, Value::INTEGER((int)g.line_idx + 1));
		output.SetValue(3, count, Value(g.lines[g.line_idx]));
		g.line_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_tables  -> one row per table row, cells as VARCHAR[]
// Backed by pdf_cli::ReconstructTables (pdftotext -bbox-layout reconstruction).
//===--------------------------------------------------------------------===//
struct ReadPdfTablesBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};
struct TableRowOut {
	string file;
	int page;
	int table_index;
	int row_index;
	std::vector<std::string> cells; // matches pdfcli::Table (pure std types)
};
struct ReadPdfTablesState : public GlobalTableFunctionState {
	vector<TableRowOut> rows;
	idx_t idx = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadPdfTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfTablesBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER,
	                LogicalType::LIST(LogicalType::VARCHAR)};
	names = {"filename", "page", "table_index", "row_index", "cells"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ReadPdfTablesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadPdfTablesBindData>();
	auto st = make_uniq<ReadPdfTablesState>();
	for (auto &f : bind.files) {
		std::vector<pdfcli::Table> tables;
		try {
			tables = pdfcli::ReconstructTables(f, bind.opt.first_page, bind.opt.last_page, "");
		} catch (std::exception &e) {
			throw InvalidInputException("read_pdf_tables: %s", e.what());
		}
		for (idx_t ti = 0; ti < tables.size(); ti++) {
			auto &t = tables[ti];
			for (idx_t r = 0; r < t.rows.size(); r++) {
				st->rows.push_back(TableRowOut {f, t.page, (int)ti, (int)r, t.rows[r]});
			}
		}
	}
	return std::move(st);
}

static void ReadPdfTablesScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &st = data_p.global_state->Cast<ReadPdfTablesState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.idx < st.rows.size()) {
		auto &row = st.rows[st.idx];
		output.SetValue(0, count, Value(row.file));
		output.SetValue(1, count, Value::INTEGER(row.page));
		output.SetValue(2, count, Value::INTEGER(row.table_index));
		output.SetValue(3, count, Value::INTEGER(row.row_index));
		vector<Value> cells;
		for (auto &c : row.cells) {
			cells.push_back(Value(c));
		}
		output.SetValue(4, count, Value::LIST(LogicalType::VARCHAR, std::move(cells)));
		st.idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Scalar conversion utilities — whole-document transforms via the CLI engine
//===--------------------------------------------------------------------===//
static void PdfToTextFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t path) {
		try {
			return StringVector::AddString(result, pdfcli::PdfToText(path.GetString(), "reading", 1, -1, ""));
		} catch (std::exception &e) {
			throw InvalidInputException("pdf_to_text: %s", e.what());
		}
	});
}
// pdf_to_text(path, layout) — layout is one of 'reading' | 'physical' | 'raw'
static void PdfToTextLayoutFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t path, string_t layout) {
		    try {
			    return StringVector::AddString(result,
			                                   pdfcli::PdfToText(path.GetString(), layout.GetString(), 1, -1, ""));
		    } catch (std::exception &e) {
			    throw InvalidInputException("pdf_to_text: %s", e.what());
		    }
	    });
}
static void PdfToHtmlFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t path) {
		try {
			return StringVector::AddString(result, pdfcli::PdfToHtml(path.GetString(), false, false, ""));
		} catch (std::exception &e) {
			throw InvalidInputException("pdf_to_html: %s", e.what());
		}
	});
}
static void PdfToXmlFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t path) {
		try {
			return StringVector::AddString(result, pdfcli::PdfToXml(path.GetString(), ""));
		} catch (std::exception &e) {
			throw InvalidInputException("pdf_to_xml: %s", e.what());
		}
	});
}
static void PdfToSvgFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t path, int32_t page) {
		    try {
			    return StringVector::AddString(result, pdfcli::PdfToSvg(path.GetString(), page, ""));
		    } catch (std::exception &e) {
			    throw InvalidInputException("pdf_to_svg: %s", e.what());
		    }
	    });
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void LoadInternal(ExtensionLoader &loader) {
	TableFunction read_pdf("read_pdf", {LogicalType::VARCHAR}, ReadPdfScan, ReadPdfBind, ReadPdfInitGlobal);
	AddCommonNamedParams(read_pdf);
	loader.RegisterFunction(read_pdf);

	TableFunction read_pdf_meta("read_pdf_meta", {LogicalType::VARCHAR}, ReadPdfMetaScan, ReadPdfMetaBind,
	                            ReadPdfMetaInit);
	read_pdf_meta.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(read_pdf_meta);

	TableFunction read_pdf_words("read_pdf_words", {LogicalType::VARCHAR}, ReadPdfWordsScan, ReadPdfWordsBind,
	                             ReadPdfWordsInit);
	AddCommonNamedParams(read_pdf_words);
	loader.RegisterFunction(read_pdf_words);

	TableFunction read_pdf_lines("read_pdf_lines", {LogicalType::VARCHAR}, ReadPdfLinesScan, ReadPdfLinesBind,
	                             ReadPdfLinesInit);
	AddCommonNamedParams(read_pdf_lines);
	loader.RegisterFunction(read_pdf_lines);

	TableFunction read_pdf_tables("read_pdf_tables", {LogicalType::VARCHAR}, ReadPdfTablesScan, ReadPdfTablesBind,
	                              ReadPdfTablesInit);
	AddCommonNamedParams(read_pdf_tables);
	loader.RegisterFunction(read_pdf_tables);

	ScalarFunctionSet pdf_to_text_set("pdf_to_text");
	pdf_to_text_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToTextFun));
	pdf_to_text_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToTextLayoutFun));
	loader.RegisterFunction(pdf_to_text_set);
	loader.RegisterFunction(ScalarFunction("pdf_to_html", {LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToHtmlFun));
	loader.RegisterFunction(ScalarFunction("pdf_to_xml", {LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToXmlFun));
	loader.RegisterFunction(
	    ScalarFunction("pdf_to_svg", {LogicalType::VARCHAR, LogicalType::INTEGER}, LogicalType::VARCHAR, PdfToSvgFun));
}

void PdfExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string PdfExtension::Name() {
	return "pdf";
}
std::string PdfExtension::Version() const {
#ifdef EXT_VERSION_PDF
	return EXT_VERSION_PDF;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(pdf, loader) {
	duckdb::LoadInternal(loader);
}
}
