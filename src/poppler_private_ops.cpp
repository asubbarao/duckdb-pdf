// poppler_private_ops.cpp — setErrorCallback + missing-font flag (C++17 TU).
//
// MUST NOT include duckdb.hpp (ODR with C++11 libduckdb_static). Only poppler
// private headers + std. See base14_fonts.hpp for the public surface.

#include "base14_fonts.hpp"

#include "Error.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace duckdb {

static bool g_poppler_missing_display_font = false;

static void PdfPopplerErrorCallback(ErrorCategory /*category*/, Goffset pos, const char *msg) {
	if (!msg) {
		return;
	}
	// setErrorCallback replaces poppler's default stderr printer — re-emit so
	// other diagnostics (malformed PDF, bad password, …) stay visible.
	if (pos >= 0) {
		std::fprintf(stderr, "poppler/error (%lld): %s\n", static_cast<long long>(pos), msg);
	} else {
		std::fprintf(stderr, "poppler/error: %s\n", msg);
	}
	// Case-insensitive substring — do not invent a full error taxonomy.
	std::string lower;
	lower.reserve(std::strlen(msg));
	for (const char *p = msg; *p; ++p) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
	}
	if (lower.find("no display font") != std::string::npos ||
	    lower.find("couldn't find a font") != std::string::npos) {
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

} // namespace duckdb
