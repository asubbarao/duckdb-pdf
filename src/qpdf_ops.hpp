//===--------------------------------------------------------------------===//
// qpdf_ops.hpp — plain-C++ interface to the qpdf-backed document operations.
//
// TRANSLATION-UNIT ISOLATION: qpdf headers require C++17, but DuckDB drives
// the extension build at whatever standard it uses itself (-std=c++11 on the
// Linux registry CI). Compiling ANY duckdb header at C++17 against a C++11
// libduckdb_static is an ODR violation: duckdb's `static constexpr` members
// (e.g. FileFlags::FILE_FLAGS_READ) get out-of-line definitions in the C++11
// archive AND implicitly-inline definitions in a C++17 TU — `ld` then fails
// with "multiple definition of duckdb::FileFlags::FILE_FLAGS_READ".
//
// The fix: qpdf_ops.cpp is the ONLY C++17 TU (pinned in CMakeLists.txt) and
// includes qpdf + the standard library ONLY — no duckdb headers, no poppler.
// This header is the boundary: plain std types in, plain std types out, no
// qpdf types, no duckdb types. pdf_extension.cpp compiles at DuckDB's own
// standard and never sees a qpdf header.
//
// Error contract: every function reports failure by throwing an exception
// derived from std::exception (qpdf's own QPDFExc, or std::runtime_error);
// the caller wraps e.what() into the appropriate duckdb exception type.
//===--------------------------------------------------------------------===//
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pdf_qpdf {

// Concatenates the inputs' pages in list order into `output`.
// The caller validates that inputs exist and the output directory exists.
void Merge(const std::vector<std::string> &inputs, const std::string &output);

// Adds `degrees` (a multiple of 90, validated by the caller) to the existing
// rotation of the selected pages. When all_pages is false, pages_spec is a
// qpdf-style numeric range ('1-3,7' / 'z' = last / 'r2' = second-to-last);
// malformed / out-of-range specs throw with qpdf's own descriptive message.
void Rotate(const std::string &input, const std::string &output, int degrees, bool all_pages,
            const std::string &pages_spec);

// Writes one single-page PDF per page as <output_dir>/<stem>_p<N>.pdf
// (N zero-padded to the page-count width); appends (page, file) per emitted
// file. The caller validates input/output_dir and computes the stem.
void Split(const std::string &input, const std::string &output_dir, const std::string &stem,
           std::vector<std::pair<int, std::string>> &emitted);

// Structural optimization only: object streams, stream recompression, and
// linearization ("fast web view"). Image data is carried over as-is.
void Compress(const std::string &input, const std::string &output);

// AES-256 (R6) with all permissions allowed; the caller has already resolved
// an empty owner password to the user password.
void Encrypt(const std::string &input, const std::string &output, const std::string &user_password,
             const std::string &owner_password);

// Opens with the password (user or owner) and writes an unencrypted copy.
// A wrong password surfaces qpdf's "invalid password" via the error contract.
void Decrypt(const std::string &input, const std::string &output, const std::string &password);

// Extracts the page subset selected by a qpdf-style numeric range into a new
// document, in range order (repeats allowed — repeated pages are shallow-copied
// because copyForeignObject caches per source object).
void Pages(const std::string &input, const std::string &output, const std::string &ranges);

// One entry per AcroForm field. `type` is qpdf's raw field type name
// ('/Tx', '/Btn', ...) — the caller maps it to display text.
struct FormField {
	bool has_page = false; // false = field has no widget annotation on any page
	int page = 0;          // 1-based when has_page
	std::string name;      // fully qualified field name
	std::string type;      // raw qpdf field type name
	bool has_value = false;
	std::string value;
	bool is_required = false;
};
std::vector<FormField> ReadFormFields(const std::string &path);

// One entry per page annotation. `subtype` is qpdf's raw name ('/Link', ...);
// uri is populated for Link annotations with an /A /URI action.
struct Annotation {
	int page = 0; // 1-based
	std::string subtype;
	bool has_contents = false;
	std::string contents;
	bool has_uri = false;
	std::string uri;
	double rect_x0 = 0, rect_y0 = 0, rect_x1 = 0, rect_y1 = 0;
};
std::vector<Annotation> ReadAnnotations(const std::string &path);

} // namespace pdf_qpdf
