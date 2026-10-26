#pragma once

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
    {"read_pdf", "files", "Read one row per PDF page, with text, display dimensions, labels and OCR provenance. Supports DuckDB filesystems and globs; auto OCR and auto layout are enabled by default.", "SELECT page, text FROM read_pdf('document.pdf');"},
    {"pdf_info", "files", "Inspect document metadata, dimensions, PDF/A identification, permissions and document features. Missing metadata is NULL; ignore_errors skips unopenable files. Only requested fields are inspected.", "SELECT * FROM pdf_info('document.pdf');"},
    {"pdf_outline", "files", "Read hierarchical outline entries and destination pages.", "SELECT * FROM pdf_outline('document.pdf');"},
    {"pdf_structure", "files", "Read tagged PDF structure elements and page associations.", "SELECT * FROM pdf_structure('document.pdf');"},
    {"pdf_attachments", "files", "Read embedded attachments, including their bytes.", "SELECT * FROM pdf_attachments('document.pdf');"},
    {"pdf_revisions", "files", "Enumerate incremental PDF revisions from startxref and Prev links.", "SELECT * FROM pdf_revisions('document.pdf');"},
    {"read_pdf_words", "files", "Read positioned words with font, column/line grouping, label and OCR provenance. Coordinates use the displayed page with a top-left origin.", "SELECT * FROM read_pdf_words('document.pdf');"},
    {"read_pdf_lines", "files", "Read lines in reading order. Auto layout uses word geometry; other layouts use Poppler text. Auto OCR is enabled by default.", "SELECT * FROM read_pdf_lines('document.pdf', layout := 'auto');"},
    {"read_pdf_elements", "files", "Read classified text elements, font metrics and bounding boxes. OCR is opt-in; auto_ocr defaults to false.", "SELECT * FROM read_pdf_elements('document.pdf');"},
    {"pdf_chunks", "files", "Pack classified elements into retrieval chunks. chunk_size and overlap are byte budgets; n_chars counts UTF-8 codepoints. OCR is opt-in.", "SELECT * FROM pdf_chunks('document.pdf', chunk_size := 1200, overlap := 150);"},
    {"read_pdf_tables", "files", "Reconstruct table cells using ruling lines or word spacing. force bypasses the whitespace-table confidence gate.", "SELECT * FROM read_pdf_tables('document.pdf');"},
    {"pdf_to_text", "input,layout", "Convert a PDF path or BLOB to text; optional layout is auto, reading, physical or raw. Path inputs use DuckDB filesystems.", "SELECT pdf_to_text('document.pdf');"},
    {"pdf_to_html", "input", "Convert PDF text and positioned words to HTML. Accepts a path or PDF BLOB.", "SELECT pdf_to_html('document.pdf');"},
    {"pdf_to_xml", "input", "Convert PDF positioned text to XML. Accepts a path or PDF BLOB.", "SELECT pdf_to_xml('document.pdf');"},
    {"pdf_to_markdown", "path", "Convert a PDF path to Markdown using font and table geometry. This converter does not perform OCR.", "SELECT pdf_to_markdown('document.pdf');"},
    {"pdf_to_svg", "input,page,dpi", "Render one PDF page as an SVG containing a raster PNG. Page numbers are one-based; dpi is 1 through 2400.", "SELECT pdf_to_svg('document.pdf', 1);"},
    {"pdf_to_png", "input,page,dpi", "Render one PDF page to a PNG BLOB. Accepts a path or PDF BLOB; page numbers are one-based.", "SELECT pdf_to_png('document.pdf', 1);"},
    {"poppler_version", "", "Return the runtime Poppler version.", "SELECT poppler_version();"},
    {"tesseract_ocr", "image_blob,language,psm,oem,tessdata_dir,preprocess,vars,config", "Recognize an encoded image BLOB with Tesseract. Optional positional arguments configure language, segmentation, engine, models, preprocessing, variables and config.", "SELECT tesseract_ocr(pdf_to_png('document.pdf', 1));"},
    {"ocr_image", "image_blob", "Recognize an encoded image BLOB and return text, confidence and format. format accepts text, hocr or tsv; named OCR settings are supported.", "SELECT * FROM ocr_image(pdf_to_png('document.pdf', 1), format := 'text');"},
    {"to_pdf", "input_path,output_path", "Convert a local Office document using LibreOffice. With no output_path, replace the input extension with .pdf. Requires external process access.", "SELECT to_pdf('document.docx', 'converted.pdf');"},
    {"write_pdf", "content,output_path", "Write text to a PDF using the in-process writer. With no output_path, create a temporary PDF and return its path.", "SELECT write_pdf('Hello', 'hello.pdf');"},
    {"pdf_merge", "inputs,output", "Merge local input PDFs in list order and atomically publish the output.", "SELECT pdf_merge(['first.pdf', 'second.pdf'], 'merged.pdf');"},
    {"pdf_rotate", "input,output,degrees,pages", "Rotate all or selected pages of a local PDF. Degrees must be a multiple of 90; optional pages uses qpdf page-range syntax.", "SELECT pdf_rotate('document.pdf', 'rotated.pdf', 90);"},
    {"pdf_split", "input,output_dir", "Split a local PDF into one file per page in an existing output directory.", "SELECT * FROM pdf_split('document.pdf', 'pages');"},
    {"pdf_split_blank", "input,output_dir", "Split a local PDF at near-white separator pages, omitting the separators.", "SELECT * FROM pdf_split_blank('document.pdf', 'parts');"},
    {"pdf_compress", "input,output", "Optimize a local PDF structure and streams without downsampling images.", "SELECT pdf_compress('document.pdf', 'compressed.pdf');"},
    {"pdf_encrypt", "input,output,user_password,owner_password", "Encrypt a local PDF using AES-256; optional owner_password defaults to user_password.", "SELECT pdf_encrypt('document.pdf', 'encrypted.pdf', 'password');"},
    {"pdf_decrypt", "input,output,password", "Decrypt a local PDF with the supplied password.", "SELECT pdf_decrypt('encrypted.pdf', 'decrypted.pdf', 'password');"},
    {"pdf_pages", "input,output,ranges", "Extract or reorder pages from a local PDF using qpdf page-range syntax.", "SELECT pdf_pages('document.pdf', 'selected.pdf', '1-3');"},
    {"pdf_watermark", "input,output,text,opacity", "Stamp text on every page of a local PDF; opacity defaults to 0.30.", "SELECT pdf_watermark('document.pdf', 'marked.pdf', 'DRAFT');"},
    {"pdf_bates", "input,output,prefix,start_number", "Stamp sequential Bates identifiers on a local PDF.", "SELECT pdf_bates('document.pdf', 'numbered.pdf', 'DOC-', 1);"},
    {"pdf_form_fields", "files", "Read AcroForm field names, types, values and flags.", "SELECT * FROM pdf_form_fields('document.pdf');"},
    {"pdf_annotations", "files", "Read PDF annotations and their text and geometry.", "SELECT * FROM pdf_annotations('document.pdf');"},
    {"pdf_signatures", "files", "Inspect PDF signatures. verified checks CMS integrity, not certificate trust; covers_whole_file is a separate coverage flag.", "SELECT * FROM pdf_signatures('document.pdf');"},
    {"pdf_images", "files", "Extract embedded image XObjects as encoded bytes, with format, pixel dimensions, bit depth and colorspace.", "SELECT * FROM pdf_images('document.pdf');"},
    {"pdf_sign", "input,output,certificate,key", "Sign a local PDF using a PEM certificate and key. Existing signed documents are rejected because incremental signing is unsupported.", "SELECT * FROM pdf_sign('document.pdf', 'signed.pdf', 'cert.pem', 'key.pem');"},
    {"pdf_redact", "input,output,redactions", "Rasterize redacted pages and permanently remove their underlying text. Boxes use original PDF user-space points with a bottom-left origin, unlike reader word coordinates; output drops forms, annotations, attachments and document metadata when any page is redacted.", "SELECT * FROM pdf_redact('document.pdf', 'redacted.pdf', [{page: 1, x: 0.0, y: 0.0, w: 100.0, h: 30.0}]);"},
    {"pdf_redact_lateral", "input,output,redactions", "Column-input variant of pdf_redact. Uses fixed dpi 200 and an empty password; no named options.", "SELECT * FROM pdf_redact_lateral('document.pdf', 'redacted.pdf', [{page: 1, x: 0.0, y: 0.0, w: 100.0, h: 30.0}]);"},
    {"pdf_ocr", "input,output", "Add searchable Tesseract text layers to selected pages. Pages with text are skipped unless force is true; forced OCR adds a layer alongside existing text and can duplicate extracted text. Returns one status row per document page.", "SELECT * FROM pdf_ocr('document.pdf', 'searchable.pdf', force := false);"},
    {"pdf_pages_info", "files", "Inspect page boxes, rotation, dimensions and labels.", "SELECT * FROM pdf_pages_info('document.pdf');"},
    {"pdf_fonts", "files", "Inspect PDF font names, types and embedding information.", "SELECT * FROM pdf_fonts('document.pdf');"},
    {"pdf_destinations", "files", "Read named PDF destinations and their target geometry.", "SELECT * FROM pdf_destinations('document.pdf');"},
    {"pdf_page_images", "files", "Render pages to PNG BLOBs with dimensions and page metadata; dpi defaults to 150. Render only requested pages and stream one page at a time.", "SELECT page, png FROM pdf_page_images('document.pdf', dpi := 150);"},
    {"pdf_write_page_images", "files,output_dir", "Write PNG page previews under output_dir/stem/pN.png. Accepts a path/glob or list of paths and creates missing output directories.", "SELECT * FROM pdf_write_page_images('document.pdf', 'previews', dpi := 150);"},
    {"pdf_qpdf_info", "files", "Inspect qpdf structural, encryption and stream properties.", "SELECT * FROM pdf_qpdf_info('document.pdf');"},
    {"pdf_json", "path,password", "Return qpdf JSON for a PDF read through DuckDB filesystems; optional password opens encrypted inputs.", "SELECT pdf_json('document.pdf');"},
    {"pdf_repair", "input,output,password", "Rewrite a local PDF using qpdf recovery; optional password opens encrypted inputs.", "SELECT pdf_repair('document.pdf', 'repaired.pdf');"},
};

template <class FUNCTION, class INFO>
static void AddPdfFunctionDocs(FUNCTION &function, INFO &info) {
	for (const auto &doc : PDF_FUNCTION_DOCS) {
		if (function.name != Identifier(doc.name)) continue;
		auto names = StringUtil::Split(doc.parameters, ',');
		FunctionDescription description;
		auto &signature = function.GetSignature();
		for (idx_t i = 0; i < signature.GetParameterCount(); ++i) {
			auto &parameter = signature.GetParameter(i);
			if (!parameter.AcceptsPosition()) continue;
			if (i >= names.size()) throw InternalException("Missing PDF parameter documentation for %s", doc.name);
			parameter.SetName(Identifier(names[i]));
			description.parameter_names.push_back(names[i]);
			description.parameter_types.push_back(parameter.GetType());
		}
		description.description = doc.description;
		description.examples = {doc.example};
		info.descriptions.push_back(std::move(description));
		return;
	}
	throw InternalException("Missing PDF function documentation for %s", function.name);
}

} // namespace duckdb
