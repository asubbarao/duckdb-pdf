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

// Extracts each page range in `ranges` (1-based, inclusive [first, last])
// into its own output PDF at <output_dir>/<stem>_doc<K>.pdf (K 1-based,
// zero-padded to ranges.size()'s digit width); appends each written path to
// `emitted`, in range order. Used by pdf_split_blank: the caller (poppler
// side, pdf_extension.cpp) detects blank-page separators and computes the
// content-page ranges; this function only does the qpdf extraction, exactly
// like Split above but for arbitrary multi-page ranges instead of one page
// each. The caller validates input/output_dir and computes stem/ranges; an
// out-of-bounds or empty range throws (via the error contract).
void SplitRanges(const std::string &input, const std::string &output_dir, const std::string &stem,
                 const std::vector<std::pair<int, int>> &ranges, std::vector<std::string> &emitted);

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

// One axis-aligned-ish ruled line segment collected from a page content stream,
// used to recover lattice (bordered) tables. Endpoints are in PDF user space
// (origin bottom-left) as produced by the content-stream path interpreter after
// applying the CTM. `page_height` is that page's MediaBox height, carried so the
// geometry consumer can flip horizontal rules into poppler's top-down text_list
// coordinate space. All length filtering, axis classification, y-flipping, and
// clustering into rule positions happen in the caller (pure geometry, no qpdf);
// this struct is only the raw qpdf-collected boundary.
struct RuledSegment {
	int page = 0; // 1-based
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	double page_height = 0;
};

// Parse every page's content stream and return the axis-aligned stroke/rectangle
// segments usable as table ruling lines, each tagged with its 1-based page and
// that page's MediaBox height. The document is read from an in-memory byte
// buffer so the caller keeps reading files through its own filesystem; an empty
// `password` opens an unencrypted document. Best-effort per page: a page whose
// content stream fails to parse contributes no segments rather than aborting the
// whole document. A fundamental open failure (bad password / corrupt file) is
// reported via the error contract (throws).
std::vector<RuledSegment> ExtractRulingLines(const std::string &pdf_bytes, const std::string &password);

// One entry per filled AcroForm /Sig field (empty/unsigned signature fields
// are skipped). Detection metadata plus OpenSSL CMS verification of the
// detached PKCS#7/CMS blob (/Contents) over the /ByteRange spans.
//
// Empty-string means SQL NULL for subfilter / signer_name / reason / location
// / signing_time_raw. `signing_time_raw` is the raw PDF /M date string
// (D:YYYYMMDD...) which the caller parses into a TIMESTAMP. `has_verified`
// distinguishes a real CMS true/false result from "no /Contents blob", in
// which case `verified` stays undefined and the caller emits NULL.
struct SignatureInfo {
	std::string field_name;         // fully qualified field name
	std::string subfilter;          // /SubFilter display text ('' = NULL)
	std::string signing_time_raw;   // raw /M date string ('' = NULL)
	std::string signer_name;        // /Name, else CMS signer CN ('' = NULL)
	std::string reason;             // /Reason ('' = NULL)
	std::string location;           // /Location ('' = NULL)
	bool covers_whole_file = false; // ByteRange starts at 0 and ends at EOF
	bool has_verified = false;      // false = no /Contents → verified is NULL
	bool verified = false;          // CMS_verify result when has_verified
};
// `password` may be empty (unencrypted / owner-open). The caller validates the
// path exists; a wrong password or unreadable file surfaces via the error
// contract (thrown std::exception).
std::vector<SignatureInfo> ReadSignatures(const std::string &path, const std::string &password);

// One entry per embedded image XObject per page (the actual stored raster, not a
// page render). `image_index` is 1-based within its page; `name` is the resource
// name ('Im0'). `colorspace` is the raw PDF colorspace name ('DeviceRGB',
// 'ICCBased', 'Indexed', ...). `format` and `data` are paired:
//   'jpeg' — /DCTDecode terminal filter; `data` is the raw JPEG (JFIF) bytes,
//            passthrough (any earlier generalized filter such as /FlateDecode is
//            undone first, the DCT layer is preserved).
//   'jp2'  — /JPXDecode terminal filter; `data` is the raw JPEG-2000 codestream.
//   'ccitt'— /CCITTFaxDecode terminal filter (qpdf cannot decode it); `data` is
//            the raw fax-encoded bytes.
//   'png'  — everything else (Flate/RunLength/LZW/none) fully decoded to raw
//            samples and re-wrapped as a PNG so `data` is directly usable. Only
//            DeviceGray/CalGray 1- or 8-bit and DeviceRGB/CalRGB 8-bit are
//            wrapped as PNG.
//   'raw'  — decoded samples that we do not know how to wrap losslessly as PNG
//            (DeviceCMYK, Indexed, ICCBased, DeviceN, image masks, exotic
//            bit depths, ...); `data` is the decoded sample bytes as-is.
struct EmbeddedImage {
	int page = 0;        // 1-based
	int image_index = 0; // 1-based within page
	std::string name;    // resource name ('Im0')
	int width = 0;
	int height = 0;
	int bits_per_component = 0;
	std::string colorspace; // raw PDF colorspace name
	std::string format;     // 'jpeg' | 'jp2' | 'ccitt' | 'png' | 'raw'
	std::string data;       // encoded/wrapped bytes per `format`
};
// `password` may be empty (unencrypted / owner-open). The caller validates the
// path exists; a wrong password or unreadable file surfaces via the error
// contract (thrown std::exception). A single image whose stream data cannot be
// extracted is skipped rather than aborting the whole document.
std::vector<EmbeddedImage> ReadImages(const std::string &path, const std::string &password);

// Stamps `text` as a large diagonal (45°) gray watermark centered on every
// page, on TOP of existing content (a fresh Helvetica text run appended to each
// page's content stream, drawn through an ExtGState alpha). Because extractors
// cannot regroup glyphs laid along a 45° baseline, an invisible (render mode 3)
// horizontal copy of the same text is also stamped so the watermark round-trips
// through the extracted text layer. `opacity` is the fill/stroke alpha
// (0 < opacity <= 1, validated by the caller). Each page is sized from its own
// MediaBox so mixed-size documents stamp correctly.
void Watermark(const std::string &input, const std::string &output, const std::string &text, double opacity);

// Bates numbering: stamps `prefix` followed by a zero-padded (min 6 digits)
// sequential number (start_number, start_number+1, ...) at the bottom-right of
// each page as real Helvetica text. When `stamped_labels` is non-null, the full
// label applied to each page is appended in page order. Each page is positioned
// from its own MediaBox.
void Bates(const std::string &input, const std::string &output, const std::string &prefix, long long start_number,
           std::vector<std::string> *stamped_labels = nullptr);

//===--------------------------------------------------------------------===//
// pdf_sign — create a PKCS#7/CMS detached digital signature (the inverse of
// ReadSignatures). qpdf builds an AcroForm /Sig field with a /ByteRange and a
// fixed-width /Contents hex placeholder and writes the document deterministically
// to an in-memory buffer; the ByteRange is then patched in place, the two spans
// are hashed and signed with OpenSSL CMS_sign (detached, DER, PEM cert+key), and
// the DER is hex-encoded into the /Contents hole before the final bytes are
// written to `output`. The result verifies through ReadSignatures with
// covers_whole_file = true and verified = true.
//
// `cert_path` / `key_path` are PEM files (X.509 certificate, PKCS#8/PKCS#1
// private key). `key_password` decrypts an encrypted key ('' = unencrypted).
// `reason` / `location` / `signer_name` populate the /Reason /Location /Name
// dictionary keys ('' = omit). `field_name` names the /Sig field (the caller
// defaults it to 'Signature1'). `password` opens an encrypted input ('' = none).
//
// Throws std::runtime_error on: unreadable cert/key, wrong key_password, a CMS
// signing failure, or a DER blob larger than the /Contents placeholder.
void SignDetached(const std::string &input, const std::string &output, const std::string &cert_path,
                  const std::string &key_path, const std::string &key_password, const std::string &reason,
                  const std::string &location, const std::string &signer_name, const std::string &field_name,
                  const std::string &password);

// One output page for pdf_redact. When `redacted` is true, the page is REPLACED
// by an image-only page whose sole content is a single DeviceRGB image XObject
// (8 bits/component) drawn to fill a MediaBox of `media_w` x `media_h` points —
// there is no text object left on the page, so text under the redaction boxes
// becomes permanently unextractable. `rgb` holds width*height*3 bytes, row-major,
// TOP-DOWN (row 0 is the visual top of the page), already flipped/painted by the
// caller. When `redacted` is false the original page is copied through untouched
// and the image fields are ignored.
struct RebuiltPage {
	bool redacted = false;
	int width = 0;      // raster pixel width  (redacted pages only)
	int height = 0;     // raster pixel height (redacted pages only)
	double media_w = 0; // target MediaBox width in points  (redacted pages only)
	double media_h = 0; // target MediaBox height in points (redacted pages only)
	std::string rgb;    // width*height*3 packed RGB bytes  (redacted pages only)
};

// Compose `output` from `input`: every page flagged `redacted` in `pages` has its
// content, resources, annotations and rotation replaced by a single full-page
// FlateDecode DeviceRGB image (see RebuiltPage); every unflagged page is carried
// over verbatim. `pages.size()` MUST equal the input's page count (the caller
// enforces this). `password` may be empty (unencrypted / owner-open). The caller
// validates paths and that input != output; a bad password or unreadable file
// surfaces via the error contract (thrown std::exception). Runs under QpdfMutex.
void RebuildPagesAsImages(const std::string &input, const std::string &output, const std::vector<RebuiltPage> &pages,
                          const std::string &password);

//===--------------------------------------------------------------------===//
// Structure / encryption / repair / JSON — high-value qpdf inspection surface
// that poppler-cpp does not expose (xref table, encryption method bits, object
// counts, linearization check, JSON structure dump, content-normalization
// repair). Plain-std types only; caller maps to DuckDB columns.
//===--------------------------------------------------------------------===//

// One-row document census from the qpdf object model (not poppler).
struct DocumentStats {
	bool is_linearized = false;
	// true when isLinearized() && checkLinearization() reported no issues.
	// For non-linearized files this stays false (there is nothing to check).
	bool linearized_ok = false;
	int page_count = 0;
	// Approximate indirect-object count (qpdf::getObjectCount).
	int64_t object_count = 0;
	// Xref table census from getXRefTable().
	int64_t xref_total = 0;
	int64_t xref_free = 0;         // type 0
	int64_t xref_uncompressed = 0; // type 1
	int64_t xref_compressed = 0;   // type 2 (object streams)
	// Encryption dictionary summary. When !is_encrypted the method strings are
	// "none" and R/P/V are 0.
	bool is_encrypted = false;
	int enc_R = 0;             // revision
	int enc_P = 0;             // permissions bitfield as stored
	int enc_V = 0;             // algorithm version
	std::string stream_method; // "none" | "unknown" | "rc4" | "aes" | "aesv3"
	std::string string_method;
	std::string file_method;
	bool owner_password_matched = false;
	bool user_password_matched = false;
	// Permission helpers (meaningful when encrypted; true when unencrypted).
	bool allow_accessibility = true;
	bool allow_extract = true;
	bool allow_print_low = true;
	bool allow_print_high = true;
	bool allow_modify_assembly = true;
	bool allow_modify_form = true;
	bool allow_modify_annotation = true;
	bool allow_modify_other = true;
	bool allow_modify_all = true;
	// Warning count drained from qpdf after open (dangling refs etc.).
	int64_t warning_count = 0;
};

// Opens `path` with optional password and returns the structure census above.
// Wrong password / unreadable file throws via the error contract.
DocumentStats InspectDocument(const std::string &path, const std::string &password);

// Dumps the document as qpdf JSON (version 2 by default). Stream *payloads* are
// omitted (qpdf_sj_none) so the result is a structural map suitable for SQL
// inspection, not a multi-megabyte re-encoding of every image. `password` may
// be empty. Throws on open/write failure.
std::string WriteJson(const std::string &path, const std::string &password, int json_version = 2);

// Best-effort structural repair: fix dangling references, content-stream
// normalization, stream recompression. Writes a new file at `output`. Does not
// attempt OCR or raster rebuilds. Password may be empty.
void Repair(const std::string &input, const std::string &output, const std::string &password);

} // namespace pdf_qpdf
