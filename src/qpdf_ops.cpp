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
#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QUtil.hh>

// OpenSSL — CMS/PKCS#7 verification for pdf_signatures (tier 2). Plain C API;
// touches neither duckdb nor poppler, so it is safe to include in this TU.
#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/x509.h>

// zlib — DEFLATE + CRC32 for the minimal PNG encoder used by ReadImages to wrap
// fully-decoded raster samples. libqpdf already links zlib, and <zlib.h> is on
// the vcpkg/homebrew include path; this is a plain C API touching neither duckdb
// nor poppler, so it is safe in this TU.
#include <zlib.h>

#include <cmath>
#include <cstdint>
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
// pdf_images — extract embedded raster image XObjects (the stored JPEG/bitmap,
// not a page render). Walk each page's image resources via
// QPDFPageObjectHelper::getImages(); per image, inspect the filter chain and
// either pass the encoded bytes through (DCT->jpeg, JPX->jp2, CCITT->ccitt) or
// fully decode and re-wrap the raw samples as PNG (Flate/RunLength/LZW/none),
// falling back to 'raw' for colorspaces/bit depths we cannot losslessly wrap.
//===--------------------------------------------------------------------===//
namespace {

// A stream filter name with the leading slash stripped ('FlateDecode'). PDF
// permits inline-image abbreviations; XObjects use full names, but we normalize
// the handful of abbreviations defensively.
std::string NormalizeFilterName(const std::string &raw) {
	std::string n = (!raw.empty() && raw[0] == '/') ? raw.substr(1) : raw;
	if (n == "DCT") {
		return "DCTDecode";
	}
	if (n == "CCF") {
		return "CCITTFaxDecode";
	}
	if (n == "Fl") {
		return "FlateDecode";
	}
	if (n == "LZW") {
		return "LZWDecode";
	}
	if (n == "RL") {
		return "RunLengthDecode";
	}
	if (n == "AHx") {
		return "ASCIIHexDecode";
	}
	if (n == "A85") {
		return "ASCII85Decode";
	}
	return n;
}

std::vector<std::string> StreamFilterNames(QPDFObjectHandle dict) {
	std::vector<std::string> names;
	if (!dict.isDictionary()) {
		return names;
	}
	auto filter = dict.getKey("/Filter");
	auto add = [&](QPDFObjectHandle item) {
		if (item.isName()) {
			names.push_back(NormalizeFilterName(item.getName()));
		}
	};
	if (filter.isName()) {
		add(filter);
	} else if (filter.isArray()) {
		int n = filter.getArrayNItems();
		for (int i = 0; i < n; i++) {
			add(filter.getArrayItem(i));
		}
	}
	return names;
}

// Raw PDF colorspace name: a name directly ('DeviceRGB') or the leading name of
// a colorspace array ('ICCBased', 'Indexed', 'DeviceN', ...). Empty if absent.
std::string ColorSpaceName(QPDFObjectHandle dict) {
	if (!dict.isDictionary()) {
		return std::string();
	}
	auto cs = dict.getKey("/ColorSpace");
	if (cs.isName()) {
		std::string s = cs.getName();
		return (!s.empty() && s[0] == '/') ? s.substr(1) : s;
	}
	if (cs.isArray() && cs.getArrayNItems() >= 1) {
		auto first = cs.getArrayItem(0);
		if (first.isName()) {
			std::string s = first.getName();
			return (!s.empty() && s[0] == '/') ? s.substr(1) : s;
		}
	}
	return std::string();
}

int DictIntOrDefault(QPDFObjectHandle dict, const char *key, int def) {
	if (!dict.isDictionary()) {
		return def;
	}
	auto v = dict.getKey(key);
	if (v.isInteger()) {
		return static_cast<int>(v.getIntValue());
	}
	return def;
}

std::string BufferToString(const std::shared_ptr<Buffer> &buf) {
	if (!buf || buf->getSize() == 0) {
		return std::string();
	}
	return std::string(reinterpret_cast<const char *>(buf->getBuffer()), buf->getSize());
}

// Pipe stream data at a given decode level with no re-encoding (encode_flags=0),
// so generalized/non-lossy filters are undone while lossy terminal filters
// (DCT/JPX) and undecodable ones (CCITT) are left intact.
std::string PipeStreamAtLevel(QPDFObjectHandle stream, qpdf_stream_decode_level_e level) {
	Pl_Buffer pl("pdf_images");
	bool attempted = false;
	stream.pipeStreamData(&pl, &attempted, 0 /* encode_flags: no recompress */, level, true /* suppress warnings */,
	                      false);
	pl.finish();
	return BufferToString(pl.getBufferSharedPointer());
}

void PngPut32(std::string &s, uint32_t v) {
	s.push_back(static_cast<char>((v >> 24) & 0xff));
	s.push_back(static_cast<char>((v >> 16) & 0xff));
	s.push_back(static_cast<char>((v >> 8) & 0xff));
	s.push_back(static_cast<char>(v & 0xff));
}

void PngAppendChunk(std::string &out, const char *type, const std::string &data) {
	PngPut32(out, static_cast<uint32_t>(data.size()));
	size_t crc_start = out.size();
	out.append(type, 4);
	out.append(data);
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, reinterpret_cast<const Bytef *>(out.data() + crc_start), static_cast<uInt>(4 + data.size()));
	PngPut32(out, static_cast<uint32_t>(crc));
}

// Minimal PNG encoder: IHDR + a single zlib-deflated IDAT (per-row filter byte 0)
// + IEND. color_type 0 (grayscale) or 2 (truecolor RGB); bit_depth 1 or 8.
// Returns empty on any inconsistency so the caller can fall back to 'raw'.
std::string EncodePng(int width, int height, int channels, int bit_depth, int color_type, const std::string &samples) {
	if (width <= 0 || height <= 0) {
		return std::string();
	}
	size_t row_bytes = (static_cast<size_t>(width) * channels * bit_depth + 7) / 8;
	if (samples.size() < row_bytes * static_cast<size_t>(height)) {
		return std::string(); // decoded data too short for the declared geometry
	}
	std::string raw;
	raw.reserve((row_bytes + 1) * static_cast<size_t>(height));
	for (int y = 0; y < height; y++) {
		raw.push_back(0); // PNG filter type: None
		raw.append(samples.data() + static_cast<size_t>(y) * row_bytes, row_bytes);
	}
	uLongf bound = compressBound(static_cast<uLong>(raw.size()));
	std::string idat;
	idat.resize(bound);
	int rc = compress2(reinterpret_cast<Bytef *>(&idat[0]), &bound, reinterpret_cast<const Bytef *>(raw.data()),
	                   static_cast<uLong>(raw.size()), Z_DEFAULT_COMPRESSION);
	if (rc != Z_OK) {
		return std::string();
	}
	idat.resize(bound);

	std::string out;
	static const unsigned char kSig[8] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
	out.append(reinterpret_cast<const char *>(kSig), 8);
	std::string ihdr;
	PngPut32(ihdr, static_cast<uint32_t>(width));
	PngPut32(ihdr, static_cast<uint32_t>(height));
	ihdr.push_back(static_cast<char>(bit_depth));
	ihdr.push_back(static_cast<char>(color_type));
	ihdr.push_back(0); // compression method: deflate
	ihdr.push_back(0); // filter method: adaptive (only type 0 used)
	ihdr.push_back(0); // interlace: none
	PngAppendChunk(out, "IHDR", ihdr);
	PngAppendChunk(out, "IDAT", idat);
	PngAppendChunk(out, "IEND", std::string());
	return out;
}

bool IsGrayColorSpace(const std::string &cs) {
	return cs == "DeviceGray" || cs == "CalGray" || cs == "G";
}

bool IsRgbColorSpace(const std::string &cs) {
	return cs == "DeviceRGB" || cs == "CalRGB" || cs == "RGB";
}

} // namespace

std::vector<EmbeddedImage> ReadImages(const std::string &path, const std::string &password) {
	std::lock_guard<std::recursive_mutex> qpdf_guard(QpdfMutex());
	std::vector<EmbeddedImage> images;
	QPDF doc;
	if (password.empty()) {
		doc.processFile(path.c_str());
	} else {
		doc.processFile(path.c_str(), password.c_str());
	}
	QPDFPageDocumentHelper doc_pages(doc);
	auto pages = doc_pages.getAllPages();
	for (size_t page_idx = 0; page_idx < pages.size(); page_idx++) {
		int image_index = 0;
		// std::map: iterated in resource-name order, so image_index is stable.
		for (auto &entry : QPDFPageObjectHelper(pages[page_idx]).getImages()) {
			const std::string &res_name = entry.first;
			QPDFObjectHandle stream = entry.second;
			if (!stream.isStream()) {
				continue;
			}
			auto dict = stream.getDict();
			EmbeddedImage img;
			img.page = static_cast<int>(page_idx + 1);
			img.image_index = ++image_index;
			// getImages() keys carry the leading slash ('/Im0'); strip it.
			img.name = (!res_name.empty() && res_name[0] == '/') ? res_name.substr(1) : res_name;
			img.width = DictIntOrDefault(dict, "/Width", 0);
			img.height = DictIntOrDefault(dict, "/Height", 0);
			bool image_mask =
			    dict.isDictionary() && dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue();
			img.bits_per_component = DictIntOrDefault(dict, "/BitsPerComponent", image_mask ? 1 : 8);
			img.colorspace = ColorSpaceName(dict);

			auto filters = StreamFilterNames(dict);
			std::string terminal = filters.empty() ? std::string() : filters.back();

			try {
				if (terminal == "DCTDecode" || terminal == "JPXDecode" || terminal == "CCITTFaxDecode") {
					// Lossy / undecodable terminal filter: pass the encoded bytes
					// through. A lone terminal filter is exact via getRawStreamData;
					// a chain undoes the earlier generalized filters at the
					// specialized level, which never touches DCT/JPX/CCITT.
					if (filters.size() <= 1) {
						img.data = BufferToString(stream.getRawStreamData());
					} else {
						img.data = PipeStreamAtLevel(stream, qpdf_dl_specialized);
					}
					img.format = (terminal == "DCTDecode") ? "jpeg" : (terminal == "JPXDecode") ? "jp2" : "ccitt";
				} else {
					// Generalized/non-lossy (Flate/RunLength/LZW/ASCII/none): fully
					// decode to raw samples, then wrap as PNG when we can do so
					// losslessly; otherwise surface the decoded samples as 'raw'.
					std::string decoded = BufferToString(stream.getStreamData(qpdf_dl_all));
					std::string png;
					if (!image_mask && IsGrayColorSpace(img.colorspace) &&
					    (img.bits_per_component == 1 || img.bits_per_component == 8)) {
						png = EncodePng(img.width, img.height, 1, img.bits_per_component, 0 /* grayscale */, decoded);
					} else if (!image_mask && IsRgbColorSpace(img.colorspace) && img.bits_per_component == 8) {
						png = EncodePng(img.width, img.height, 3, 8, 2 /* truecolor */, decoded);
					}
					if (!png.empty()) {
						img.format = "png";
						img.data = std::move(png);
					} else {
						img.format = "raw";
						img.data = std::move(decoded);
					}
				}
			} catch (...) {
				// Best-effort per image: a stream whose data cannot be extracted
				// (unknown filter chain, corrupt stream) contributes no row rather
				// than aborting the whole document.
				continue;
			}
			images.push_back(std::move(img));
		}
	}
	return images;
}

} // namespace pdf_qpdf
