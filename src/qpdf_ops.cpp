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
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QUtil.hh>

#include <cmath>
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

namespace {

// Lightweight content-stream path interpreter: collects axis-aligned stroke
// segments (and rectangle strokes/fills used as borders). Tracks CTM for `cm`.
// poppler-cpp has no path API; qpdf's page content parser supplies the tokens.
struct RulingPathCollector : public QPDFObjectHandle::ParserCallbacks {
	struct Pt {
		double x = 0, y = 0;
	};
	// CTM as [a b c d e f] mapping user space -> device (page) space.
	double ctm[6] = {1, 0, 0, 1, 0, 0};
	std::vector<double> stack; // operand stack (numeric only; others ignored)
	Pt cur;
	Pt path_start;
	bool has_cur = false;
	std::vector<std::pair<Pt, Pt>> segments; // stroke segments in page space

	void ApplyCtm(double x, double y, double &ox, double &oy) const {
		ox = ctm[0] * x + ctm[2] * y + ctm[4];
		oy = ctm[1] * x + ctm[3] * y + ctm[5];
	}

	void MultiplyCtm(double a, double b, double c, double d, double e, double f) {
		// new = T * current  (PDF: cm concatenates T onto CTM)
		double na = a * ctm[0] + b * ctm[2];
		double nb = a * ctm[1] + b * ctm[3];
		double nc = c * ctm[0] + d * ctm[2];
		double nd = c * ctm[1] + d * ctm[3];
		double ne = e * ctm[0] + f * ctm[2] + ctm[4];
		double nf = e * ctm[1] + f * ctm[3] + ctm[5];
		ctm[0] = na;
		ctm[1] = nb;
		ctm[2] = nc;
		ctm[3] = nd;
		ctm[4] = ne;
		ctm[5] = nf;
	}

	void AddSeg(Pt a, Pt b) {
		// Keep near-axis-aligned segments only (table rules).
		double dx = std::fabs(a.x - b.x);
		double dy = std::fabs(a.y - b.y);
		if (dx < 0.5 && dy < 0.5) {
			return; // point
		}
		// allow ~3° skew: |dx| or |dy| dominates
		if (dx < 1.0 || dy < 1.0 || dx > 20 * dy || dy > 20 * dx) {
			segments.emplace_back(a, b);
		}
	}

	void AddRect(double x, double y, double w, double h) {
		Pt p0, p1, p2, p3;
		ApplyCtm(x, y, p0.x, p0.y);
		ApplyCtm(x + w, y, p1.x, p1.y);
		ApplyCtm(x + w, y + h, p2.x, p2.y);
		ApplyCtm(x, y + h, p3.x, p3.y);
		AddSeg(p0, p1);
		AddSeg(p1, p2);
		AddSeg(p2, p3);
		AddSeg(p3, p0);
	}

	void handleObject(QPDFObjectHandle obj, size_t, size_t) override {
		if (obj.isInteger() || obj.isReal()) {
			stack.push_back(obj.getNumericValue());
			return;
		}
		if (!obj.isOperator()) {
			// names, strings, arrays used by non-path ops — leave stack alone for
			// mixed operands; path ops only consume numbers we push above.
			return;
		}
		std::string op = obj.getOperatorValue();
		auto need = [&](size_t n) -> bool {
			return stack.size() >= n;
		};
		auto popn = [&](size_t n) {
			std::vector<double> out(n);
			for (size_t i = 0; i < n; ++i) {
				out[n - 1 - i] = stack.back();
				stack.pop_back();
			}
			return out;
		};

		if (op == "cm" && need(6)) {
			auto v = popn(6);
			MultiplyCtm(v[0], v[1], v[2], v[3], v[4], v[5]);
		} else if (op == "m" && need(2)) {
			auto v = popn(2);
			ApplyCtm(v[0], v[1], cur.x, cur.y);
			path_start = cur;
			has_cur = true;
		} else if (op == "l" && need(2)) {
			auto v = popn(2);
			Pt nxt;
			ApplyCtm(v[0], v[1], nxt.x, nxt.y);
			if (has_cur) {
				AddSeg(cur, nxt);
			}
			cur = nxt;
			has_cur = true;
		} else if (op == "re" && need(4)) {
			auto v = popn(4);
			AddRect(v[0], v[1], v[2], v[3]);
			// PDF: after re, current point is undefined for further path construction
			has_cur = false;
		} else if (op == "h") {
			if (has_cur) {
				AddSeg(cur, path_start);
				cur = path_start;
			}
		} else if (op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" || op == "B" || op == "B*" ||
		           op == "b" || op == "b*") {
			// stroke / fill ends the path; segments already recorded on construction
			if (op == "s" || op == "b" || op == "b*") {
				if (has_cur) {
					AddSeg(cur, path_start);
				}
			}
			has_cur = false;
			// leave stack as-is (these take no operands)
		} else if (op == "n" || op == "W" || op == "W*") {
			has_cur = false;
		} else if (op == "q" || op == "Q") {
			// ignore graphics-state stack for rules (CTM save/restore not modelled fully);
			// most table drawers set CTM once or use identity.
			stack.clear();
		} else {
			// unknown operator: drop its would-be numeric leftovers by clearing
			// when the operator is a common text/state op that consumes the stack.
			// Heuristic: clear stack after any non-path operator to avoid pollution.
			if (op == "Tj" || op == "TJ" || op == "'" || op == "\"" || op == "Td" || op == "TD" || op == "Tm" ||
			    op == "T*" || op == "Tc" || op == "Tw" || op == "Tz" || op == "TL" || op == "Tf" || op == "Tr" ||
			    op == "Ts" || op == "rg" || op == "RG" || op == "g" || op == "G" || op == "k" || op == "K" ||
			    op == "w" || op == "J" || op == "j" || op == "M" || op == "d" || op == "gs" || op == "cs" ||
			    op == "CS" || op == "scn" || op == "SCN" || op == "sc" || op == "SC" || op == "Do" || op == "BT" ||
			    op == "ET" || op == "BMC" || op == "BDC" || op == "EMC" || op == "MP" || op == "DP") {
				stack.clear();
			}
		}
	}

	void handleEOF() override {
	}
};

} // namespace

// Collect axis-aligned ruling segments from every page's content stream. Uses a
// fresh interpreter per page and reads each page's MediaBox height so the caller
// can flip horizontal rules into poppler's top-down text_list space. Parsing is
// best-effort per page: a page whose content stream fails to parse contributes
// no segments rather than aborting the document. processMemoryFile failures
// (bad password / corrupt input) propagate per the error contract.
std::vector<RuledSegment> ExtractRulingLines(const std::string &pdf_bytes, const std::string &password) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	std::vector<RuledSegment> out;
	QPDF qpdf;
	qpdf.processMemoryFile("read_pdf_tables", pdf_bytes.data(), pdf_bytes.size(),
	                       password.empty() ? nullptr : password.c_str());
	auto pages = QPDFPageDocumentHelper(qpdf).getAllPages();
	for (size_t pi = 0; pi < pages.size(); ++pi) {
		auto &page = pages[pi];
		// MediaBox [llx lly urx ury] — page height for the caller's y flip.
		double page_h = 792.0;
		try {
			auto mb = page.getObjectHandle().getKey("/MediaBox");
			if (mb.isArray() && mb.getArrayNItems() >= 4) {
				double lly = mb.getArrayItem(1).getNumericValue();
				double ury = mb.getArrayItem(3).getNumericValue();
				if (ury > lly) {
					page_h = ury - lly;
				}
			}
		} catch (...) {
		}

		RulingPathCollector collector;
		try {
			page.parseContents(&collector);
		} catch (...) {
			continue; // best-effort: skip pages whose content stream won't parse
		}
		for (auto &seg : collector.segments) {
			RuledSegment rs;
			rs.page = static_cast<int>(pi + 1);
			rs.x0 = seg.first.x;
			rs.y0 = seg.first.y;
			rs.x1 = seg.second.x;
			rs.y1 = seg.second.y;
			rs.page_height = page_h;
			out.push_back(rs);
		}
	}
	return out;
}

} // namespace pdf_qpdf
