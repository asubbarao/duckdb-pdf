// poppler_private_ops.cpp — setErrorCallback + missing-font flag (C++20 TU).
//
// MUST NOT include duckdb.hpp (ODR with C++11 libduckdb_static). Only poppler
// private headers + std. Poppler private-api headers need C++20 (std::span).
// See base14_fonts.hpp for the public surface.

#include "base14_fonts.hpp"

#include "Error.h"
#include <poppler-version.h>
#if POPPLER_VERSION_MAJOR < 25
#include "PDFDoc.h"
#include "Catalog.h"
#include "Stream.h"
#include "goo/GooString.h"
#include <optional>
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace duckdb {

static bool g_poppler_missing_display_font = false;

// Opt-in stderr for poppler library chatter. Equality check only (no regex).
static bool PopplerLogToStderr() {
	const char *e = std::getenv("PDF_POPPLER_LOG");
	return e && std::strcmp(e, "1") == 0;
}

static void PdfPopplerErrorCallback(ErrorCategory /*category*/, Goffset pos, const char *msg) {
	if (!msg) {
		return;
	}
	// setErrorCallback replaces poppler's default stderr printer. We still
	// track missing-display-font for fail-loud font paths, but do NOT spam the
	// user terminal by default — corrupt/open failures throw via LoadDoc.
	// Opt in: PDF_POPPLER_LOG=1.
	if (PopplerLogToStderr()) {
		if (pos >= 0) {
			std::fprintf(stderr, "poppler/error (%lld): %s\n", static_cast<long long>(pos), msg);
		} else {
			std::fprintf(stderr, "poppler/error: %s\n", msg);
		}
	}
	// Case-insensitive substring — do not invent a full error taxonomy.
	std::string lower;
	lower.reserve(std::strlen(msg));
	for (const char *p = msg; *p; ++p) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
	}
	if (lower.find("no display font") != std::string::npos || lower.find("couldn't find a font") != std::string::npos) {
		g_poppler_missing_display_font = true;
	}
}

void EnsurePopplerErrorCallback() {
	static bool installed = false;
	if (!installed) {
		setErrorCallback(PdfPopplerErrorCallback);
		installed = true;
	}
}

void ResetPopplerMissingDisplayFont() {
	g_poppler_missing_display_font = false;
}

bool PopplerMissingDisplayFont() {
	return g_poppler_missing_display_font;
}

bool PopplerLegacyDocumentFeatures(const std::string &bytes, const std::string &password, bool &javascript,
                                   int &form_type) {
#if POPPLER_VERSION_MAJOR < 25
	std::optional<GooString> pw;
	if (!password.empty())
		pw.emplace(password);
	auto stream = new MemStream(bytes.data(), 0, static_cast<Goffset>(bytes.size()), Object(objNull));
	PDFDoc doc(stream, pw, pw);
	if (!doc.isOk() || !doc.getCatalog())
		return false;
	javascript = doc.hasJavascript();
	form_type = static_cast<int>(doc.getCatalog()->getFormType());
	return true;
#else
	(void)bytes;
	(void)password;
	(void)javascript;
	(void)form_type;
	return false;
#endif
}

} // namespace duckdb
