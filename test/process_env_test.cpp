// Rendering a page must leave the process environment as it found it.
//
// Every child process the host starts inherits its environment. When page
// rendering set FONTCONFIG_FILE / FONTCONFIG_PATH to the extension's 14 bundled
// fonts, LibreOffice started afterwards found no other fonts and failed to
// convert. Run from the repository root (`make test` does), after the build.
#include "duckdb.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef _WIN32
static std::string Env(const char *name) {
	const char *value = std::getenv(name);
	return value ? std::string("set:") + value : "unset";
}

static std::string Child(const char *command) {
	std::string out;
	FILE *pipe = popen(command, "r");
	if (!pipe) {
		return "popen failed";
	}
	char buffer[4096];
	size_t n;
	while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
		out.append(buffer, n);
	}
	pclose(pipe);
	return out;
}

static bool Same(const char *what, const std::string &before, const std::string &after) {
	if (before == after) {
		std::cout << "ok   " << what << ": " << after << "\n";
		return true;
	}
	std::cout << "FAIL " << what << " changed\n  before: " << before << "\n  after:  " << after << "\n";
	return false;
}
#endif

int main() {
#ifdef _WIN32
	// The extension never touched the environment on Windows, and popen differs.
	std::cout << "skip: POSIX only\n";
	return 0;
#else
	// What a child sees: its own FONTCONFIG_* and how many fonts fontconfig lists.
	const char *child_env = "printf 'FONTCONFIG_FILE=%s FONTCONFIG_PATH=%s' \"${FONTCONFIG_FILE-unset}\" "
	                        "\"${FONTCONFIG_PATH-unset}\"";
	const char *child_fonts = "command -v fc-list >/dev/null 2>&1 && fc-list | wc -l || echo 'no fc-list'";

	const std::string file_before = Env("FONTCONFIG_FILE");
	const std::string path_before = Env("FONTCONFIG_PATH");
	const std::string child_env_before = Child(child_env);
	const std::string child_fonts_before = Child(child_fonts);

	duckdb::DuckDB db(nullptr);
	duckdb::Connection con(db);
	auto load = con.Query("LOAD pdf");
	if (load->HasError()) {
		std::cout << "FAIL LOAD pdf: " << load->GetError() << "\n";
		return 1;
	}
	// Forced OCR rasterizes the page, which registers the bundled base-14 fonts.
	auto ocr = con.Query("SELECT count(*) FROM read_pdf_words('test/data/scanned.pdf', ocr := true) "
	                     "WHERE source = 'ocr'");
	if (ocr->HasError()) {
		std::cout << "FAIL OCR read: " << ocr->GetError() << "\n";
		return 1;
	}
	auto chunk = ocr->Fetch();
	const int64_t words = chunk ? chunk->GetValue(0, 0).GetValue<int64_t>() : 0;
	if (words == 0) {
		std::cout << "FAIL OCR read returned no OCR words, so no page was rendered\n";
		return 1;
	}
	std::cout << "ok   OCR read: " << words << " words\n";

	bool pass = true;
	pass &= Same("FONTCONFIG_FILE in this process", file_before, Env("FONTCONFIG_FILE"));
	pass &= Same("FONTCONFIG_PATH in this process", path_before, Env("FONTCONFIG_PATH"));
	pass &= Same("FONTCONFIG_* in a child process", child_env_before, Child(child_env));
	pass &= Same("fonts a child's fc-list sees", child_fonts_before, Child(child_fonts));
	return pass ? 0 : 1;
#endif
}
