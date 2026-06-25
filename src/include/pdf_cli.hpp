// pdf_cli.hpp
//
// Self-contained PDF command-line bridge for the DuckDB read_pdf extension.
//
// This module shells out (SAFELY, with NO /bin/sh) to the poppler/tesseract
// command-line binaries that are installed on the host:
//
//     pdftotext   pdftohtml   pdftocairo   tesseract
//
// to provide whole-document text/HTML/XML/SVG conversion plus a best-effort
// structured table reconstructor.
//
// DESIGN CONSTRAINTS (intentional):
//   * NO DuckDB headers are included here. This file is pure std C++17 + POSIX
//     so that it can be compiled and unit-tested completely standalone:
//
//         c++ -std=c++17 -DPDFCLI_STANDALONE_TEST src/pdf_cli.cpp \
//             -o /tmp/pdfcli_test && /tmp/pdfcli_test /tmp/table.pdf
//
//   * Process launching uses posix_spawnp() with an explicit argv vector. The
//     child program is invoked directly -- the user-supplied arguments are
//     NEVER concatenated into a shell command string, so there is no shell
//     metacharacter / command-injection surface.
//
// Everything lives in `namespace pdfcli`.

#ifndef PDFCLI_HPP
#define PDFCLI_HPP

#include <string>
#include <vector>

namespace pdfcli {

// ---------------------------------------------------------------------------
// Structured-table value types
// ---------------------------------------------------------------------------

// A single positioned word extracted from `pdftotext -bbox-layout` output.
// Coordinates are in the PDF user-space-ish units that poppler reports
// (origin top-left, y grows downward).
struct Word {
	double xMin = 0.0;
	double yMin = 0.0;
	double xMax = 0.0;
	double yMax = 0.0;
	std::string text;
};

// A reconstructed table cell (text only -- geometry is consumed during
// reconstruction). Kept as a named type because the DuckDB layer may want to
// extend it (e.g. with span info) without changing the grid representation.
struct Cell {
	std::string text;
};

// A reconstructed table for one page. `rows[r][c]` is the joined text of all
// words assigned to row r, column c (empty string for a blank cell).
struct Table {
	int page = 0;
	std::vector<std::vector<std::string>> rows; // rows[r][c]
};

// ---------------------------------------------------------------------------
// 1. Process runner (no shell -- injection-safe)
// ---------------------------------------------------------------------------

// Run `argv` directly via posix_spawnp (NO /bin/sh involvement).
//   * argv[0] is the program name; it is resolved against PATH by spawnp.
//   * If `stdin_data` is non-empty it is fed to the child's stdin through a
//     pipe; otherwise the child gets no stdin (its stdin fd is closed).
//   * The child's stdout is captured into `out`, stderr into `err`.
// Returns the child's exit status (0..255), or -1 if the process could not be
// spawned at all.
int RunCapture(const std::vector<std::string> &argv, const std::string &stdin_data, std::string &out, std::string &err);

// Return true if `tool` can be found (either an absolute/relative path that
// exists & is executable, or a bare name resolvable on $PATH).
bool ToolExists(const std::string &tool);

// ---------------------------------------------------------------------------
// 2. Quote-aware tokenizer for user `raw_args` passthrough
// ---------------------------------------------------------------------------

// Split `raw` on unquoted whitespace, honoring single and double quotes so
// that e.g.  -enc "UTF-8"  ->  {"-enc", "UTF-8"}. No shell metacharacter,
// variable, glob, or backslash-escape interpretation is performed beyond
// quote grouping (a backslash inside a double quote escapes the next char).
std::vector<std::string> TokenizeArgs(const std::string &raw);

// ---------------------------------------------------------------------------
// 3. Conversion utilities (whole-document output as std::string)
// ---------------------------------------------------------------------------
// Every converter appends the tokenized `raw_args` into the argv *before* the
// input path, and throws std::runtime_error (carrying captured stderr) on a
// missing tool or a non-zero exit.

// pdftotext.  layout: "reading" -> (no flag), "physical" -> -layout,
// "raw" -> -raw.  Page range via -f/-l (omit when <= 0). Always -enc UTF-8.
// Output goes to stdout via a trailing "-".
std::string PdfToText(const std::string &path, const std::string &layout, int first_page, int last_page,
                      const std::string &raw_args);

// pdftohtml -stdout -noframes (+ -s if single_doc, +-i if ignore_images).
std::string PdfToHtml(const std::string &path, bool single_doc, bool ignore_images, const std::string &raw_args);

// pdftotext -bbox-layout ... -   (XML with flow/block/line/word bboxes).
// first_page/last_page (1-based, <=0 means unbounded) are passed as explicit argv.
std::string PdfToXml(const std::string &path, int first_page, int last_page, const std::string &raw_args);

// pdftocairo -svg -f <page> -l <page> <path> -   (single-page SVG to stdout).
std::string PdfToSvg(const std::string &path, int page, const std::string &raw_args);

// ---------------------------------------------------------------------------
// 4. Structured table extraction
// ---------------------------------------------------------------------------

// Parse `pdftotext -bbox-layout` XML into a flat list of positioned words.
// `out_pages` receives the number of <page> elements seen. Each returned Word
// also carries its page index implicitly via order; ReconstructTables uses the
// page-grouped overload below, but this flat parser is exposed for callers /
// tests that just want the words. Words from page p appear contiguously.
std::vector<Word> ParseBBoxLayoutWords(const std::string &xml, int &out_pages);

// Run PdfToXml, parse words per page, and reconstruct a grid per page.
// Returns one Table per page that looks tabular (more than one detected
// column); prose pages (single column) are skipped. See pdf_cli.cpp for the
// clustering heuristics.
std::vector<Table> ReconstructTables(const std::string &path, int first_page, int last_page,
                                     const std::string &raw_args);

} // namespace pdfcli

#endif // PDFCLI_HPP
