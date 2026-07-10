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
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QUtil.hh>

// Pipelines for deflating the redaction raster into a FlateDecode image XObject.
#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_Flate.hh>

// OpenSSL — CMS/PKCS#7 verification for pdf_signatures (tier 2) and CMS_sign for
// pdf_sign. Plain C API; touches neither duckdb nor poppler, so it is safe to
// include in this TU.
#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <ctime>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
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

//===--------------------------------------------------------------------===//
// pdf_signatures — detect + cryptographically verify existing PDF signatures.
//
// One SignatureInfo per filled AcroForm /Sig field. Tier 1: parse the signature
// dictionary (/SubFilter, /M, /Name, /Reason, /Location, /ByteRange) and compute
// covers_whole_file (ByteRange starts at 0 and ends at EOF — the cheap "nothing
// was appended after signing" tamper signal). Tier 2: rebuild the signed message
// from the ByteRange spans and CMS_verify the /Contents PKCS#7/CMS blob with
// OpenSSL. Signing (creation) is out of scope; detection + verification only.
//===--------------------------------------------------------------------===//

// Reads the whole file into memory for ByteRange coverage + CMS message rebuild.
// qpdf's processFile reads the local filesystem, so pdf_signatures is already
// local-path-only; a plain std::ifstream matches that constraint.
static std::string ReadFileBytes(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		throw std::runtime_error("could not read '" + path + "'");
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// Read a PDF-string value from a dict key as UTF-8; empty string means absent
// (the caller maps empty to SQL NULL).
static std::string DictStringOrEmpty(QPDFObjectHandle dict, const char *key) {
	if (!dict.isDictionary()) {
		return std::string();
	}
	auto v = dict.getKey(key);
	if (!v.isString()) {
		return std::string();
	}
	return v.getUTF8Value();
}

// Extract CN= from an X509 subject name; empty if none.
static std::string X509CommonName(X509 *cert) {
	if (!cert) {
		return std::string();
	}
	X509_NAME *name = X509_get_subject_name(cert);
	if (!name) {
		return std::string();
	}
	char buf[256];
	int n = X509_NAME_get_text_by_NID(name, NID_commonName, buf, sizeof(buf));
	if (n <= 0) {
		return std::string();
	}
	return std::string(buf, static_cast<size_t>(n));
}

// Tier 2: verify a detached (or attached) CMS/PKCS#7 signature over the
// ByteRange-covered bytes. Returns true only when CMS_verify succeeds.
static bool CmsVerify(const std::string &message, const std::string &cms_der) {
	if (cms_der.empty() || message.empty()) {
		return false;
	}
	const unsigned char *p = reinterpret_cast<const unsigned char *>(cms_der.data());
	CMS_ContentInfo *cms = d2i_CMS_ContentInfo(nullptr, &p, static_cast<long>(cms_der.size()));
	if (!cms) {
		ERR_clear_error();
		return false;
	}
	BIO *data_bio = BIO_new_mem_buf(message.data(), static_cast<int>(message.size()));
	if (!data_bio) {
		CMS_ContentInfo_free(cms);
		return false;
	}
	// CMS_NO_SIGNER_CERT_VERIFY: do not require a trusted CA chain — we only
	// check that the digest over ByteRange matches the SignedData (integrity).
	// CMS_BINARY: PDF bytes are raw, not canonicalized text.
	// CMS_DETACHED: eContent is absent; message is the ByteRange concatenation.
	unsigned flags = CMS_NO_SIGNER_CERT_VERIFY | CMS_BINARY | CMS_DETACHED;
	int rc = CMS_verify(cms, nullptr, nullptr, data_bio, nullptr, flags);
	if (rc != 1) {
		// Some writers embed content (non-detached). Retry without DETACHED.
		ERR_clear_error();
		BIO_reset(data_bio);
		flags = CMS_NO_SIGNER_CERT_VERIFY | CMS_BINARY;
		rc = CMS_verify(cms, nullptr, nullptr, data_bio, nullptr, flags);
	}
	BIO_free(data_bio);
	CMS_ContentInfo_free(cms);
	ERR_clear_error();
	return rc == 1;
}

// Best-effort CN from the CMS signer certificate (for signer_name fallback).
static std::string CmsSignerCN(const std::string &cms_der) {
	if (cms_der.empty()) {
		return std::string();
	}
	const unsigned char *p = reinterpret_cast<const unsigned char *>(cms_der.data());
	CMS_ContentInfo *cms = d2i_CMS_ContentInfo(nullptr, &p, static_cast<long>(cms_der.size()));
	if (!cms) {
		ERR_clear_error();
		return std::string();
	}
	std::string cn;
	STACK_OF(X509) *signers = CMS_get0_signers(cms);
	if (signers && sk_X509_num(signers) > 0) {
		cn = X509CommonName(sk_X509_value(signers, 0));
	}
	// CMS_get0_signers may be empty until verify; fall back to embedded certs.
	if (cn.empty()) {
		STACK_OF(X509) *certs = CMS_get1_certs(cms);
		if (certs) {
			if (sk_X509_num(certs) > 0) {
				cn = X509CommonName(sk_X509_value(certs, 0));
			}
			sk_X509_pop_free(certs, X509_free);
		}
	}
	CMS_ContentInfo_free(cms);
	ERR_clear_error();
	return cn;
}

std::vector<SignatureInfo> ReadSignatures(const std::string &path, const std::string &password) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	std::vector<SignatureInfo> sigs;

	// File length + raw bytes for ByteRange coverage and CMS message rebuild.
	std::string file_bytes = ReadFileBytes(path);
	const int64_t file_size = static_cast<int64_t>(file_bytes.size());

	QPDF doc;
	if (password.empty()) {
		doc.processFile(path.c_str());
	} else {
		doc.processFile(path.c_str(), password.c_str());
	}
	QPDFAcroFormDocumentHelper acroform(doc);
	if (!acroform.hasAcroForm()) {
		return sigs;
	}
	for (auto &field : acroform.getFormFields()) {
		if (field.getFieldType() != "/Sig") {
			continue;
		}
		auto value = field.getValue();
		// Empty / unsigned signature field: skip (no signature dictionary).
		if (!value.isDictionary()) {
			continue;
		}
		SignatureInfo out;
		out.field_name = field.getFullyQualifiedName();

		// /SubFilter is a name (/adbe.pkcs7.detached); strip the leading slash.
		auto sf = value.getKey("/SubFilter");
		if (sf.isName()) {
			std::string n = sf.getName();
			out.subfilter = (!n.empty() && n[0] == '/') ? n.substr(1) : n;
		} else if (sf.isString()) {
			out.subfilter = sf.getUTF8Value();
		}

		out.signing_time_raw = DictStringOrEmpty(value, "/M");
		// Prefer dictionary /Name; fall back to CMS cert CN after parsing Contents.
		out.signer_name = DictStringOrEmpty(value, "/Name");
		out.reason = DictStringOrEmpty(value, "/Reason");
		out.location = DictStringOrEmpty(value, "/Location");

		// /ByteRange: array of integers [off0 len0 off1 len1 ...]
		std::vector<int64_t> byte_range;
		auto br = value.getKey("/ByteRange");
		if (br.isArray()) {
			int n = br.getArrayNItems();
			for (int i = 0; i < n; i++) {
				auto item = br.getArrayItem(i);
				if (item.isInteger()) {
					byte_range.push_back(item.getIntValue());
				}
			}
		}

		std::string message; // concatenation of ByteRange spans
		if (byte_range.size() >= 2 && byte_range.size() % 2 == 0) {
			out.covers_whole_file = (byte_range[0] == 0);
			int64_t last_off = byte_range[byte_range.size() - 2];
			int64_t last_len = byte_range[byte_range.size() - 1];
			if (last_off < 0 || last_len < 0 || last_off > file_size || last_len > file_size - last_off) {
				out.covers_whole_file = false;
			} else if (last_off + last_len != file_size) {
				out.covers_whole_file = false;
			}
			// Build the signed message; also validates each span.
			bool spans_ok = true;
			for (size_t i = 0; i + 1 < byte_range.size(); i += 2) {
				int64_t off = byte_range[i];
				int64_t len = byte_range[i + 1];
				if (off < 0 || len < 0 || off > file_size || len > file_size - off) {
					spans_ok = false;
					break;
				}
				message.append(file_bytes.data() + off, static_cast<size_t>(len));
			}
			if (!spans_ok) {
				out.covers_whole_file = false;
				message.clear();
			}
		}

		// /Contents: hex (or binary) string holding the CMS/PKCS#7 blob.
		std::string cms_der;
		auto contents = value.getKey("/Contents");
		if (contents.isString()) {
			// qpdf returns the decoded string bytes (hex already unhexed).
			cms_der = contents.getStringValue();
			// Strip trailing NUL padding writers leave in the fixed-width hex hole.
			while (!cms_der.empty() && cms_der.back() == '\0') {
				cms_der.pop_back();
			}
		}

		// Tier 2 cryptographic verification.
		if (!cms_der.empty() && !message.empty()) {
			out.has_verified = true;
			out.verified = CmsVerify(message, cms_der);
		} else if (!cms_der.empty() && message.empty()) {
			// Have a signature blob but an unusable ByteRange → not verified.
			out.has_verified = true;
			out.verified = false;
		}
		// else: no Contents → leave verified NULL (empty/incomplete sig dict).

		if (out.signer_name.empty() && !cms_der.empty()) {
			std::string cn = CmsSignerCN(cms_der);
			if (!cn.empty()) {
				out.signer_name = cn;
			}
		}

		sigs.push_back(std::move(out));
	}
	return sigs;
}

//===--------------------------------------------------------------------===//
// pdf_sign — create an adbe.pkcs7.detached CMS signature. See qpdf_ops.hpp for
// the contract. Strategy: qpdf writes the AcroForm /Sig field with a /ByteRange
// and a fixed-width /Contents hex placeholder (all-NUL bytes → qpdf serializes a
// hex string `<0000...>`) to a memory buffer, deterministically (static /ID, no
// object streams, streams preserved) so the placeholder appears verbatim. We
// then locate the placeholder, patch /ByteRange in place, CMS_sign the two spans,
// and hex-fill the DER into the /Contents hole.
//===--------------------------------------------------------------------===//

// Fixed /Contents hex budget: 8192 DER bytes → 16384 hex chars. A self-signed
// RSA-2048 detached CMS blob is ~1.5-2 KB, so this leaves generous headroom.
static const size_t kSignContentsBytes = 8192;

// pem_password_cb: hands the (possibly empty) key password to OpenSSL's PEM
// reader. An empty password on an encrypted key surfaces as a decrypt failure,
// which the caller reports as a wrong-password error.
static int SignPemPasswordCb(char *buf, int size, int /*rwflag*/, void *user) {
	const std::string *pw = static_cast<const std::string *>(user);
	if (!pw) {
		return 0;
	}
	int len = static_cast<int>(pw->size());
	if (len > size) {
		len = size;
	}
	if (len > 0) {
		std::memcpy(buf, pw->data(), static_cast<size_t>(len));
	}
	return len;
}

static std::string ReadWholeFile(const std::string &path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		throw std::runtime_error("could not read '" + path + "'");
	}
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

// CMS_sign the message with a PEM cert + PEM (optionally encrypted) key. Returns
// the detached SignedData as DER bytes. Throws on any load / sign failure.
static std::string CmsSignDetached(const std::string &message, const std::string &cert_pem, const std::string &key_pem,
                                   const std::string &key_password) {
	std::unique_ptr<BIO, decltype(&BIO_free)> cert_bio(
	    BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size())), BIO_free);
	if (!cert_bio) {
		throw std::runtime_error("out of memory reading certificate");
	}
	std::unique_ptr<X509, decltype(&X509_free)> cert(PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr),
	                                                 X509_free);
	if (!cert) {
		ERR_clear_error();
		throw std::runtime_error("could not parse certificate PEM (expected an X.509 certificate)");
	}

	std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size())),
	                                                  BIO_free);
	if (!key_bio) {
		throw std::runtime_error("out of memory reading private key");
	}
	std::string pw_copy = key_password;
	std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
	    PEM_read_bio_PrivateKey(key_bio.get(), nullptr, SignPemPasswordCb, &pw_copy), EVP_PKEY_free);
	if (!key) {
		ERR_clear_error();
		throw std::runtime_error("could not read private key (wrong key_password, or unsupported PEM)");
	}

	std::unique_ptr<BIO, decltype(&BIO_free)> data_bio(
	    BIO_new_mem_buf(message.data(), static_cast<int>(message.size())), BIO_free);
	if (!data_bio) {
		throw std::runtime_error("out of memory building signed message");
	}

	// CMS_DETACHED: eContent absent (the /Contents blob signs external bytes).
	// CMS_BINARY: PDF bytes are raw, not S/MIME canonical text.
	// CMS_NOSMIMECAP: omit S/MIME capabilities to keep the blob compact.
	unsigned flags = CMS_DETACHED | CMS_BINARY | CMS_NOSMIMECAP;
	std::unique_ptr<CMS_ContentInfo, decltype(&CMS_ContentInfo_free)> cms(
	    CMS_sign(cert.get(), key.get(), nullptr, data_bio.get(), flags), CMS_ContentInfo_free);
	if (!cms) {
		ERR_clear_error();
		throw std::runtime_error("CMS signing failed");
	}

	int der_len = i2d_CMS_ContentInfo(cms.get(), nullptr);
	if (der_len <= 0) {
		ERR_clear_error();
		throw std::runtime_error("CMS DER encoding failed");
	}
	std::string der(static_cast<size_t>(der_len), '\0');
	unsigned char *out_ptr = reinterpret_cast<unsigned char *>(&der[0]);
	if (i2d_CMS_ContentInfo(cms.get(), &out_ptr) != der_len) {
		ERR_clear_error();
		throw std::runtime_error("CMS DER encoding failed");
	}
	return der;
}

void SignDetached(const std::string &input, const std::string &output, const std::string &cert_path,
                  const std::string &key_path, const std::string &key_password, const std::string &reason,
                  const std::string &location, const std::string &signer_name, const std::string &field_name,
                  const std::string &password) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());

	// Load the signing material up front so bad cert/key paths fail before we
	// touch the PDF.
	std::string cert_pem = ReadWholeFile(cert_path);
	std::string key_pem = ReadWholeFile(key_path);

	QPDF doc;
	if (password.empty()) {
		doc.processFile(input.c_str());
	} else {
		doc.processFile(input.c_str(), password.c_str());
	}

	// Build the signature value dictionary with wide placeholders.
	auto sig_dict = QPDFObjectHandle::newDictionary();
	sig_dict.replaceKey("/Type", QPDFObjectHandle::newName("/Sig"));
	sig_dict.replaceKey("/Filter", QPDFObjectHandle::newName("/Adobe.PPKLite"));
	sig_dict.replaceKey("/SubFilter", QPDFObjectHandle::newName("/adbe.pkcs7.detached"));
	// /ByteRange placeholder: three 10-digit sentinels (supports files < 10 GB);
	// patched in place after the deterministic write.
	sig_dict.replaceKey("/ByteRange",
	                    QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle> {
	                        QPDFObjectHandle::newInteger(0), QPDFObjectHandle::newInteger(9999999999LL),
	                        QPDFObjectHandle::newInteger(9999999999LL), QPDFObjectHandle::newInteger(9999999999LL)}));
	// All-NUL string → qpdf writes a hex string `<0000...>` of exactly
	// 2*kSignContentsBytes digits, which becomes the signature hole.
	sig_dict.replaceKey("/Contents", QPDFObjectHandle::newString(std::string(kSignContentsBytes, '\0')));
	// Signing time /M (current UTC), plus optional metadata.
	{
		std::time_t now = std::time(nullptr);
		std::tm tm_utc {};
#if defined(_WIN32)
		gmtime_s(&tm_utc, &now);
#else
		gmtime_r(&now, &tm_utc);
#endif
		char m_buf[32];
		std::strftime(m_buf, sizeof(m_buf), "D:%Y%m%d%H%M%S+00'00'", &tm_utc);
		sig_dict.replaceKey("/M", QPDFObjectHandle::newString(std::string(m_buf)));
	}
	if (!signer_name.empty()) {
		sig_dict.replaceKey("/Name", QPDFObjectHandle::newString(signer_name));
	}
	if (!reason.empty()) {
		sig_dict.replaceKey("/Reason", QPDFObjectHandle::newString(reason));
	}
	if (!location.empty()) {
		sig_dict.replaceKey("/Location", QPDFObjectHandle::newString(location));
	}
	auto sig_obj = doc.makeIndirectObject(sig_dict);

	// Merged signature field / widget annotation on page 1 (invisible: zero Rect).
	auto pages = QPDFPageDocumentHelper(doc).getAllPages();
	if (pages.empty()) {
		throw std::runtime_error("cannot sign a document with no pages");
	}
	auto page0 = pages[0].getObjectHandle();
	auto widget = QPDFObjectHandle::newDictionary();
	widget.replaceKey("/Type", QPDFObjectHandle::newName("/Annot"));
	widget.replaceKey("/Subtype", QPDFObjectHandle::newName("/Widget"));
	widget.replaceKey("/FT", QPDFObjectHandle::newName("/Sig"));
	widget.replaceKey("/T", QPDFObjectHandle::newString(field_name));
	widget.replaceKey("/V", sig_obj);
	widget.replaceKey("/P", page0);
	widget.replaceKey("/F", QPDFObjectHandle::newInteger(132)); // Print | Locked
	widget.replaceKey("/Rect", QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle> {
	                               QPDFObjectHandle::newInteger(0), QPDFObjectHandle::newInteger(0),
	                               QPDFObjectHandle::newInteger(0), QPDFObjectHandle::newInteger(0)}));
	auto widget_obj = doc.makeIndirectObject(widget);

	// Attach the widget to page 1's /Annots.
	auto annots = page0.getKey("/Annots");
	if (!annots.isArray()) {
		annots = QPDFObjectHandle::newArray();
	}
	annots.appendItem(widget_obj);
	page0.replaceKey("/Annots", annots);

	// Register the field in the document AcroForm (create it if absent).
	auto root = doc.getRoot();
	auto acroform = root.getKey("/AcroForm");
	if (!acroform.isDictionary()) {
		acroform = doc.makeIndirectObject(QPDFObjectHandle::newDictionary());
		root.replaceKey("/AcroForm", acroform);
	}
	auto fields = acroform.getKey("/Fields");
	if (!fields.isArray()) {
		fields = QPDFObjectHandle::newArray();
	}
	fields.appendItem(widget_obj);
	acroform.replaceKey("/Fields", fields);
	// SigFlags: bit1 SignaturesExist, bit2 AppendOnly.
	acroform.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(3));

	// Deterministic write to memory: static /ID, no object streams (so the /Sig
	// dict is a plain visible object), streams preserved (predictable bytes), no
	// encryption (encrypting the /Contents string would break the placeholder).
	QPDFWriter writer(doc);
	writer.setOutputMemory();
	writer.setStaticID(true);
	writer.setObjectStreamMode(qpdf_o_disable);
	writer.setStreamDataMode(qpdf_s_preserve);
	writer.setPreserveEncryption(false);
	writer.write();
	auto buffer = writer.getBufferSharedPointer();
	std::string bytes(reinterpret_cast<const char *>(buffer->getBuffer()), buffer->getSize());

	// Locate the /Contents hex hole via its long zero run (unique in the file).
	const std::string zero_run = "<" + std::string(256, '0');
	size_t c_lt = bytes.find(zero_run);
	if (c_lt == std::string::npos) {
		throw std::runtime_error("could not locate /Contents placeholder in written PDF");
	}
	size_t c_gt = bytes.find('>', c_lt);
	if (c_gt == std::string::npos) {
		throw std::runtime_error("malformed /Contents placeholder in written PDF");
	}
	// Hole capacity in hex digits (between '<' and '>').
	size_t hole_hex = c_gt - c_lt - 1;

	// Locate and patch /ByteRange in place. The array occupies [lb, rb].
	size_t br_key = bytes.find("/ByteRange");
	if (br_key == std::string::npos) {
		throw std::runtime_error("could not locate /ByteRange in written PDF");
	}
	size_t lb = bytes.find('[', br_key);
	size_t rb = (lb == std::string::npos) ? std::string::npos : bytes.find(']', lb);
	if (lb == std::string::npos || rb == std::string::npos) {
		throw std::runtime_error("malformed /ByteRange array in written PDF");
	}

	// ByteRange = [0, len_before_hole, offset_after_hole, len_after_hole]. The
	// hole is the /Contents string including its `<` and `>` delimiters.
	const int64_t total = static_cast<int64_t>(bytes.size());
	const int64_t b0 = 0;
	const int64_t b1 = static_cast<int64_t>(c_lt);     // bytes [0, c_lt)
	const int64_t b2 = static_cast<int64_t>(c_gt) + 1; // first byte after '>'
	const int64_t b3 = total - b2;                     // bytes [b2, total)
	std::string br_body = "[0 " + std::to_string(b1) + " " + std::to_string(b2) + " " + std::to_string(b3);
	const size_t span_width = rb - lb + 1; // includes '[' and ']'
	if (br_body.size() + 1 > span_width) { // +1 for the closing ']'
		throw std::runtime_error("ByteRange placeholder too small for computed offsets");
	}
	std::string br_final = br_body + std::string(span_width - br_body.size() - 1, ' ') + "]";
	bytes.replace(lb, span_width, br_final);

	// Build the signed message from the (now patched) spans and CMS-sign it.
	std::string message;
	message.reserve(static_cast<size_t>(b1 + b3));
	message.append(bytes.data() + b0, static_cast<size_t>(b1));
	message.append(bytes.data() + b2, static_cast<size_t>(b3));
	std::string der = CmsSignDetached(message, cert_pem, key_pem, key_password);

	// Hex-encode the DER (uppercase) into the /Contents hole; zero-pad the rest.
	if (der.size() * 2 > hole_hex) {
		throw std::runtime_error("signature (" + std::to_string(der.size()) +
		                         " bytes) exceeds the /Contents placeholder capacity (" + std::to_string(hole_hex / 2) +
		                         " bytes)");
	}
	static const char kHex[] = "0123456789ABCDEF";
	std::string filled(hole_hex, '0');
	for (size_t i = 0; i < der.size(); i++) {
		unsigned char byte = static_cast<unsigned char>(der[i]);
		filled[2 * i] = kHex[(byte >> 4) & 0xF];
		filled[2 * i + 1] = kHex[byte & 0xF];
	}
	bytes.replace(c_lt + 1, hole_hex, filled);

	// Write the final signed bytes to the output path.
	std::ofstream out(output, std::ios::binary | std::ios::trunc);
	if (!out) {
		throw std::runtime_error("could not open output '" + output + "' for writing");
	}
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	if (!out) {
		throw std::runtime_error("failed writing signed PDF to '" + output + "'");
	}
}

// pdf_redact — true (raster) redaction: replace each page carrying a redaction
// box with an image-only page so the underlying text is destroyed, not merely
// covered. The poppler-side caller renders each redacted page to RGB and paints
// the boxes black; this function composes the output document, deflating the
// raster into a FlateDecode DeviceRGB image XObject and rewriting the page.
//===--------------------------------------------------------------------===//

// Deflate raw bytes with zlib via qpdf's own pipeline (no external zlib include).
static std::string FlateDeflate(const std::string &raw) {
	Pl_Buffer collector("pdf_redact-flate");
	Pl_Flate deflate("pdf_redact-deflate", &collector, Pl_Flate::a_deflate);
	deflate.write(reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
	deflate.finish();
	return collector.getString();
}

// Locale-independent fixed-point formatter for PDF numeric operands (never emits
// a locale decimal comma the way std::to_string / ostringstream might).
static std::string PdfNum(double v) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.4f", v);
	return std::string(buf);
}

void RebuildPagesAsImages(const std::string &input, const std::string &output, const std::vector<RebuiltPage> &pages,
                          const std::string &password) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	QPDF doc;
	doc.processFile(input.c_str(), password.empty() ? nullptr : password.c_str());
	QPDFPageDocumentHelper doc_pages(doc);
	auto page_helpers = doc_pages.getAllPages();
	if (page_helpers.size() != pages.size()) {
		throw std::runtime_error("pdf_redact: internal page-count mismatch (" + std::to_string(page_helpers.size()) +
		                         " document pages vs " + std::to_string(pages.size()) + " rebuilt pages)");
	}
	for (size_t i = 0; i < page_helpers.size(); ++i) {
		const RebuiltPage &rp = pages[i];
		if (!rp.redacted) {
			continue; // page without redactions is carried over verbatim
		}
		if (rp.width <= 0 || rp.height <= 0 ||
		    rp.rgb.size() != static_cast<size_t>(rp.width) * static_cast<size_t>(rp.height) * 3) {
			throw std::runtime_error("pdf_redact: malformed raster for page " + std::to_string(i + 1));
		}

		// Image XObject: DeviceRGB, 8 bpc, FlateDecode-compressed raster bytes.
		std::string compressed = FlateDeflate(rp.rgb);
		QPDFObjectHandle image = QPDFObjectHandle::newStream(&doc);
		image.replaceStreamData(compressed, QPDFObjectHandle::newName("/FlateDecode"), QPDFObjectHandle::newNull());
		QPDFObjectHandle idict = image.getDict();
		idict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
		idict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
		idict.replaceKey("/Width", QPDFObjectHandle::newInteger(rp.width));
		idict.replaceKey("/Height", QPDFObjectHandle::newInteger(rp.height));
		idict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
		idict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
		QPDFObjectHandle image_ind = doc.makeIndirectObject(image);

		// Resources: expose the image as /Im0.
		QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
		xobjects.replaceKey("/Im0", image_ind);
		QPDFObjectHandle resources = QPDFObjectHandle::newDictionary();
		resources.replaceKey("/XObject", xobjects);

		// Content: scale the unit image square to the whole page and paint it.
		std::string content = "q " + PdfNum(rp.media_w) + " 0 0 " + PdfNum(rp.media_h) + " 0 0 cm /Im0 Do Q\n";
		QPDFObjectHandle contents = QPDFObjectHandle::newStream(&doc, content);
		QPDFObjectHandle contents_ind = doc.makeIndirectObject(contents);

		// Rewrite the page in place: image-only content, fresh MediaBox at the
		// raster's display size, no rotation (the raster is already upright), and
		// no annotations (an annotation could re-introduce extractable text).
		QPDFObjectHandle page = page_helpers[i].getObjectHandle();
		QPDFObjectHandle media_box = QPDFObjectHandle::newArray();
		media_box.appendItem(QPDFObjectHandle::newInteger(0));
		media_box.appendItem(QPDFObjectHandle::newInteger(0));
		media_box.appendItem(QPDFObjectHandle::newReal(PdfNum(rp.media_w)));
		media_box.appendItem(QPDFObjectHandle::newReal(PdfNum(rp.media_h)));
		page.replaceKey("/Contents", contents_ind);
		page.replaceKey("/Resources", resources);
		page.replaceKey("/MediaBox", media_box);
		page.removeKey("/Rotate");
		page.removeKey("/CropBox");
		page.removeKey("/Annots");
	}
	QPDFWriter writer(doc, output.c_str());
	writer.write();
}

} // namespace pdf_qpdf
