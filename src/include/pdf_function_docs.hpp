#pragma once

#include <type_traits>
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {

struct PdfFunctionDoc {
	const char *name;
	const char *parameters;
	const char *description;
	const char *example;
};

// One public API inventory, shared by scalar/table overload registration.
// Names describe positional arguments; typed kwargs remain on the signature.
static const PdfFunctionDoc PDF_FUNCTION_DOCS[] = {
    {"read_pdf_elements", "files",
     "Deprecated compatibility alias: use read_pdf(files, grain := 'element'). Preserves element schema, page bounds, "
     "password and OCR options. Retained through 0.9.x; removal planned no earlier than 1.0 after downstream "
     "migration.",
     "SELECT * FROM read_pdf('document.pdf', grain := 'element');"},
    {"pdf_to_text", "input,layout",
     "Deprecated compatibility alias: use pdf_text(input) or pdf_text(input, {layout: 'physical'}). Accepts a path or "
     "BLOB and the legacy optional layout string. Retained through 0.9.x; removal planned no earlier than 1.0 after "
     "downstream migration.",
     "SELECT pdf_text('document.pdf', {layout: 'physical'});"},
    {"read_pdf", "files",
     "Read PDF pages by default, or select grain page, line, element, table or chunk at bind time. Each grain retains "
     "its native typed columns and streams through DuckDB file readers. Unsupported options for the selected grain are "
     "rejected.",
     "SELECT page, text FROM read_pdf('document.pdf', grain := 'page');"},
    {"read_pdf_words", "files",
     "Read positioned words with font, column/line grouping, label and OCR provenance. Coordinates use displayed page "
     "points with a top-left origin.",
     "SELECT * FROM read_pdf_words('document.pdf');"},
    {"pdf_info", "files",
     "Inspect metadata, permissions and features (detail basic), or qpdf structural and encryption statistics (detail "
     "qpdf). Basic supports projection pushdown and ignore_errors.",
     "SELECT * FROM pdf_info('document.pdf', detail := 'basic');"},
    {"pdf_objects", "files",
     "Inspect typed PDF objects; kind is required: outline, structure, destinations, fonts, form_fields, annotations, "
     "attachments, images, signatures, revisions or pages. Each kind has its own schema. Signature verification checks "
     "integrity, not certificate trust.",
     "SELECT * FROM pdf_objects('document.pdf', kind := 'outline');"},
    {"pdf_text", "input,options",
     "Convert a path or PDF BLOB to VARCHAR. Optional STRUCT: format text (default), markdown, html, xml or json; text "
     "layout defaults to reading. Password is supported except for Markdown. Markdown requires a path.",
     "SELECT pdf_text('document.pdf', {format: 'markdown'});"},
    {"pdf_render", "input,page,options",
     "Scalar: render one page to encoded PNG or SVG bytes (BLOB); optional STRUCT format defaults to png, dpi to 150. "
     "Use decode for SVG text. Table: stream pages, with format png (default) or svg; output_dir writes preview files. "
     "Page bounds, dpi and password are named table options.",
     "SELECT pdf_render('document.pdf', 1, {format: 'png', dpi: 150});"},
    {"pdf_merge", "inputs,output", "Merge local input PDFs in list order and atomically publish the output.",
     "SELECT pdf_merge(['first.pdf', 'second.pdf'], 'merged.pdf');"},
    {"pdf_pages", "input,output,ranges,options",
     "Extract/reorder local PDF pages using qpdf ranges; optional STRUCT rotate (multiple of 90) and rotate_pages "
     "select rotation after extraction, in output page coordinates. Default rotates all selected pages.",
     "SELECT pdf_pages('document.pdf', 'selected.pdf', '1-3', {rotate: 90});"},
    {"pdf_split", "input,output_dir",
     "Split a local PDF into files by page (default mode page), or near-white separators (mode blank, blank_threshold "
     "in (0,1]). Schemas differ by mode. The output directory must exist.",
     "SELECT * FROM pdf_split('document.pdf', 'parts', mode := 'blank');"},
    {"pdf_stamp", "input,output,options",
     "Stamp a local PDF. STRUCT mode watermark requires text, optional opacity defaults to 0.30; mode bates requires "
     "prefix and start. Output is atomically published.",
     "SELECT pdf_stamp('document.pdf', 'marked.pdf', {mode: 'watermark', text: 'DRAFT'});"},
    {"pdf_rewrite", "input,output,options",
     "Rewrite a local PDF. STRUCT mode is compress, repair, encrypt or decrypt. Encrypt/decrypt require password; "
     "encrypt optionally accepts owner_password. Repair optionally accepts password. Output is atomically published.",
     "SELECT pdf_rewrite('document.pdf', 'small.pdf', {mode: 'compress'});"},
    {"pdf_redact", "input,output,redactions",
     "Rasterize redacted pages and remove their underlying text. Boxes use original PDF user-space points with a "
     "bottom-left origin. Redaction removes forms, annotations, attachments and document metadata.",
     "SELECT * FROM pdf_redact('document.pdf', 'redacted.pdf', [{page: 1, x: 0.0, y: 0.0, w: 100.0, h: 30.0}]);"},
    {"pdf_redact_lateral", "input,output,redactions",
     "Column-input variant of pdf_redact. Uses dpi 200 and an empty password; no named options.",
     "SELECT * FROM pdf_redact_lateral('document.pdf', 'redacted.pdf', [{page: 1, x: 0.0, y: 0.0, w: 100.0, h: "
     "30.0}]);"},
    {"pdf_sign", "input,output,certificate,key",
     "Sign using a PEM certificate and key. Existing signatures are rejected: incremental signing is unsupported. "
     "Inspect with pdf_objects kind signatures.",
     "SELECT * FROM pdf_sign('document.pdf', 'signed.pdf', 'cert.pem', 'key.pem');"},
    {"write_pdf", "content,output_path",
     "Write text to PDF using the in-process writer. Omit output_path to create a temporary PDF and return its path.",
     "SELECT write_pdf('Hello', 'hello.pdf');"},
    {"to_pdf", "input_path,output_path",
     "Convert a local Office document. macOS tries the matching Microsoft app, then its Apple equivalent, then "
     "LibreOffice; other platforms use LibreOffice. Omit output_path to replace the input extension with pdf. "
     "Requires external process access.",
     "SELECT to_pdf('document.docx', 'converted.pdf');"},
    {"ocr_image", "image_blob,options",
     "Recognize an encoded image BLOB. Scalar returns text, HOCR or TSV and accepts an optional options STRUCT. Table "
     "returns text, confidence and format with named OCR settings.",
     "SELECT ocr_image(pdf_render('document.pdf', 1), {language: 'eng'});"},
    {"pdf_ocr", "input,output",
     "Add searchable Tesseract layers to selected pages and return one status row per document page. Existing text "
     "pages are skipped unless force is true. Force adds another layer and can duplicate extracted text.",
     "SELECT * FROM pdf_ocr('document.pdf', 'searchable.pdf', force := false);"},
    {"poppler_version", "", "Return the linked runtime Poppler version.", "SELECT poppler_version();"},
};

template <class FUNCTION, class INFO>
static void AddPdfFunctionDocs(FUNCTION &function, INFO &info) {
	for (const auto &doc : PDF_FUNCTION_DOCS) {
		if (function.name != Identifier(doc.name))
			continue;
		auto parameters = string(doc.parameters);
		if (function.name == Identifier("pdf_render") && std::is_same<FUNCTION, TableFunction>::value)
			parameters = "files";
		if (function.name == Identifier("pdf_pages") && function.GetSignature().GetParameterCount() == 3 &&
		    function.GetSignature().GetParameter(2).GetType() == LogicalType::ANY)
			parameters = "input,output,options";
		auto names = StringUtil::Split(parameters, ',');
		FunctionDescription description;
		auto &signature = function.GetSignature();
		for (idx_t i = 0; i < signature.GetParameterCount(); ++i) {
			auto &parameter = signature.GetParameter(i);
			if (!parameter.AcceptsPosition())
				continue;
			if (i >= names.size())
				throw InternalException("Missing PDF parameter documentation for %s", doc.name);
			parameter.SetName(Identifier(names[i]));
			description.parameter_names.push_back(names[i]);
			description.parameter_types.push_back(parameter.GetType());
		}
		description.description = doc.description;
		description.examples = {doc.example};
		if (function.name == Identifier("pdf_pages") && parameters == "input,output,options") {
			description.description =
			    "Rotate a complete local PDF without extracting pages, preserving its catalog, "
			    "metadata and bookmarks. STRUCT rotate is a multiple of 90; optional rotate_pages "
			    "defaults to all and uses original document page numbers.";
			description.examples = {"SELECT pdf_pages('document.pdf', 'rotated.pdf', {rotate: 90});"};
		}
		if (std::is_same<FUNCTION, TableFunction>::value) {
			if (function.name == Identifier("pdf_render"))
				description.examples = {"SELECT page, png FROM pdf_render('document.pdf', dpi := 150);"};
			if (function.name == Identifier("ocr_image"))
				description.examples = {"SELECT text, confidence FROM ocr_image(pdf_render('document.pdf', 1));"};
		}
		info.descriptions.push_back(std::move(description));
		return;
	}
	throw InternalException("Missing PDF function documentation for %s", function.name);
}

} // namespace duckdb
