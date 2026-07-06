//===--------------------------------------------------------------------===//
// qpdf_ops.cpp — the ONLY C++17 translation unit in this extension.
//
// See qpdf_ops.hpp for why: qpdf headers require C++17, libduckdb_static may
// be built at C++11, and mixing standards across a duckdb header is an ODR
// violation. This file therefore includes qpdf and the standard library ONLY —
// absolutely no duckdb headers, no poppler. CMakeLists.txt pins -std=c++17
// (/std:c++17 on MSVC) on this one source file.
//
// Locking: qpdf documents its C++ API as safe for concurrent use on distinct
// QPDF objects, but we stay conservative and consistent with this codebase —
// every whole operation runs under one dedicated global lock. This is
// deliberately NOT PopplerMutex: qpdf and poppler share no state, so the two
// libraries may run concurrently with each other, just not with themselves.
//===--------------------------------------------------------------------===//
#include "qpdf_ops.hpp"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFAnnotationObjectHelper.hh>
#include <qpdf/QPDFFormFieldObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QUtil.hh>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pdf_qpdf {

static std::recursive_mutex &QpdfMutex() {
	static std::recursive_mutex m;
	return m;
}

void Merge(const std::vector<std::string> &inputs, const std::string &output) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	QPDF merged;
	merged.emptyPDF();
	QPDFPageDocumentHelper merged_pages(merged);
	// Sources must outlive the write: addPage on a foreign page copies
	// lazily; objects are pulled from the source during QPDFWriter::write.
	std::vector<std::unique_ptr<QPDF>> sources;
	for (auto &in_path : inputs) {
		auto source = std::make_unique<QPDF>();
		source->processFile(in_path.c_str());
		QPDFPageDocumentHelper source_pages(*source);
		for (auto &page : source_pages.getAllPages()) {
			merged_pages.addPage(page, false);
		}
		sources.push_back(std::move(source));
	}
	QPDFWriter writer(merged, output.c_str());
	writer.write();
}

void Rotate(const std::string &input, const std::string &output, int degrees, bool all_pages,
            const std::string &pages_spec) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	QPDF doc;
	doc.processFile(input.c_str());
	QPDFPageDocumentHelper doc_pages(doc);
	auto pages = doc_pages.getAllPages();
	auto page_count = static_cast<int>(pages.size());
	std::vector<int> selected;
	if (all_pages) {
		for (int page_no = 1; page_no <= page_count; page_no++) {
			selected.push_back(page_no);
		}
	} else {
		// qpdf's own CLI range grammar; throws with a descriptive message
		// on malformed / out-of-range specs.
		selected = QUtil::parse_numrange(pages_spec.c_str(), page_count);
	}
	for (int page_no : selected) {
		pages[page_no - 1].rotatePage(degrees, true /* relative to existing rotation */);
	}
	QPDFWriter writer(doc, output.c_str());
	writer.write();
}

void Split(const std::string &input, const std::string &output_dir, const std::string &stem,
           std::vector<std::pair<int, std::string>> &emitted) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	QPDF doc;
	doc.processFile(input.c_str());
	QPDFPageDocumentHelper doc_pages(doc);
	auto pages = doc_pages.getAllPages();
	auto page_count = static_cast<int>(pages.size());
	auto pad_width = std::to_string(page_count).size();
	for (int page_idx = 0; page_idx < page_count; page_idx++) {
		std::string page_no = std::to_string(page_idx + 1);
		while (page_no.size() < pad_width) {
			page_no = "0" + page_no;
		}
		std::string out_path = output_dir + "/" + stem + "_p" + page_no + ".pdf";
		QPDF single;
		single.emptyPDF();
		QPDFPageDocumentHelper single_pages(single);
		single_pages.addPage(pages[page_idx], false);
		QPDFWriter writer(single, out_path.c_str());
		writer.write();
		emitted.emplace_back(page_idx + 1, out_path);
	}
}

void Compress(const std::string &input, const std::string &output) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	QPDF doc;
	doc.processFile(input.c_str());
	QPDFWriter writer(doc, output.c_str());
	writer.setObjectStreamMode(qpdf_o_generate);
	writer.setCompressStreams(true);
	writer.setRecompressFlate(true);
	writer.setLinearization(true);
	writer.write();
}

void Encrypt(const std::string &input, const std::string &output, const std::string &user_password,
             const std::string &owner_password) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	QPDF doc;
	doc.processFile(input.c_str());
	QPDFWriter writer(doc, output.c_str());
	writer.setR6EncryptionParameters(user_password.c_str(), owner_password.c_str(), true /* accessibility */,
	                                 true /* extract */, true /* assemble */, true /* annotate_and_form */,
	                                 true /* form_filling */, true /* modify_other */, qpdf_r3p_full,
	                                 true /* encrypt metadata */);
	writer.write();
}

void Decrypt(const std::string &input, const std::string &output, const std::string &password) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	QPDF doc;
	doc.processFile(input.c_str(), password.c_str());
	QPDFWriter writer(doc, output.c_str());
	writer.setPreserveEncryption(false);
	writer.write();
}

void Pages(const std::string &input, const std::string &output, const std::string &ranges) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	QPDF source;
	source.processFile(input.c_str());
	QPDFPageDocumentHelper source_pages(source);
	auto pages = source_pages.getAllPages();
	auto page_count = static_cast<int>(pages.size());
	// qpdf's own CLI range grammar; throws with a descriptive message on
	// malformed / out-of-range specs.
	auto selected = QUtil::parse_numrange(ranges.c_str(), page_count);
	QPDF subset;
	subset.emptyPDF();
	QPDFPageDocumentHelper subset_pages(subset);
	// copyForeignObject caches per source object, so adding the SAME page
	// twice would insert one object twice (an error); shallow-copy repeats.
	std::set<QPDFObjGen> seen;
	for (int page_no : selected) {
		auto &page = pages[page_no - 1];
		auto og = page.getObjectHandle().getObjGen();
		if (seen.count(og)) {
			subset_pages.addPage(page.shallowCopyPage(), false);
		} else {
			subset_pages.addPage(page, false);
			seen.insert(og);
		}
	}
	// source must outlive the write: foreign pages are pulled lazily.
	QPDFWriter writer(subset, output.c_str());
	writer.write();
}

std::vector<FormField> ReadFormFields(const std::string &path) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	std::vector<FormField> fields;
	QPDF doc;
	doc.processFile(path.c_str());
	QPDFAcroFormDocumentHelper acroform(doc);
	if (!acroform.hasAcroForm()) {
		return fields;
	}
	// Map field object -> 1-based page number via each page's widget
	// annotations (a field with no widget keeps page unset).
	std::map<QPDFObjGen, int> field_page;
	QPDFPageDocumentHelper doc_pages(doc);
	auto pages = doc_pages.getAllPages();
	for (size_t page_idx = 0; page_idx < pages.size(); page_idx++) {
		for (auto &field : acroform.getFormFieldsForPage(pages[page_idx])) {
			auto og = field.getObjectHandle().getObjGen();
			if (!field_page.count(og)) {
				field_page[og] = static_cast<int>(page_idx + 1);
			}
		}
	}
	for (auto &field : acroform.getFormFields()) {
		FormField out;
		auto og = field.getObjectHandle().getObjGen();
		auto page_it = field_page.find(og);
		if (page_it != field_page.end()) {
			out.has_page = true;
			out.page = page_it->second;
		}
		out.name = field.getFullyQualifiedName();
		out.type = field.getFieldType();
		auto value_handle = field.getValue();
		if (!value_handle.isNull()) {
			out.has_value = true;
			out.value = field.getValueAsString();
		}
		out.is_required = (field.getFlags() & ff_all_required) != 0;
		fields.push_back(std::move(out));
	}
	return fields;
}

std::vector<Annotation> ReadAnnotations(const std::string &path) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	std::vector<Annotation> annotations;
	QPDF doc;
	doc.processFile(path.c_str());
	QPDFPageDocumentHelper doc_pages(doc);
	auto pages = doc_pages.getAllPages();
	for (size_t page_idx = 0; page_idx < pages.size(); page_idx++) {
		for (auto &annot : pages[page_idx].getAnnotations()) {
			Annotation out;
			out.page = static_cast<int>(page_idx + 1);
			out.subtype = annot.getSubtype();
			auto dict = annot.getObjectHandle();
			auto contents = dict.getKey("/Contents");
			if (contents.isString()) {
				out.has_contents = true;
				out.contents = contents.getUTF8Value();
			}
			auto action = dict.getKey("/A");
			if (action.isDictionary() && action.getKey("/S").isNameAndEquals("/URI") &&
			    action.getKey("/URI").isString()) {
				out.has_uri = true;
				out.uri = action.getKey("/URI").getUTF8Value();
			}
			auto rect = annot.getRect();
			out.rect_x0 = rect.llx;
			out.rect_y0 = rect.lly;
			out.rect_x1 = rect.urx;
			out.rect_y1 = rect.ury;
			annotations.push_back(std::move(out));
		}
	}
	return annotations;
}

} // namespace pdf_qpdf
