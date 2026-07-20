#define DUCKDB_EXTENSION_MAIN

#include "pdf_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

// poppler-cpp — text extraction + page rendering + metadata
#include <poppler-document.h>
#include <poppler-embedded-file.h>
#include <poppler-page.h>
#include <poppler-page-renderer.h>
#include <poppler-image.h>
#include <poppler-toc.h>

// libharu — native PDF writer (C API) for write_pdf. No external processes.
#include <hpdf.h>

// qpdf — document-level operations (pdf_merge / pdf_split / pdf_rotate).
// Content-preserving structural transforms; no rasterization, no poppler.
// The qpdf implementation lives in its own C++17 translation unit
// (qpdf_ops.cpp) because qpdf headers require C++17 while this TU must build
// at whatever standard DuckDB drives (C++11 on the Linux registry CI) — see
// qpdf_ops.hpp for the ODR story. Only the plain-std-types interface is
// visible here; NO qpdf header may be included in this file.
#include "qpdf_ops.hpp"

// tesseract + leptonica — OCR for scanned / image-only PDFs. Isolated into
// ocr_ops.cpp (mirrors the qpdf TU isolation): this file never includes
// <tesseract/*> or <leptonica/*>. Poppler rendering stays here under
// PopplerMutex; the raw bitmap is handed to pdf_ocr::Recognize*.
#include "ocr_ops.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Process spawn + PATH probing for the runtime LibreOffice shell-out (to_pdf).
// These are the ONLY new system headers; no new library is linked.
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define popen  _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif

namespace duckdb {

//===--------------------------------------------------------------------===//
// Shared helpers
//===--------------------------------------------------------------------===//
static poppler::page::text_layout_enum LayoutFromString(const string &s, bool parse_tables) {
	if (parse_tables) {
		// physical layout preserves column alignment — the portable approximation of
		// poppler core's table mode (true cell extraction is a core-engine follow-up).
		return poppler::page::physical_layout;
	}
	auto l = StringUtil::Lower(s);
	if (l == "physical") {
		return poppler::page::physical_layout;
	}
	if (l == "raw") {
		return poppler::page::raw_order_layout;
	}
	return poppler::page::non_raw_non_physical_layout; // 'reading'
}

// poppler is not thread-safe across independent documents ON WINDOWS
// (observed: intermittent hard process crashes on Windows CI when UNION ALL
// pipelines parse/extract concurrently). Every poppler entry point below takes
// this global lock, with ONE carve-out: the parallel multi-file read_pdf scan
// on macOS/Linux, where each thread owns its own poppler::document and
// per-document page calls (create_page / page_rect / text) run unlocked.
// Document OPEN (LoadDoc) and all rasterization (page_renderer, used by OCR /
// png / svg) stay globally locked on every platform — those paths touch
// poppler global/shared state (global params, font config caches) that we
// cannot prove is per-document. Critical sections are per-call — never held
// across scan chunks or stored in state — and the mutex is recursive so
// helpers that call other guarded helpers (e.g. DocToSvg ->
// RenderPageToPngBytes) cannot self-deadlock.
static std::recursive_mutex &PopplerMutex() {
	static std::recursive_mutex m;
	return m;
}

// Compile-time platform switch for the parallel scan: Windows keeps FULL
// serialization (the CI-proven cross-document crash), POSIX runs per-document
// poppler calls from the scan lock-free. Declaring a guard is a no-op on
// POSIX; on Windows it takes the global poppler lock for the enclosing scope.
struct PopplerDocGuard {
#ifdef _WIN32
	std::lock_guard<std::recursive_mutex> lock {PopplerMutex()};
#else
	PopplerDocGuard() {
	} // user-provided ctor: a declared-but-"unused" guard never warns
#endif
};

static string UStringToUtf8(const poppler::ustring &u) {
	poppler::byte_array b = u.to_utf8();
	return string(b.begin(), b.end());
}

static string TempDir();

//===--------------------------------------------------------------------===//
// Table reconstruction (geometry over positioned words + optional ruling lines)
//
// Words come from poppler-cpp page::text_list() (or OCR). poppler-cpp does not
// expose path/graphics operators, so lattice (ruled) tables are recovered by
// parsing the page content stream with qpdf (already a hard dependency) for
// axis-aligned stroke segments and rectangles. That qpdf parse lives in the
// isolated C++17 translation unit (qpdf_ops.cpp) and returns raw RuledSegment
// values; ALL of the geometry below — including turning those segments into
// clustered rule positions — is pure std/poppler math with no qpdf types.
//
// Strategy:
//  1. LATTICE (preferred when present): vertical/horizontal stroke rules become
//     the authoritative column/row separators.
//  2. STREAM / whitespace (fallback): provisional per-row cells + a *global*
//     column model that votes on both left (xMin) and right (xMax) alignment so
//     right- / decimal-aligned numeric columns are not shattered.
//  3. MULTI-LINE CELLS: continuation rows (sparse rows continuing a previous
//     cell) are merged; the regularity gate is looser than the old "identical
//     filled-count majority" check while still rejecting body prose.
//===--------------------------------------------------------------------===//
struct PdfWord {
	double xMin = 0.0;
	double yMin = 0.0;
	double xMax = 0.0;
	double yMax = 0.0;
	string text;
};

struct RulingLines {
	std::vector<double> v_rules; // vertical rule x positions (sorted)
	std::vector<double> h_rules; // horizontal rule y positions (sorted)
	bool Usable() const {
		return v_rules.size() >= 3 && h_rules.size() >= 3; // outer box + ≥1 internal
	}
};

static double Median(std::vector<double> v) {
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	size_t m = v.size() / 2;
	if (v.size() % 2 == 1) {
		return v[m];
	}
	return 0.5 * (v[m - 1] + v[m]);
}

// Cluster 1-D positions that fall within `tol` of an existing cluster centroid.
// Returns sorted cluster centroids.
static std::vector<double> Cluster1D(std::vector<double> vals, double tol) {
	std::vector<double> centers;
	if (vals.empty()) {
		return centers;
	}
	std::sort(vals.begin(), vals.end());
	std::vector<double> bucket;
	for (double v : vals) {
		if (bucket.empty() || (v - bucket.back()) <= tol) {
			bucket.push_back(v);
		} else {
			centers.push_back(Median(bucket));
			bucket.clear();
			bucket.push_back(v);
		}
	}
	if (!bucket.empty()) {
		centers.push_back(Median(bucket));
	}
	return centers;
}

// Geometry consumer for the qpdf-collected ruling segments — no qpdf here.
// Converts raw stroke segments on one page (0-based `page_index0`) into
// clustered vertical/horizontal rule positions in poppler text_list space
// (top-down y). qpdf reports segments in PDF user space (origin bottom-left);
// horizontal rules are flipped with y' = page_height - y so they share the word
// coordinate system. Segments below a minimum length (tick marks / fragments)
// are ignored, and rules within 1.5pt are merged. This is exactly the geometry
// the old inline ExtractRulingLines performed after the qpdf content parse.
static RulingLines RulesForPage(const std::vector<pdf_qpdf::RuledSegment> &segments, int page_index0) {
	RulingLines rules;
	int target_page = page_index0 + 1;
	std::vector<double> v_raw, h_raw;
	for (const auto &seg : segments) {
		if (seg.page != target_page) {
			continue;
		}
		double x0 = seg.x0, y0 = seg.y0, x1 = seg.x1, y1 = seg.y1;
		double dx = std::fabs(x1 - x0), dy = std::fabs(y1 - y0);
		double len = std::sqrt(dx * dx + dy * dy);
		if (len < 8.0) {
			continue; // ignore tick marks / short fragments
		}
		if (dx <= 1.5 || dx * 20 < dy) {
			// vertical — x unchanged
			v_raw.push_back(0.5 * (x0 + x1));
		} else if (dy <= 1.5 || dy * 20 < dx) {
			// horizontal — flip y into poppler text_list space
			double y_pdf = 0.5 * (y0 + y1);
			h_raw.push_back(seg.page_height - y_pdf);
		}
	}
	rules.v_rules = Cluster1D(std::move(v_raw), 1.5);
	rules.h_rules = Cluster1D(std::move(h_raw), 1.5);
	return rules;
}

struct ProvCell {
	double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
	string text;
	size_t row = 0;
};

// Group words of one geometric row into provisional cells by small x-gaps.
static std::vector<ProvCell> GroupRowIntoCells(std::vector<PdfWord> row, double gap_tol, size_t row_idx) {
	std::vector<ProvCell> cells;
	if (row.empty()) {
		return cells;
	}
	std::sort(row.begin(), row.end(), [](const PdfWord &a, const PdfWord &b) { return a.xMin < b.xMin; });
	ProvCell cur;
	cur.xMin = row[0].xMin;
	cur.xMax = row[0].xMax;
	cur.yMin = row[0].yMin;
	cur.yMax = row[0].yMax;
	cur.text = row[0].text;
	cur.row = row_idx;
	for (size_t i = 1; i < row.size(); ++i) {
		double gap = row[i].xMin - cur.xMax;
		if (gap <= gap_tol) {
			if (!cur.text.empty()) {
				cur.text.push_back(' ');
			}
			cur.text += row[i].text;
			cur.xMax = std::max(cur.xMax, row[i].xMax);
			cur.xMin = std::min(cur.xMin, row[i].xMin);
			cur.yMin = std::min(cur.yMin, row[i].yMin);
			cur.yMax = std::max(cur.yMax, row[i].yMax);
		} else {
			cells.push_back(std::move(cur));
			cur = ProvCell {};
			cur.xMin = row[i].xMin;
			cur.xMax = row[i].xMax;
			cur.yMin = row[i].yMin;
			cur.yMax = row[i].yMax;
			cur.text = row[i].text;
			cur.row = row_idx;
		}
	}
	cells.push_back(std::move(cur));
	return cells;
}

// Build a global column model from provisional cells by left- AND right-edge
// recurrence across rows. Returns sorted column center x positions.
static std::vector<double> DetectColumnsGlobal(const std::vector<std::vector<ProvCell>> &row_cells, double col_tol) {
	const size_t n_rows = row_cells.size();
	if (n_rows == 0) {
		return {};
	}

	// Vote for alignment positions (xMin left, xMax right) that recur across rows.
	// Key is quantized x; value is set of row indices that voted.
	std::map<long, std::set<size_t>> left_votes;
	std::map<long, std::set<size_t>> right_votes;
	auto quant = [&](double x) -> long {
		return static_cast<long>(std::llround(x / std::max(col_tol, 0.5)));
	};

	for (size_t r = 0; r < n_rows; ++r) {
		for (const auto &c : row_cells[r]) {
			left_votes[quant(c.xMin)].insert(r);
			right_votes[quant(c.xMax)].insert(r);
		}
	}

	int min_support = std::max(2, static_cast<int>(std::ceil(0.35 * static_cast<double>(n_rows))));

	// Collect supported edge positions (de-quantize to mean of raw edges near key)
	std::vector<double> supported_edges;
	auto collect = [&](const std::map<long, std::set<size_t>> &votes, bool is_left) {
		for (const auto &kv : votes) {
			if (static_cast<int>(kv.second.size()) < min_support) {
				continue;
			}
			// average the actual edges for cells that voted this bucket
			std::vector<double> xs;
			for (size_t r : kv.second) {
				for (const auto &c : row_cells[r]) {
					long q = is_left ? quant(c.xMin) : quant(c.xMax);
					if (q == kv.first) {
						xs.push_back(is_left ? c.xMin : c.xMax);
					}
				}
			}
			if (!xs.empty()) {
				supported_edges.push_back(Median(xs));
			}
		}
	};
	collect(left_votes, true);
	collect(right_votes, false);

	// Also always include cell midpoints that co-occur in many rows (center-aligned)
	std::map<long, std::set<size_t>> mid_votes;
	for (size_t r = 0; r < n_rows; ++r) {
		for (const auto &c : row_cells[r]) {
			mid_votes[quant(0.5 * (c.xMin + c.xMax))].insert(r);
		}
	}
	for (const auto &kv : mid_votes) {
		if (static_cast<int>(kv.second.size()) < min_support) {
			continue;
		}
		std::vector<double> xs;
		for (size_t r : kv.second) {
			for (const auto &c : row_cells[r]) {
				double mid = 0.5 * (c.xMin + c.xMax);
				if (quant(mid) == kv.first) {
					xs.push_back(mid);
				}
			}
		}
		if (!xs.empty()) {
			supported_edges.push_back(Median(xs));
		}
	}

	if (supported_edges.empty()) {
		// Fallback: use modal per-row cell midpoints from rows with modal cell count
		std::map<int, int> count_hist;
		for (const auto &rc : row_cells) {
			count_hist[static_cast<int>(rc.size())]++;
		}
		int modal_n = 0, modal_c = 0;
		for (const auto &kv : count_hist) {
			if (kv.second > modal_c && kv.first >= 2) {
				modal_c = kv.second;
				modal_n = kv.first;
			}
		}
		if (modal_n < 2) {
			return {};
		}
		std::vector<std::vector<double>> col_mids(static_cast<size_t>(modal_n));
		for (const auto &rc : row_cells) {
			if (static_cast<int>(rc.size()) != modal_n) {
				continue;
			}
			for (int i = 0; i < modal_n; ++i) {
				col_mids[static_cast<size_t>(i)].push_back(
				    0.5 * (rc[static_cast<size_t>(i)].xMin + rc[static_cast<size_t>(i)].xMax));
			}
		}
		std::vector<double> cols;
		for (auto &m : col_mids) {
			if (!m.empty()) {
				cols.push_back(Median(m));
			}
		}
		return cols;
	}

	// Cluster supported edges into column anchors. Edges that are close belong
	// to the same column (left+right of a narrow column, or left-only/right-only).
	// Then derive one center per column from provisional cells matching that anchor.
	auto edge_clusters = Cluster1D(supported_edges, col_tol * 1.5);

	// Assign each provisional cell to nearest edge-cluster by min(|xMin-e|,|xMax-e|,|mid-e|).
	// Columns are the clusters that receive cells from ≥ min_support rows.
	std::vector<std::set<size_t>> cluster_rows(edge_clusters.size());
	std::vector<std::vector<double>> cluster_mids(edge_clusters.size());
	for (size_t r = 0; r < n_rows; ++r) {
		// Greedy L→R assignment so two cells in one row don't share a column.
		std::vector<bool> used(edge_clusters.size(), false);
		std::vector<ProvCell> cells = row_cells[r];
		std::sort(cells.begin(), cells.end(), [](const ProvCell &a, const ProvCell &b) { return a.xMin < b.xMin; });
		for (const auto &c : cells) {
			double mid = 0.5 * (c.xMin + c.xMax);
			size_t best = edge_clusters.size();
			double best_d = 1e300;
			for (size_t k = 0; k < edge_clusters.size(); ++k) {
				if (used[k]) {
					continue;
				}
				double e = edge_clusters[k];
				double d = std::min({std::fabs(c.xMin - e), std::fabs(c.xMax - e), std::fabs(mid - e)});
				// Prefer columns to the right as we scan L→R only when distances tie-ish
				if (d < best_d - 1e-6 ||
				    (std::fabs(d - best_d) < 1e-6 && (best == edge_clusters.size() || e > edge_clusters[best]))) {
					// actually prefer smaller e for L→R stability
					if (d < best_d) {
						best_d = d;
						best = k;
					}
				}
			}
			// Accept only if reasonably close (wide numeric columns: right edge may be far from left)
			double width = std::max(c.xMax - c.xMin, col_tol);
			if (best < edge_clusters.size() && best_d <= std::max(col_tol * 2.5, width * 0.6)) {
				used[best] = true;
				cluster_rows[best].insert(r);
				cluster_mids[best].push_back(mid);
			}
		}
	}

	std::vector<double> cols;
	for (size_t k = 0; k < edge_clusters.size(); ++k) {
		if (static_cast<int>(cluster_rows[k].size()) >= min_support && !cluster_mids[k].empty()) {
			cols.push_back(Median(cluster_mids[k]));
		}
	}
	std::sort(cols.begin(), cols.end());
	// Final merge of columns that ended up nearly identical
	return Cluster1D(cols, col_tol);
}

// Assign a provisional cell to a global column by left/right/center alignment.
static size_t AssignCellToColumn(const ProvCell &c, const std::vector<double> &col_centers, double col_tol) {
	double mid = 0.5 * (c.xMin + c.xMax);
	size_t best = 0;
	double best_score = 1e300;
	for (size_t k = 0; k < col_centers.size(); ++k) {
		double center = col_centers[k];
		// score: best of left-align, right-align, center-align distance
		double d = std::min({std::fabs(c.xMin - center), std::fabs(c.xMax - center), std::fabs(mid - center)});
		// Also score by how well the cell "covers" the column center
		if (c.xMin - col_tol <= center && center <= c.xMax + col_tol) {
			d = std::min(d, 0.25 * std::fabs(mid - center));
		}
		if (d < best_score) {
			best_score = d;
			best = k;
		}
	}
	return best;
}

// Merge sparse multi-line continuation rows into the previous row.
static void MergeContinuationRows(std::vector<std::vector<string>> &grid) {
	if (grid.size() < 2) {
		return;
	}
	std::vector<std::vector<string>> out;
	out.reserve(grid.size());
	out.push_back(grid[0]);
	for (size_t i = 1; i < grid.size(); ++i) {
		const auto &prev = out.back();
		const auto &cur = grid[i];
		if (prev.size() != cur.size() || cur.empty()) {
			out.push_back(cur);
			continue;
		}
		int filled_cur = 0, filled_prev = 0;
		int shared_nonempty = 0;
		int only_cur = 0;
		for (size_t c = 0; c < cur.size(); ++c) {
			bool pc = !prev[c].empty();
			bool cc = !cur[c].empty();
			if (pc) {
				filled_prev++;
			}
			if (cc) {
				filled_cur++;
			}
			if (pc && cc) {
				shared_nonempty++;
			}
			if (!pc && cc) {
				only_cur++;
			}
		}
		// Continuation: sparse row whose non-empty cells continue columns that
		// already have text in the previous row (wrapped multi-line cell), and
		// that does not introduce many brand-new columns.
		bool is_cont = filled_cur > 0 && filled_cur < filled_prev && shared_nonempty >= filled_cur && only_cur == 0 &&
		               filled_cur <= std::max(1, filled_prev / 2);
		if (is_cont) {
			for (size_t c = 0; c < cur.size(); ++c) {
				if (cur[c].empty()) {
					continue;
				}
				if (!out.back()[c].empty()) {
					out.back()[c].push_back(' ');
				}
				out.back()[c] += cur[c];
			}
		} else {
			out.push_back(cur);
		}
	}
	grid = std::move(out);
}

// Regularity gate: keep precision against prose while allowing sparse/merged cells.
// Returns true if the grid looks tabular.
static bool PassesTabularGate(const std::vector<std::vector<string>> &grid, bool lattice) {
	if (grid.size() < 3) {
		return false;
	}
	if (grid.front().size() < 2) {
		return false;
	}
	std::vector<int> filled_per_row;
	filled_per_row.reserve(grid.size());
	int total_filled = 0;
	for (const auto &row : grid) {
		int filled = 0;
		for (const auto &cell : row) {
			if (!cell.empty()) {
				filled++;
			}
		}
		filled_per_row.push_back(filled);
		total_filled += filled;
	}
	int modal_filled = 0;
	int modal_count = 0;
	for (size_t i = 0; i < filled_per_row.size(); ++i) {
		int c = 0;
		for (size_t j = 0; j < filled_per_row.size(); ++j) {
			if (filled_per_row[j] == filled_per_row[i]) {
				c++;
			}
		}
		if (c > modal_count) {
			modal_count = c;
			modal_filled = filled_per_row[i];
		}
	}
	// Near-modal: rows whose filled count is within 1 of modal (spanning/merged cells)
	int near_modal = 0;
	for (int f : filled_per_row) {
		if (std::abs(f - modal_filled) <= 1 && f >= 2) {
			near_modal++;
		}
	}
	double modal_fraction = static_cast<double>(modal_count) / static_cast<double>(grid.size());
	double near_fraction = static_cast<double>(near_modal) / static_cast<double>(grid.size());
	double density = static_cast<double>(total_filled) / static_cast<double>(grid.size() * grid.front().size());

	if (lattice) {
		// Rules already prove structure; only reject nearly empty grids.
		return modal_filled >= 1 && density >= 0.15 && grid.size() >= 2;
	}
	// Whitespace path: still precision-first — reject prose (one long column of
	// sentences rarely has modal_filled >= 2 with near_fraction high).
	if (modal_filled < 2) {
		return false;
	}
	// Old gate was modal_fraction >= 0.6 exact match. Allow near-modal majority
	// so multi-line / lightly sparse rows do not discard the whole table.
	if (modal_fraction >= 0.45 || near_fraction >= 0.55) {
		return density >= 0.25;
	}
	return false;
}

// Lattice reconstruction: rules define row/column bands.
static std::vector<std::vector<string>> ReconstructLatticeGrid(const std::vector<PdfWord> &page_words,
                                                               const RulingLines &rules) {
	std::vector<std::vector<string>> grid;
	if (!rules.Usable() || page_words.empty()) {
		return grid;
	}
	// Bands between consecutive rules. n rules => n-1 bands.
	const size_t ncols = rules.v_rules.size() - 1;
	const size_t nrows = rules.h_rules.size() - 1;
	if (ncols < 2 || nrows < 2) {
		return grid;
	}

	// h_rules are in poppler text_list space (top-down y, sorted ascending):
	// band 0 is the topmost row band.
	std::vector<std::vector<string>> bands(nrows, std::vector<string>(ncols));
	std::vector<std::vector<std::vector<PdfWord>>> band_words(nrows, std::vector<std::vector<PdfWord>>(ncols));

	auto col_of = [&](double x) -> int {
		for (size_t i = 0; i + 1 < rules.v_rules.size(); ++i) {
			if (x >= rules.v_rules[i] - 0.5 && x < rules.v_rules[i + 1] + 0.5) {
				return static_cast<int>(i);
			}
		}
		return -1;
	};
	auto row_of = [&](double y) -> int {
		for (size_t i = 0; i + 1 < rules.h_rules.size(); ++i) {
			if (y >= rules.h_rules[i] - 0.5 && y < rules.h_rules[i + 1] + 0.5) {
				return static_cast<int>(i);
			}
		}
		return -1;
	};

	for (const auto &w : page_words) {
		double mx = 0.5 * (w.xMin + w.xMax);
		double my = 0.5 * (w.yMin + w.yMax);
		int c = col_of(mx);
		int r = row_of(my);
		if (c < 0 || r < 0) {
			continue;
		}
		band_words[static_cast<size_t>(r)][static_cast<size_t>(c)].push_back(w);
	}

	for (size_t r = 0; r < nrows; ++r) {
		for (size_t c = 0; c < ncols; ++c) {
			auto &ws = band_words[r][c];
			if (ws.empty()) {
				continue;
			}
			// reading order within cell: top-to-bottom, then left-to-right
			std::sort(ws.begin(), ws.end(), [](const PdfWord &a, const PdfWord &b) {
				if (std::fabs(a.yMin - b.yMin) > 1.0) {
					return a.yMin < b.yMin; // smaller y first (top-down)
				}
				return a.xMin < b.xMin;
			});
			string cell;
			for (auto &w : ws) {
				if (!cell.empty()) {
					cell.push_back(' ');
				}
				cell += w.text;
			}
			bands[r][c] = std::move(cell);
		}
	}

	// Emit top-to-bottom (band 0 = top)
	for (size_t ri = 0; ri < nrows; ++ri) {
		bool any = false;
		for (const auto &cell : bands[ri]) {
			if (!cell.empty()) {
				any = true;
				break;
			}
		}
		if (any) {
			grid.push_back(bands[ri]);
		}
	}
	return grid;
}

// Reconstruct one page's words into a grid of text cells. Returns an empty grid
// for non-tabular pages (prose, single column/row, irregular cell counts).
// When `rules` is non-null and usable, lattice separators are authoritative.
static std::vector<std::vector<string>> ReconstructPageGrid(std::vector<PdfWord> page_words,
                                                            const RulingLines *rules = nullptr) {
	std::vector<std::vector<string>> grid;
	if (page_words.size() < 2) {
		return grid;
	}

	// --- lattice path ---
	if (rules && rules->Usable()) {
		grid = ReconstructLatticeGrid(page_words, *rules);
		MergeContinuationRows(grid);
		if (!PassesTabularGate(grid, /*lattice=*/true)) {
			grid.clear();
		}
		if (!grid.empty()) {
			return grid;
		}
		// fall through to whitespace if lattice produced nothing usable
	}

	// --- median line height / char width ---
	std::vector<double> heights;
	std::vector<double> widths;
	heights.reserve(page_words.size());
	for (const auto &w : page_words) {
		double h = w.yMax - w.yMin;
		if (h > 0) {
			heights.push_back(h);
		}
		double ww = w.xMax - w.xMin;
		size_t len = w.text.size();
		if (ww > 0 && len > 0) {
			widths.push_back(ww / static_cast<double>(len));
		}
	}
	double med_h = Median(heights);
	if (med_h <= 0) {
		med_h = 10.0;
	}
	double char_w = Median(widths);
	if (char_w <= 0) {
		char_w = med_h * 0.5;
	}
	double row_tol = med_h * 0.5;
	double col_tol = char_w * 1.5;
	double cell_gap_tol = char_w * 1.8; // words closer than this share a cell

	// --- cluster into rows by yMin ---
	std::sort(page_words.begin(), page_words.end(), [](const PdfWord &a, const PdfWord &b) { return a.yMin < b.yMin; });

	std::vector<std::vector<PdfWord>> rows;
	{
		std::vector<PdfWord> cur;
		double row_anchor = page_words.front().yMin;
		for (auto &w : page_words) {
			if (cur.empty()) {
				cur.push_back(w);
				row_anchor = w.yMin;
			} else if (std::fabs(w.yMin - row_anchor) <= row_tol) {
				cur.push_back(w);
			} else {
				rows.push_back(cur);
				cur.clear();
				cur.push_back(w);
				row_anchor = w.yMin;
			}
		}
		if (!cur.empty()) {
			rows.push_back(cur);
		}
	}

	if (rows.size() < 2) {
		return grid;
	}

	// poppler-cpp text_list bboxes use a top-down y axis on the pages we see
	// (smaller yMin = higher on the page), so ascending yMin is reading order.

	// --- provisional cells per row ---
	std::vector<std::vector<ProvCell>> row_cells;
	row_cells.reserve(rows.size());
	for (size_t r = 0; r < rows.size(); ++r) {
		row_cells.push_back(GroupRowIntoCells(std::move(rows[r]), cell_gap_tol, r));
	}

	// --- global column model (left + right alignment recurrence) ---
	std::vector<double> col_centers = DetectColumnsGlobal(row_cells, col_tol);
	if (col_centers.size() < 2) {
		return grid;
	}

	const size_t ncols = col_centers.size();
	for (auto &cells : row_cells) {
		std::vector<string> line(ncols);
		// Assign L→R so same-row cells never collide on one column when possible
		std::sort(cells.begin(), cells.end(), [](const ProvCell &a, const ProvCell &b) { return a.xMin < b.xMin; });
		std::vector<bool> used(ncols, false);
		for (auto &c : cells) {
			size_t best = 0;
			double best_score = 1e300;
			for (size_t k = 0; k < ncols; ++k) {
				if (used[k]) {
					continue;
				}
				// temporary un-mark: score as if free
				ProvCell tmp = c;
				// reuse AssignCellToColumn logic inline with used mask
				double mid = 0.5 * (c.xMin + c.xMax);
				double center = col_centers[k];
				double d = std::min({std::fabs(c.xMin - center), std::fabs(c.xMax - center), std::fabs(mid - center)});
				if (c.xMin - col_tol <= center && center <= c.xMax + col_tol) {
					d = std::min(d, 0.25 * std::fabs(mid - center));
				}
				if (d < best_score) {
					best_score = d;
					best = k;
				}
				(void)tmp;
			}
			// If all columns used, fall back to absolute best including collisions
			if (used[best] || best_score > col_tol * 8) {
				best = AssignCellToColumn(c, col_centers, col_tol);
			}
			used[best] = true;
			if (!line[best].empty()) {
				line[best].push_back(' ');
			}
			line[best] += c.text;
		}
		grid.push_back(std::move(line));
	}

	// --- multi-line cell merge + regularity gate ---
	MergeContinuationRows(grid);
	if (!PassesTabularGate(grid, /*lattice=*/false)) {
		grid.clear();
	}
	return grid;
}

//===--------------------------------------------------------------------===//
// OCR bridge — poppler render (this TU) + tesseract/leptonica (ocr_ops TU)
//===--------------------------------------------------------------------===//

static pdf_ocr::ImageFormat PopplerFormatToOcr(poppler::image::format_enum fmt) {
	if (fmt == poppler::image::format_rgb24) {
		return pdf_ocr::ImageFormat::RGB24;
	}
	return pdf_ocr::ImageFormat::ARGB32;
}

static pdf_ocr::Options MakeOcrOptions(const string &language, int dpi, int psm, int oem,
                                       const string &tessdata_dir, bool preprocess, bool best_effort) {
	pdf_ocr::Options o;
	o.language = language;
	o.dpi = dpi;
	o.psm = psm;
	o.oem = oem;
	o.tessdata_dir = tessdata_dir;
	o.preprocess = preprocess;
	o.best_effort = best_effort;
	return o;
}

// Render a poppler page under the global lock. Returns an invalid image on
// failure. Caller owns the returned poppler::image (value type).
static poppler::image RenderPageForOcr(poppler::page *page, int dpi) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	poppler::page_renderer renderer;
	renderer.set_render_hint(poppler::page_renderer::antialiasing, true);
	renderer.set_render_hint(poppler::page_renderer::text_antialiasing, true);
	return renderer.render_page(page, dpi, dpi);
}

static pdf_ocr::TextResult OcrBitmapText(const poppler::image &img, const pdf_ocr::Options &opt) {
	if (!img.is_valid()) {
		return pdf_ocr::TextResult();
	}
	try {
		return pdf_ocr::RecognizeText(reinterpret_cast<const unsigned char *>(img.const_data()), img.width(),
		                              img.height(), img.bytes_per_row(), PopplerFormatToOcr(img.format()), opt);
	} catch (const std::exception &e) {
		throw IOException("%s", string(e.what()));
	}
}

static pdf_ocr::WordsResult OcrBitmapWords(const poppler::image &img, const pdf_ocr::Options &opt) {
	if (!img.is_valid()) {
		return pdf_ocr::WordsResult();
	}
	try {
		return pdf_ocr::RecognizeWords(reinterpret_cast<const unsigned char *>(img.const_data()), img.width(),
		                               img.height(), img.bytes_per_row(), PopplerFormatToOcr(img.format()), opt);
	} catch (const std::exception &e) {
		throw IOException("%s", string(e.what()));
	}
}

// Word-level OCR result shape used by read_pdf_words / read_pdf_tables.
struct OcrWord {
	string text;
	double x0, y0, x1, y1;
	float confidence;
};

// Render a poppler page and OCR it with tesseract, honoring engine knobs plus
// the preprocessing (`preprocess`) and confidence-retry (`retry`) toggles. When
// retry is on and the first pass is low-confidence at a sub-400 dpi render, we
// re-render at 2x dpi and keep whichever pass scored higher.
static string OcrPage(poppler::page *page, const string &language, int dpi, int psm, int oem,
                      const string &tessdata_dir, bool preprocess, bool retry, bool best_effort) {
	auto opt = MakeOcrOptions(language, dpi, psm, oem, tessdata_dir, preprocess, best_effort);
	poppler::image img = RenderPageForOcr(page, dpi);
	pdf_ocr::TextResult first = OcrBitmapText(img, opt);
	if (retry && first.confidence < 55 && dpi < 400) {
		auto opt2 = opt;
		opt2.dpi = dpi * 2;
		poppler::image img2 = RenderPageForOcr(page, dpi * 2);
		pdf_ocr::TextResult second = OcrBitmapText(img2, opt2);
		if (second.confidence > first.confidence) {
			return second.text;
		}
	}
	return first.text;
}

// Word-level OCR with the same preprocessing + confidence-retry semantics as
// OcrPage. On retry the higher-confidence pass wins, so the returned words carry
// the confidences of whichever attempt was kept.
static std::vector<OcrWord> OcrPageWords(poppler::page *page, const string &language, int dpi, int psm, int oem,
                                         const string &tessdata_dir, bool preprocess, bool retry, bool best_effort) {
	auto opt = MakeOcrOptions(language, dpi, psm, oem, tessdata_dir, preprocess, best_effort);
	poppler::image img = RenderPageForOcr(page, dpi);
	pdf_ocr::WordsResult first = OcrBitmapWords(img, opt);
	if (retry && first.confidence < 55 && dpi < 400) {
		auto opt2 = opt;
		opt2.dpi = dpi * 2;
		poppler::image img2 = RenderPageForOcr(page, dpi * 2);
		pdf_ocr::WordsResult second = OcrBitmapWords(img2, opt2);
		if (second.confidence > first.confidence) {
			first = std::move(second);
		}
	}
	std::vector<OcrWord> out;
	out.reserve(first.words.size());
	for (auto &w : first.words) {
		OcrWord ow;
		ow.text = w.text;
		ow.x0 = w.x0;
		ow.y0 = w.y0;
		ow.x1 = w.x1;
		ow.y1 = w.y1;
		ow.confidence = w.confidence;
		out.push_back(std::move(ow));
	}
	return out;
}

//===--------------------------------------------------------------------===//
// Common options bag (shared by read_pdf / read_pdf_words)
//===--------------------------------------------------------------------===//
struct PdfOptions {
	bool force_ocr = false;
	bool auto_ocr = true;
	string ocr_language = "eng";
	int32_t ocr_dpi = 300;
	int32_t ocr_psm = 3;        // PSM_AUTO
	int32_t ocr_oem = 3;        // OEM_DEFAULT
	bool ocr_preprocess = true; // leptonica grayscale/deskew/binarize/despeckle before OCR
	bool ocr_retry = true;      // re-render low-confidence sub-400dpi pages at 2x and keep the better result
	string tessdata_dir;        // optional: directory containing <lang>.traineddata
	string layout = "reading";
	bool parse_tables = false;
	string password;
	int32_t first_page = 1;
	int32_t last_page = -1; // -1 => through end
	// Folder-scan fault isolation: when a glob matches many files, skip the ones
	// that cannot be opened (corrupt / not-a-PDF / encrypted-without-password /
	// truncated) instead of aborting the whole query on the first bad file.
	// Default false preserves the strict "one bad file throws" contract. Skipped
	// files are recoverable in SQL: glob(pattern) EXCEPT SELECT DISTINCT filename.
	bool ignore_errors = false;
};

static void ParseNamed(const named_parameter_map_t &params, PdfOptions &o) {
	for (auto &kv : params) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "ocr") {
			o.force_ocr = BooleanValue::Get(kv.second);
		} else if (key == "auto_ocr") {
			o.auto_ocr = BooleanValue::Get(kv.second);
		} else if (key == "ocr_language") {
			o.ocr_language = StringValue::Get(kv.second);
		} else if (key == "ocr_dpi") {
			o.ocr_dpi = IntegerValue::Get(kv.second);
		} else if (key == "ocr_psm") {
			o.ocr_psm = IntegerValue::Get(kv.second);
		} else if (key == "ocr_oem") {
			o.ocr_oem = IntegerValue::Get(kv.second);
		} else if (key == "ocr_preprocess") {
			o.ocr_preprocess = BooleanValue::Get(kv.second);
		} else if (key == "ocr_retry") {
			o.ocr_retry = BooleanValue::Get(kv.second);
		} else if (key == "tessdata_dir") {
			o.tessdata_dir = StringValue::Get(kv.second);
		} else if (key == "layout") {
			o.layout = StringValue::Get(kv.second);
		} else if (key == "parse_tables") {
			o.parse_tables = BooleanValue::Get(kv.second);
		} else if (key == "password") {
			o.password = StringValue::Get(kv.second);
		} else if (key == "first_page") {
			o.first_page = IntegerValue::Get(kv.second);
		} else if (key == "last_page") {
			o.last_page = IntegerValue::Get(kv.second);
		} else if (key == "ignore_errors") {
			o.ignore_errors = BooleanValue::Get(kv.second);
		}
	}

	// Validate up front so bad input fails with a clear message instead of
	// silently returning zero rows or feeding out-of-range values to the
	// rendering / OCR engines (a huge dpi can OOM; an out-of-range psm/oem is
	// cast into an undefined enum).
	if (o.ocr_dpi < 1 || o.ocr_dpi > 2400) {
		throw InvalidInputException("read_pdf: ocr_dpi must be between 1 and 2400 (got %d)", o.ocr_dpi);
	}
	if (o.ocr_psm < 0 || o.ocr_psm > 13) {
		throw InvalidInputException("read_pdf: ocr_psm must be between 0 and 13 (got %d)", o.ocr_psm);
	}
	if (o.ocr_oem < 0 || o.ocr_oem > 3) {
		throw InvalidInputException("read_pdf: ocr_oem must be between 0 and 3 (got %d)", o.ocr_oem);
	}
	if (o.first_page < 1) {
		throw InvalidInputException("read_pdf: first_page must be >= 1 (got %d)", o.first_page);
	}
	if (o.last_page == 0 || o.last_page < -1) {
		throw InvalidInputException("read_pdf: last_page must be >= 1 (got %d)", o.last_page);
	}
	if (o.last_page > 0 && o.last_page < o.first_page) {
		throw InvalidInputException("read_pdf: last_page (%d) must be >= first_page (%d)", o.last_page, o.first_page);
	}
}

static void AddCommonNamedParams(TableFunction &fn) {
	fn.named_parameters["ocr"] = LogicalType::BOOLEAN;
	fn.named_parameters["auto_ocr"] = LogicalType::BOOLEAN;
	fn.named_parameters["ocr_language"] = LogicalType::VARCHAR;
	fn.named_parameters["ocr_dpi"] = LogicalType::INTEGER;
	fn.named_parameters["ocr_psm"] = LogicalType::INTEGER;
	fn.named_parameters["ocr_oem"] = LogicalType::INTEGER;
	fn.named_parameters["ocr_preprocess"] = LogicalType::BOOLEAN;
	fn.named_parameters["ocr_retry"] = LogicalType::BOOLEAN;
	fn.named_parameters["tessdata_dir"] = LogicalType::VARCHAR;
	fn.named_parameters["layout"] = LogicalType::VARCHAR;
	fn.named_parameters["parse_tables"] = LogicalType::BOOLEAN;
	fn.named_parameters["password"] = LogicalType::VARCHAR;
	fn.named_parameters["first_page"] = LogicalType::INTEGER;
	fn.named_parameters["last_page"] = LogicalType::INTEGER;
}

static vector<string> ResolveFiles(ClientContext &context, const string &pattern) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto globbed = fs.GlobFiles(pattern, FileGlobOptions::ALLOW_EMPTY);
	vector<string> files;
	if (globbed.empty()) {
		files.push_back(pattern);
	} else {
		for (auto &f : globbed) {
			files.push_back(f.path);
		}
	}
	return files;
}

// Read all bytes of a path through DuckDB's FileSystem (works with any fs).
static void ReadAllBytes(ClientContext &context, const string &path, string &out) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	int64_t size = fs.GetFileSize(*handle);
	out.resize(size);
	// loop until fully read — fs.Read may return a short count (P0-4)
	int64_t total = 0;
	while (total < size) {
		int64_t n = fs.Read(*handle, &out[total], size - total);
		if (n <= 0) {
			break;
		}
		total += n;
	}
	if (total != size) {
		throw IOException("read_pdf: short read on '%s' (%lld of %lld bytes)", path, (long long)total, (long long)size);
	}
}

static unique_ptr<poppler::document> LoadDoc(const string &bytes, const string &password, const string &path) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	// poppler's load_from_raw_data takes an int length; guard against silent
	// truncation (and the corresponding garbage read) for >2 GiB inputs.
	if (bytes.size() > (size_t)NumericLimits<int>::Maximum()) {
		throw IOException("read_pdf: '%s' is too large (%lld bytes; max ~2 GiB)", path, (long long)bytes.size());
	}
	auto doc = unique_ptr<poppler::document>(
	    poppler::document::load_from_raw_data(bytes.data(), (int)bytes.size(), password, password));
	if (!doc) {
		throw IOException("read_pdf: could not open '%s' (corrupt or not a PDF)", path);
	}
	if (doc->is_locked()) {
		throw IOException("read_pdf: '%s' is encrypted; supply the correct password via password := '...'", path);
	}
	if (doc->pages() <= 0) {
		throw IOException("read_pdf: '%s' has no readable pages (empty or unreadable document)", path);
	}
	return doc;
}

//===--------------------------------------------------------------------===//
// read_pdf  -> (file, page, page_count, text)
//===--------------------------------------------------------------------===//
struct ReadPdfBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};

// Parallel multi-file scan: the global state only hands out file indices; every
// worker thread owns its OWN poppler::document in its local state, so no
// document is ever touched by two threads.
struct ReadPdfGlobalState : public GlobalTableFunctionState {
	explicit ReadPdfGlobalState(idx_t max_threads_p) : max_threads(max_threads_p) {
	}
	std::atomic<idx_t> next_file_idx {0};
	const idx_t max_threads;
	idx_t MaxThreads() const override {
		return max_threads;
	}
};

struct ReadPdfLocalState : public LocalTableFunctionState {
	int page_idx = 0; // 0-based
	int page_count = 0;
	int last_page_0 = 0; // exclusive upper bound (0-based)
	string file_bytes;
	unique_ptr<poppler::document> doc; // owned by THIS thread only
	string current_file;
};

static unique_ptr<FunctionData> ReadPdfBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);

	// has_text_layer: true when the native embedded text layer is non-blank.
	// used_ocr: true when OCR produced (or replaced) the returned text.
	// Together they make image-only vs embedded-text detection first-class without
	// a second pass over the file.
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::BOOLEAN, LogicalType::BOOLEAN};
	names = {"filename", "page", "page_count", "text", "width", "height", "has_text_layer", "used_ocr"};
	return std::move(result);
}

// Claim the next unscanned file for this thread and open it into LOCAL state.
// Returns false when the file list is exhausted. Document open stays under the
// global poppler lock on ALL platforms — LoadDoc itself takes PopplerMutex —
// because load_from_raw_data walks shared font-config/global-params state we
// cannot prove is per-document.
static bool ClaimNextFile(ClientContext &context, const ReadPdfBindData &bind, ReadPdfGlobalState &g,
                          ReadPdfLocalState &l) {
	// Loop so that, under ignore_errors, an unopenable file is skipped and this
	// thread simply claims the next index rather than returning failure.
	for (;;) {
		auto file_idx = g.next_file_idx.fetch_add(1, std::memory_order_relaxed);
		if (file_idx >= bind.files.size()) {
			l.doc.reset();
			return false;
		}
		l.current_file = bind.files[file_idx];
		try {
			ReadAllBytes(context, l.current_file, l.file_bytes);
			l.doc = LoadDoc(l.file_bytes, bind.opt.password, l.current_file);
		} catch (const std::exception &) {
			// Strict by default; opt in to skipping bad files with ignore_errors.
			if (!bind.opt.ignore_errors) {
				throw;
			}
			continue; // skip this file, claim the next
		}
		l.page_count = l.doc->pages();
		l.page_idx = bind.opt.first_page > 0 ? bind.opt.first_page - 1 : 0;
		l.last_page_0 = bind.opt.last_page < 0 ? l.page_count : MinValue<int>(bind.opt.last_page, l.page_count);
		return true;
	}
}

static unique_ptr<GlobalTableFunctionState> ReadPdfInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadPdfBindData>();
#ifdef _WIN32
	// Windows: poppler cross-document use crashes the process (CI-proven);
	// keep the scan fully serial there.
	idx_t max_threads = 1;
	(void)bind;
	(void)context;
#else
	auto scheduler_threads = (idx_t)TaskScheduler::GetScheduler(context).NumberOfThreads();
	idx_t max_threads = MaxValue<idx_t>(1, MinValue<idx_t>((idx_t)bind.files.size(), scheduler_threads));
#endif
	return make_uniq<ReadPdfGlobalState>(max_threads);
}

static unique_ptr<LocalTableFunctionState> ReadPdfInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                            GlobalTableFunctionState *gstate) {
	return make_uniq<ReadPdfLocalState>();
}

static void ReadPdfScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadPdfBindData>();
	auto &g = data_p.global_state->Cast<ReadPdfGlobalState>();
	auto &l = data_p.local_state->Cast<ReadPdfLocalState>();
	auto layout = LayoutFromString(bind.opt.layout, bind.opt.parse_tables);

	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		while (!l.doc || l.page_idx >= l.last_page_0) {
			if (!ClaimNextFile(context, bind, g, l)) {
				break;
			}
		}
		if (!l.doc) {
			break;
		}

		string text;
		double width = 0.0;
		double height = 0.0;
		bool has_text_layer = false;
		bool used_ocr = false;
		{
			// Per-document page work: lock-free on POSIX (this thread owns
			// l.doc), globally locked on Windows via the guard. OCR inside is
			// safe: OcrPage takes PopplerMutex itself (rasterization touches
			// poppler globals) and the mutex is recursive.
			PopplerDocGuard poppler_guard;
			unique_ptr<poppler::page> page(l.doc->create_page(l.page_idx));
			if (page) {
				auto rect = page->page_rect();
				width = rect.width();
				height = rect.height();
				// Probe the native text layer even under force_ocr so the
				// has_text_layer flag always reflects the PDF itself, not the
				// extraction path chosen by the caller.
				string native = UStringToUtf8(page->text(poppler::rectf(), layout));
				has_text_layer = native.find_first_not_of(" \t\r\n\f\v") != string::npos;
				text = native;
				bool blank = !has_text_layer;
				if (bind.opt.force_ocr || (bind.opt.auto_ocr && blank)) {
					// best_effort under force_ocr too when the page has a native
					// layer: a blank raster (missing display fonts on a text PDF)
					// must not discard readable embedded text. Image-only pages
					// still get the loud missing-model error under force_ocr.
					const bool best_effort = !bind.opt.force_ocr || has_text_layer;
					string ocr =
					    OcrPage(page.get(), bind.opt.ocr_language, bind.opt.ocr_dpi, bind.opt.ocr_psm, bind.opt.ocr_oem,
					            bind.opt.tessdata_dir, bind.opt.ocr_preprocess, bind.opt.ocr_retry, best_effort);
					if (!ocr.empty()) {
						text = ocr;
						used_ocr = true;
					}
					// else: keep native (possibly empty on image-only pages)
				}
			}
		}

		output.SetValue(0, count, Value(l.current_file));
		output.SetValue(1, count, Value::INTEGER(l.page_idx + 1));
		output.SetValue(2, count, Value::INTEGER(l.page_count));
		output.SetValue(3, count, Value(text));
		output.SetValue(4, count, Value::DOUBLE(width));
		output.SetValue(5, count, Value::DOUBLE(height));
		output.SetValue(6, count, Value::BOOLEAN(has_text_layer));
		output.SetValue(7, count, Value::BOOLEAN(used_ocr));
		l.page_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_meta  -> one row per file
//===--------------------------------------------------------------------===//
struct ReadPdfMetaBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};
struct ReadPdfMetaState : public GlobalTableFunctionState {
	idx_t idx = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadPdfMetaBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfMetaBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::BOOLEAN};
	names = {"filename", "title",    "author", "subject",     "keywords",
	         "creator",  "producer", "pages",  "pdf_version", "encrypted"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ReadPdfMetaInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ReadPdfMetaState>();
}

static void ReadPdfMetaScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	auto &bind = data_p.bind_data->Cast<ReadPdfMetaBindData>();
	auto &st = data_p.global_state->Cast<ReadPdfMetaState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.idx < bind.files.size()) {
		auto &path = bind.files[st.idx];
		string bytes;
		unique_ptr<poppler::document> doc;
		try {
			ReadAllBytes(context, path, bytes);
			doc = LoadDoc(bytes, bind.opt.password, path);
		} catch (const std::exception &) {
			// Strict by default; opt in to skipping bad files with ignore_errors.
			if (!bind.opt.ignore_errors) {
				throw;
			}
			st.idx++;
			continue; // skip this file, advance to the next
		}

		int major = 0, minor = 0;
		doc->get_pdf_version(&major, &minor);

		output.SetValue(0, count, Value(path));
		output.SetValue(1, count, Value(UStringToUtf8(doc->info_key("Title"))));
		output.SetValue(2, count, Value(UStringToUtf8(doc->info_key("Author"))));
		output.SetValue(3, count, Value(UStringToUtf8(doc->info_key("Subject"))));
		output.SetValue(4, count, Value(UStringToUtf8(doc->info_key("Keywords"))));
		output.SetValue(5, count, Value(UStringToUtf8(doc->info_key("Creator"))));
		output.SetValue(6, count, Value(UStringToUtf8(doc->info_key("Producer"))));
		output.SetValue(7, count, Value::INTEGER(doc->pages()));
		output.SetValue(8, count, Value(to_string(major) + "." + to_string(minor)));
		output.SetValue(9, count, Value::BOOLEAN(doc->is_encrypted()));
		st.idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// Inspection suite: pdf_info / pdf_outline / pdf_attachments
//
// Read-only "folder census" functions. All three take the same files argument
// as read_pdf (single path or glob), run serially per file, and follow the
// same failure contract as read_pdf_meta: a corrupt/missing/locked file
// throws (never silently yields zero rows). Poppler work stays inside the
// per-call PopplerMutex critical section.
//===--------------------------------------------------------------------===//
struct PdfInspectBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt; // only `password` is honored here
};

// Shared bind: resolve the file list and parse the password named parameter.
static unique_ptr<FunctionData> PdfInspectBindCommon(ClientContext &context, TableFunctionBindInput &input) {
	auto result = make_uniq<PdfInspectBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return std::move(result);
}

// Empty/missing metadata strings surface as NULL, never as ''.
static Value InspectStringOrNull(const string &s) {
	if (s.empty()) {
		return Value(LogicalType::VARCHAR);
	}
	return Value(s);
}

// poppler reports unset dates as <= 0; those surface as NULL.
static Value InspectDateOrNull(time_t t) {
	if (t <= 0) {
		return Value(LogicalType::TIMESTAMP);
	}
	return Value::TIMESTAMP(Timestamp::FromEpochSeconds((int64_t)t));
}

// Trim leading/trailing ASCII whitespace from an XMP-extracted value.
static string TrimXmpValue(const string &s) {
	size_t a = 0, b = s.size();
	while (a < b && isspace((unsigned char)s[a])) {
		a++;
	}
	while (b > a && isspace((unsigned char)s[b - 1])) {
		b--;
	}
	return s.substr(a, b - a);
}

// Detection-only scan of an XMP metadata packet for a pdfaid:<key> value.
// Handles both the attribute form (pdfaid:part="2") and the element form
// (<pdfaid:part>2</pdfaid:part>) that both occur in the wild. Returns the
// trimmed value, or an empty string when the key is absent. No validation and
// no XML library — a corrupt packet simply yields no match.
static string ExtractPdfaId(const string &xmp, const string &key) {
	const string needle = "pdfaid:" + key;
	size_t pos = 0;
	while ((pos = xmp.find(needle, pos)) != string::npos) {
		// Skip closing tags like </pdfaid:part>.
		if (pos > 0 && xmp[pos - 1] == '/') {
			pos += needle.size();
			continue;
		}
		size_t p = pos + needle.size();
		while (p < xmp.size() && isspace((unsigned char)xmp[p])) {
			p++;
		}
		if (p < xmp.size() && xmp[p] == '=') {
			// Attribute form: pdfaid:key="value" or pdfaid:key='value'.
			p++;
			while (p < xmp.size() && isspace((unsigned char)xmp[p])) {
				p++;
			}
			if (p < xmp.size() && (xmp[p] == '"' || xmp[p] == '\'')) {
				char quote = xmp[p++];
				size_t end = xmp.find(quote, p);
				if (end != string::npos) {
					return TrimXmpValue(xmp.substr(p, end - p));
				}
			}
		} else if (p < xmp.size() && xmp[p] == '>') {
			// Element form: <pdfaid:key>value</pdfaid:key>.
			p++;
			size_t end = xmp.find('<', p);
			if (end != string::npos) {
				return TrimXmpValue(xmp.substr(p, end - p));
			}
		}
		pos += needle.size();
	}
	return string();
}

//===--------------------------------------------------------------------===//
// pdf_info  -> one row per file (identity + geometry + storage census)
//===--------------------------------------------------------------------===//
struct PdfInfoState : public GlobalTableFunctionState {
	idx_t idx = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PdfInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP,
	                LogicalType::TIMESTAMP, LogicalType::INTEGER, LogicalType::BOOLEAN, LogicalType::BOOLEAN,
	                LogicalType::VARCHAR,   LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::BIGINT,
	                LogicalType::INTEGER,   LogicalType::VARCHAR};
	names = {"file",        "title",         "author",   "subject",    "keywords",     "creator",
	         "producer",    "creation_date", "mod_date", "page_count", "is_encrypted", "is_linearized",
	         "pdf_version", "width",         "height",   "file_size",  "pdfa_part",    "pdfa_conformance"};
	return PdfInspectBindCommon(context, input);
}

static unique_ptr<GlobalTableFunctionState> PdfInfoInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PdfInfoState>();
}

static void PdfInfoScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	auto &bind = data_p.bind_data->Cast<PdfInspectBindData>();
	auto &st = data_p.global_state->Cast<PdfInfoState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.idx < bind.files.size()) {
		auto &path = bind.files[st.idx];
		string bytes;
		ReadAllBytes(context, path, bytes);
		auto doc = LoadDoc(bytes, bind.opt.password, path);

		int major = 0, minor = 0;
		doc->get_pdf_version(&major, &minor);

		// first-page media box, in points; NULL if the page will not open
		Value width_val(LogicalType::DOUBLE);
		Value height_val(LogicalType::DOUBLE);
		unique_ptr<poppler::page> first_page(doc->create_page(0));
		if (first_page) {
			auto rect = first_page->page_rect(poppler::media_box);
			width_val = Value::DOUBLE(rect.width());
			height_val = Value::DOUBLE(rect.height());
		}

		// PDF/A identification from the XMP metadata packet (detection only,
		// no validation): pdfaid:part (INTEGER) and pdfaid:conformance (A/B/U).
		Value pdfa_part_val(LogicalType::INTEGER);
		Value pdfa_conformance_val(LogicalType::VARCHAR);
		{
			string xmp = UStringToUtf8(doc->metadata());
			if (!xmp.empty()) {
				string part_str = ExtractPdfaId(xmp, "part");
				if (!part_str.empty()) {
					try {
						size_t consumed = 0;
						int part = std::stoi(part_str, &consumed);
						if (consumed == part_str.size()) {
							pdfa_part_val = Value::INTEGER(part);
						}
					} catch (const std::exception &) {
						// non-numeric part -> leave NULL
					}
				}
				string conformance = ExtractPdfaId(xmp, "conformance");
				if (!conformance.empty()) {
					pdfa_conformance_val = Value(conformance);
				}
			}
		}

		output.SetValue(0, count, Value(path));
		output.SetValue(1, count, InspectStringOrNull(UStringToUtf8(doc->info_key("Title"))));
		output.SetValue(2, count, InspectStringOrNull(UStringToUtf8(doc->info_key("Author"))));
		output.SetValue(3, count, InspectStringOrNull(UStringToUtf8(doc->info_key("Subject"))));
		output.SetValue(4, count, InspectStringOrNull(UStringToUtf8(doc->info_key("Keywords"))));
		output.SetValue(5, count, InspectStringOrNull(UStringToUtf8(doc->info_key("Creator"))));
		output.SetValue(6, count, InspectStringOrNull(UStringToUtf8(doc->info_key("Producer"))));
		output.SetValue(7, count, InspectDateOrNull(doc->info_date_t("CreationDate")));
		output.SetValue(8, count, InspectDateOrNull(doc->info_date_t("ModDate")));
		output.SetValue(9, count, Value::INTEGER(doc->pages()));
		output.SetValue(10, count, Value::BOOLEAN(doc->is_encrypted()));
		output.SetValue(11, count, Value::BOOLEAN(doc->is_linearized()));
		output.SetValue(12, count, Value(to_string(major) + "." + to_string(minor)));
		output.SetValue(13, count, width_val);
		output.SetValue(14, count, height_val);
		output.SetValue(15, count, Value::BIGINT((int64_t)bytes.size()));
		output.SetValue(16, count, pdfa_part_val);
		output.SetValue(17, count, pdfa_conformance_val);
		st.idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// pdf_outline  -> one row per bookmark (depth-first document order)
//===--------------------------------------------------------------------===//
struct PdfOutlineRow {
	int ord = 0;   // 1-based, depth-first document order
	int depth = 0; // 1 = top level
	string title;
};

// Depth-first walk over the toc tree. poppler's root() is a synthetic node;
// its children are the visible top-level bookmarks (depth 1).
static void OutlineWalk(const poppler::toc_item *item, int depth, int &ord, std::vector<PdfOutlineRow> &rows) {
	for (auto child : item->children()) {
		PdfOutlineRow row;
		row.ord = ++ord;
		row.depth = depth;
		row.title = UStringToUtf8(child->title());
		rows.push_back(std::move(row));
		OutlineWalk(child, depth + 1, ord, rows);
	}
}

struct PdfOutlineState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	idx_t row_idx = 0;
	string current_file;
	std::vector<PdfOutlineRow> rows; // materialized per file; doc is not kept
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PdfOutlineBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR};
	names = {"file", "ord", "depth", "title"};
	return PdfInspectBindCommon(context, input);
}

static unique_ptr<GlobalTableFunctionState> PdfOutlineInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PdfOutlineState>();
}

static void PdfOutlineScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	auto &bind = data_p.bind_data->Cast<PdfInspectBindData>();
	auto &st = data_p.global_state->Cast<PdfOutlineState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (st.row_idx >= st.rows.size()) {
			// advance to the next file; files without an outline yield no rows
			if (st.file_idx >= bind.files.size()) {
				break;
			}
			st.rows.clear();
			st.row_idx = 0;
			st.current_file = bind.files[st.file_idx++];
			string bytes;
			ReadAllBytes(context, st.current_file, bytes);
			auto doc = LoadDoc(bytes, bind.opt.password, st.current_file);
			unique_ptr<poppler::toc> toc(doc->create_toc());
			if (toc && toc->root()) {
				int ord = 0;
				OutlineWalk(toc->root(), 1, ord, st.rows);
			}
			continue;
		}
		auto &row = st.rows[st.row_idx];
		output.SetValue(0, count, Value(st.current_file));
		output.SetValue(1, count, Value::INTEGER(row.ord));
		output.SetValue(2, count, Value::INTEGER(row.depth));
		output.SetValue(3, count, Value(row.title));
		st.row_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// pdf_attachments  -> one row per embedded file
//===--------------------------------------------------------------------===//
struct PdfAttachmentRow {
	string name;
	string description;
	int64_t size = -1; // poppler reports unknown size as < 0 -> NULL
	string mime_type;
	string data;
};

struct PdfAttachmentsState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	idx_t row_idx = 0;
	string current_file;
	std::vector<PdfAttachmentRow> rows; // materialized per file; doc is not kept
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PdfAttachmentsBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::BLOB};
	names = {"file", "name", "description", "size", "mime_type", "data"};
	return PdfInspectBindCommon(context, input);
}

static unique_ptr<GlobalTableFunctionState> PdfAttachmentsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PdfAttachmentsState>();
}

static void PdfAttachmentsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	auto &bind = data_p.bind_data->Cast<PdfInspectBindData>();
	auto &st = data_p.global_state->Cast<PdfAttachmentsState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (st.row_idx >= st.rows.size()) {
			// advance to the next file; files without attachments yield no rows
			if (st.file_idx >= bind.files.size()) {
				break;
			}
			st.rows.clear();
			st.row_idx = 0;
			st.current_file = bind.files[st.file_idx++];
			string bytes;
			ReadAllBytes(context, st.current_file, bytes);
			auto doc = LoadDoc(bytes, bind.opt.password, st.current_file);
			// embedded_file objects are owned by the document — copy everything
			// out while the document is alive.
			for (auto attachment : doc->embedded_files()) {
				if (!attachment || !attachment->is_valid()) {
					continue;
				}
				PdfAttachmentRow row;
				// name() (not unicodeName()): CI's poppler predates unicodeName, and
				// name() exists across all pinned poppler versions.
				row.name = attachment->name();
				row.description = UStringToUtf8(attachment->description());
				row.size = (int64_t)attachment->size();
				row.mime_type = attachment->mime_type();
				auto payload = attachment->data();
				row.data = string(payload.begin(), payload.end());
				st.rows.push_back(std::move(row));
			}
			continue;
		}
		auto &row = st.rows[st.row_idx];
		output.SetValue(0, count, Value(st.current_file));
		output.SetValue(1, count, InspectStringOrNull(row.name));
		output.SetValue(2, count, InspectStringOrNull(row.description));
		output.SetValue(3, count, row.size < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(row.size));
		output.SetValue(4, count, InspectStringOrNull(row.mime_type));
		output.SetValue(5, count, Value::BLOB(const_data_ptr_cast(row.data.data()), row.data.size()));
		st.row_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// pdf_revisions -> one row per PDF revision (oldest first)
//
// PDFs support incremental updates: editors append a new xref + trailer +
// %%EOF rather than rewriting the file. Trailers chain via /Prev (byte offset
// of the previous xref). Each chain entry is one "revision"; revision 0 is the
// original document, later indexes are appended updates.
//
// Implemented by scanning the raw bytes for startxref/%%EOF sections and
// walking the trailer /Prev chain. qpdf's QPDF::processFile merges revisions
// into one logical document (getXRefTable is the final table only), so it does
// not expose a clean multi-revision enumeration API — byte-level parsing is
// the right tool for this forensic signal. Handles classic xref tables and
// PDF 1.5+ xref streams (/Type /XRef). Password is accepted for API parity
// with the rest of the inspection suite (trailers are plaintext even when
// streams are encrypted).
//===--------------------------------------------------------------------===//
struct PdfRevisionRow {
	int32_t revision_index = 0;
	int64_t startxref_offset = 0;
	int64_t eof_offset = 0;
	int64_t size_bytes = 0;
	bool is_incremental = false;
};

struct PdfStartxrefSection {
	int64_t xref_offset = 0; // value after the startxref keyword
	int64_t eof_offset = 0;  // byte offset just past this section's %%EOF
	size_t kw_pos = 0;       // offset of the "startxref" keyword
};

static bool PdfIsAsciiSpace(unsigned char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\0';
}

// Locate every startxref <offset> %%EOF section in document order.
static vector<PdfStartxrefSection> PdfFindStartxrefSections(const string &bytes) {
	vector<PdfStartxrefSection> out;
	const string needle = "startxref";
	size_t pos = 0;
	while (pos < bytes.size()) {
		size_t found = bytes.find(needle, pos);
		if (found == string::npos) {
			break;
		}
		// Keyword boundary: start of buffer or preceding whitespace.
		if (found > 0 && !PdfIsAsciiSpace((unsigned char)bytes[found - 1])) {
			pos = found + 1;
			continue;
		}
		size_t i = found + needle.size();
		while (i < bytes.size() && PdfIsAsciiSpace((unsigned char)bytes[i])) {
			i++;
		}
		if (i >= bytes.size() || !std::isdigit((unsigned char)bytes[i])) {
			pos = found + 1;
			continue;
		}
		int64_t xref_off = 0;
		while (i < bytes.size() && std::isdigit((unsigned char)bytes[i])) {
			xref_off = xref_off * 10 + (bytes[i] - '0');
			i++;
		}
		while (i < bytes.size() && PdfIsAsciiSpace((unsigned char)bytes[i])) {
			i++;
		}
		if (i + 5 > bytes.size() || bytes.compare(i, 5, "%%EOF") != 0) {
			pos = found + 1;
			continue;
		}
		i += 5;
		// Optional single CRLF / LF after %%EOF is still part of this revision.
		if (i < bytes.size() && bytes[i] == '\r') {
			i++;
		}
		if (i < bytes.size() && bytes[i] == '\n') {
			i++;
		}
		PdfStartxrefSection sec;
		sec.xref_offset = xref_off;
		sec.eof_offset = (int64_t)i;
		sec.kw_pos = found;
		out.push_back(sec);
		pos = i;
	}
	return out;
}

// Scan a trailer-like dictionary byte range for `/Prev <int>`.
// Returns true when /Prev is present.
static bool PdfDictGetPrev(const string &bytes, size_t dict_start, size_t dict_end, int64_t &prev_out) {
	if (dict_end <= dict_start || dict_end > bytes.size()) {
		return false;
	}
	const string region = bytes.substr(dict_start, dict_end - dict_start);
	size_t p = 0;
	while (p < region.size()) {
		size_t found = region.find("/Prev", p);
		if (found == string::npos) {
			return false;
		}
		// Ensure "/Prev" is a name token (not /Previously...).
		size_t after = found + 5;
		if (after < region.size()) {
			unsigned char c = (unsigned char)region[after];
			// PDF name continues with regular characters; whitespace or delimiter ends it.
			if (!PdfIsAsciiSpace(c) && c != '/' && c != '[' && c != ']' && c != '<' && c != '>' && c != '(' &&
			    c != ')' && c != '{' && c != '}' && c != '%') {
				p = found + 1;
				continue;
			}
		}
		size_t i = after;
		while (i < region.size() && PdfIsAsciiSpace((unsigned char)region[i])) {
			i++;
		}
		if (i >= region.size() || !std::isdigit((unsigned char)region[i])) {
			p = found + 1;
			continue;
		}
		int64_t v = 0;
		while (i < region.size() && std::isdigit((unsigned char)region[i])) {
			v = v * 10 + (region[i] - '0');
			i++;
		}
		prev_out = v;
		return true;
	}
	return false;
}

// Given a startxref offset, locate the trailer dictionary (classic) or the
// xref-stream dictionary (PDF 1.5+) and read /Prev when present.
static bool PdfReadPrevAtXref(const string &bytes, int64_t xref_offset, int64_t &prev_out) {
	if (xref_offset < 0 || (size_t)xref_offset >= bytes.size()) {
		return false;
	}
	const size_t off = (size_t)xref_offset;

	// Classic cross-reference table: "xref" ... "trailer" << ... >>
	if (off + 4 <= bytes.size() && bytes.compare(off, 4, "xref") == 0) {
		// "xref" must be a keyword (end boundary).
		bool classic_ok = (off + 4 >= bytes.size()) || PdfIsAsciiSpace((unsigned char)bytes[off + 4]);
		if (classic_ok) {
			size_t search_from = off + 4;
			// Stay inside a reasonable window before the matching startxref keyword.
			size_t search_limit = std::min(bytes.size(), off + 2 * 1024 * 1024);
			while (search_from < search_limit) {
				size_t tpos = bytes.find("trailer", search_from);
				if (tpos == string::npos || tpos >= search_limit) {
					break;
				}
				if (tpos > 0 && !PdfIsAsciiSpace((unsigned char)bytes[tpos - 1])) {
					search_from = tpos + 1;
					continue;
				}
				size_t after = tpos + 7;
				if (after < bytes.size() && !PdfIsAsciiSpace((unsigned char)bytes[after]) && bytes[after] != '<') {
					search_from = tpos + 1;
					continue;
				}
				size_t d0 = bytes.find("<<", tpos);
				if (d0 == string::npos || d0 >= search_limit) {
					break;
				}
				// Match nested dict depth for the trailer dictionary.
				size_t i = d0;
				int depth = 0;
				size_t d1 = string::npos;
				while (i + 1 < bytes.size() && i < search_limit) {
					if (bytes[i] == '<' && bytes[i + 1] == '<') {
						depth++;
						i += 2;
						continue;
					}
					if (bytes[i] == '>' && bytes[i + 1] == '>') {
						depth--;
						i += 2;
						if (depth == 0) {
							d1 = i;
							break;
						}
						continue;
					}
					i++;
				}
				if (d1 == string::npos) {
					search_from = tpos + 1;
					continue;
				}
				return PdfDictGetPrev(bytes, d0, d1, prev_out);
			}
			return false;
		}
	}

	// XRef stream: object dictionary with /Type /XRef and optional /Prev.
	size_t search_limit = std::min(bytes.size(), off + 256 * 1024);
	size_t d0 = bytes.find("<<", off);
	if (d0 == string::npos || d0 >= search_limit) {
		return false;
	}
	size_t i = d0;
	int depth = 0;
	size_t d1 = string::npos;
	while (i + 1 < bytes.size() && i < search_limit) {
		if (bytes[i] == '<' && bytes[i + 1] == '<') {
			depth++;
			i += 2;
			continue;
		}
		if (bytes[i] == '>' && bytes[i + 1] == '>') {
			depth--;
			i += 2;
			if (depth == 0) {
				d1 = i;
				break;
			}
			continue;
		}
		i++;
	}
	if (d1 == string::npos) {
		return false;
	}
	// Prefer dictionaries that look like xref streams (/Type /XRef), but still
	// accept /Prev from the first dict at this offset when Type is omitted.
	const string dict = bytes.substr(d0, d1 - d0);
	if (dict.find("/XRef") == string::npos && dict.find("/Prev") == string::npos) {
		return false;
	}
	return PdfDictGetPrev(bytes, d0, d1, prev_out);
}

// Enumerate revisions oldest-first by walking the trailer /Prev chain from the
// last startxref section. Falls back to every startxref/%%EOF pair in file
// order when the chain cannot be reconstructed.
static vector<PdfRevisionRow> EnumeratePdfRevisions(const string &bytes) {
	vector<PdfRevisionRow> rows;
	auto sections = PdfFindStartxrefSections(bytes);
	if (sections.empty()) {
		return rows;
	}

	// Map xref offset -> section (last occurrence wins — matches the final view).
	std::map<int64_t, PdfStartxrefSection> by_xref;
	for (auto &s : sections) {
		by_xref[s.xref_offset] = s;
	}

	// Walk /Prev from the last section in the file.
	vector<PdfStartxrefSection> chain;
	std::set<int64_t> seen;
	int64_t cur = sections.back().xref_offset;
	while (true) {
		if (seen.count(cur)) {
			break; // cycle guard
		}
		seen.insert(cur);
		auto it = by_xref.find(cur);
		if (it == by_xref.end()) {
			// Chain points at an xref we did not observe via startxref — stop.
			break;
		}
		chain.push_back(it->second);
		int64_t prev = -1;
		if (!PdfReadPrevAtXref(bytes, cur, prev)) {
			break;
		}
		if (prev < 0) {
			break;
		}
		cur = prev;
	}

	// chain is newest-first; reverse to oldest-first.
	if (chain.empty()) {
		// Fallback: all sections in document order.
		chain = sections;
	} else {
		std::reverse(chain.begin(), chain.end());
	}

	rows.reserve(chain.size());
	for (size_t i = 0; i < chain.size(); i++) {
		PdfRevisionRow row;
		row.revision_index = (int32_t)i;
		row.startxref_offset = chain[i].xref_offset;
		row.eof_offset = chain[i].eof_offset;
		int64_t prev_eof = (i == 0) ? 0 : chain[i - 1].eof_offset;
		row.size_bytes = row.eof_offset - prev_eof;
		if (row.size_bytes < 0) {
			row.size_bytes = 0;
		}
		row.is_incremental = (i > 0);
		rows.push_back(row);
	}
	return rows;
}

struct PdfRevisionsState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	idx_t row_idx = 0;
	std::vector<PdfRevisionRow> rows;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PdfRevisionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BOOLEAN};
	names = {"revision_index", "startxref_offset", "eof_offset", "size_bytes", "is_incremental"};
	return PdfInspectBindCommon(context, input);
}

static unique_ptr<GlobalTableFunctionState> PdfRevisionsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PdfRevisionsState>();
}

static void PdfRevisionsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfInspectBindData>();
	auto &st = data_p.global_state->Cast<PdfRevisionsState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (st.row_idx >= st.rows.size()) {
			if (st.file_idx >= bind.files.size()) {
				break;
			}
			st.rows.clear();
			st.row_idx = 0;
			string path = bind.files[st.file_idx++];
			string bytes;
			ReadAllBytes(context, path, bytes);
			// Password is accepted for API parity; trailers sit outside encrypted
			// streams so revision discovery does not require decryption. Skip
			// LoadDoc so a structured incremental tail remains visible even when
			// the body is damaged.
			(void)bind.opt.password;
			if (bytes.size() < 5 || bytes.compare(0, 5, "%PDF-") != 0) {
				// Tolerate leading junk (see garbage_header.pdf) by searching.
				size_t magic = bytes.find("%PDF-");
				if (magic == string::npos) {
					throw IOException("pdf_revisions: '%s' is not a PDF (missing %%PDF- header)", path);
				}
			}
			st.rows = EnumeratePdfRevisions(bytes);
			if (st.rows.empty()) {
				throw IOException("pdf_revisions: '%s' has no startxref/%%EOF revision markers", path);
			}
			continue;
		}
		auto &row = st.rows[st.row_idx];
		output.SetValue(0, count, Value::INTEGER(row.revision_index));
		output.SetValue(1, count, Value::BIGINT(row.startxref_offset));
		output.SetValue(2, count, Value::BIGINT(row.eof_offset));
		output.SetValue(3, count, Value::BIGINT(row.size_bytes));
		output.SetValue(4, count, Value::BOOLEAN(row.is_incremental));
		st.row_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_words  -> one row per word with bbox + font (layout/table substrate)
//===--------------------------------------------------------------------===//
struct ReadPdfWordsBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};

static unique_ptr<FunctionData> ReadPdfWordsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfWordsBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::DOUBLE,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::VARCHAR,
	                LogicalType::DOUBLE,  LogicalType::VARCHAR, LogicalType::DOUBLE};
	names = {"filename", "page", "word", "x0", "y0", "x1", "y1", "font_name", "font_size", "source", "confidence"};
	return std::move(result);
}

struct ReadPdfWordsState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	int page_idx = 0;
	int page_count = 0;
	int last_page_0 = 0;
	idx_t word_idx = 0;
	string file_bytes;
	unique_ptr<poppler::document> doc;
	std::vector<poppler::text_box> boxes;
	std::vector<OcrWord> ocr_boxes;
	bool page_is_ocr = false;
	string current_file;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static void WordsOpenFile(ClientContext &context, const ReadPdfWordsBindData &bind, ReadPdfWordsState &g) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	g.current_file = bind.files[g.file_idx];
	ReadAllBytes(context, g.current_file, g.file_bytes);
	g.doc = LoadDoc(g.file_bytes, bind.opt.password, g.current_file);
	g.page_count = g.doc->pages();
	g.page_idx = bind.opt.first_page > 0 ? bind.opt.first_page - 1 : 0;
	g.last_page_0 = bind.opt.last_page < 0 ? g.page_count : MinValue<int>(bind.opt.last_page, g.page_count);
	g.boxes.clear();
	g.ocr_boxes.clear();
	g.page_is_ocr = false;
	g.word_idx = 0;
}

static bool WordsLoadPage(ReadPdfWordsState &g, const PdfOptions &opt) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	g.boxes.clear();
	g.ocr_boxes.clear();
	g.page_is_ocr = false;
	g.word_idx = 0;
	if (g.page_idx >= g.last_page_0) {
		return false;
	}
	unique_ptr<poppler::page> page(g.doc->create_page(g.page_idx));
	if (page) {
		if (opt.force_ocr) {
			// Prefer OCR; fall back to native words when the raster is blank
			// (e.g. missing display fonts on a text-only PDF under vcpkg poppler).
			// Probe native first so best_effort can stay false on image-only pages
			// (loud missing-model error under explicit ocr:=true).
			g.boxes = page->text_list(poppler::page::text_list_include_font);
			const bool has_native = !g.boxes.empty();
			g.ocr_boxes = OcrPageWords(page.get(), opt.ocr_language, opt.ocr_dpi, opt.ocr_psm, opt.ocr_oem,
			                           opt.tessdata_dir, opt.ocr_preprocess, opt.ocr_retry,
			                           /*best_effort=*/has_native);
			if (!g.ocr_boxes.empty()) {
				g.boxes.clear();
				g.page_is_ocr = true;
			} else {
				g.page_is_ocr = false;
			}
		} else {
			g.boxes = page->text_list(poppler::page::text_list_include_font);
			if (!g.boxes.empty()) {
				g.page_is_ocr = false;
			} else if (opt.auto_ocr) {
				g.ocr_boxes = OcrPageWords(page.get(), opt.ocr_language, opt.ocr_dpi, opt.ocr_psm, opt.ocr_oem,
				                           opt.tessdata_dir, opt.ocr_preprocess, opt.ocr_retry, /*best_effort=*/true);
				g.page_is_ocr = !g.ocr_boxes.empty();
			} else {
				g.page_is_ocr = false;
			}
		}
	}
	return true;
}

static unique_ptr<GlobalTableFunctionState> ReadPdfWordsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadPdfWordsBindData>();
	auto g = make_uniq<ReadPdfWordsState>();
	if (!bind.files.empty()) {
		WordsOpenFile(context, bind, *g);
		WordsLoadPage(*g, bind.opt);
	}
	return std::move(g);
}

static void ReadPdfWordsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadPdfWordsBindData>();
	auto &g = data_p.global_state->Cast<ReadPdfWordsState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (!g.doc) {
			break;
		}
		bool page_done = g.page_is_ocr ? (g.word_idx >= g.ocr_boxes.size()) : (g.word_idx >= g.boxes.size());
		if (page_done) {
			// advance page, then file
			g.page_idx++;
			if (g.page_idx >= g.last_page_0) {
				g.file_idx++;
				if (g.file_idx >= bind.files.size()) {
					g.doc.reset();
					break;
				}
				WordsOpenFile(context, bind, g);
			}
			WordsLoadPage(g, bind.opt);
			continue;
		}
		output.SetValue(0, count, Value(g.current_file));
		output.SetValue(1, count, Value::INTEGER(g.page_idx + 1));
		if (g.page_is_ocr) {
			auto &w = g.ocr_boxes[g.word_idx];
			output.SetValue(2, count, Value(w.text));
			output.SetValue(3, count, Value::DOUBLE(w.x0));
			output.SetValue(4, count, Value::DOUBLE(w.y0));
			output.SetValue(5, count, Value::DOUBLE(w.x1));
			output.SetValue(6, count, Value::DOUBLE(w.y1));
			output.SetValue(7, count, Value(LogicalType::VARCHAR)); // font_name NULL for OCR
			output.SetValue(8, count, Value(LogicalType::DOUBLE));  // font_size NULL for OCR
			output.SetValue(9, count, Value("ocr"));
			output.SetValue(10, count, Value::DOUBLE(w.confidence));
		} else {
			auto &b = g.boxes[g.word_idx];
			auto r = b.bbox();
			output.SetValue(2, count, Value(UStringToUtf8(b.text())));
			output.SetValue(3, count, Value::DOUBLE(r.x()));
			output.SetValue(4, count, Value::DOUBLE(r.y()));
			output.SetValue(5, count, Value::DOUBLE(r.x() + r.width()));
			output.SetValue(6, count, Value::DOUBLE(r.y() + r.height()));
			output.SetValue(7, count, b.has_font_info() ? Value(b.get_font_name()) : Value(LogicalType::VARCHAR));
			output.SetValue(8, count,
			                b.has_font_info() ? Value::DOUBLE(b.get_font_size()) : Value(LogicalType::DOUBLE));
			output.SetValue(9, count, Value("text"));
			output.SetValue(10, count, Value(LogicalType::DOUBLE)); // NULL confidence for native
		}
		g.word_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_lines  -> one row per layout-preserving line of text
// (analog to the community `read_lines` extension, but PDF-aware). Splits each
// page's extracted text on newlines; honors the same `layout` knob as read_pdf.
//===--------------------------------------------------------------------===//
struct ReadPdfLinesBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};

static unique_ptr<FunctionData> ReadPdfLinesBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfLinesBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR};
	names = {"filename", "page", "line", "text"};
	return std::move(result);
}

struct ReadPdfLinesState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	int page_idx = 0; // 0-based
	int page_count = 0;
	int last_page_0 = 0;
	idx_t line_idx = 0;
	string file_bytes;
	unique_ptr<poppler::document> doc;
	vector<string> lines;
	string current_file;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static void LinesOpenFile(ClientContext &context, const ReadPdfLinesBindData &bind, ReadPdfLinesState &g) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	g.current_file = bind.files[g.file_idx];
	ReadAllBytes(context, g.current_file, g.file_bytes);
	g.doc = LoadDoc(g.file_bytes, bind.opt.password, g.current_file);
	g.page_count = g.doc->pages();
	g.page_idx = bind.opt.first_page > 0 ? bind.opt.first_page - 1 : 0;
	g.last_page_0 = bind.opt.last_page < 0 ? g.page_count : MinValue<int>(bind.opt.last_page, g.page_count);
	g.lines.clear();
	g.line_idx = 0;
}

static bool LinesLoadPage(ReadPdfLinesState &g, const PdfOptions &opt) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	g.lines.clear();
	g.line_idx = 0;
	if (g.page_idx >= g.last_page_0) {
		return false;
	}
	auto layout = LayoutFromString(opt.layout, false);
	unique_ptr<poppler::page> page(g.doc->create_page(g.page_idx));
	if (page) {
		string text = UStringToUtf8(page->text(poppler::rectf(), layout));
		size_t start = 0;
		while (start <= text.size()) {
			size_t nl = text.find('\n', start);
			if (nl == string::npos) {
				g.lines.push_back(text.substr(start));
				break;
			}
			g.lines.push_back(text.substr(start, nl - start));
			start = nl + 1;
		}
		// Drop trailing empty/whitespace-only lines so a page does not emit a
		// run of blank rows; interior blank lines are preserved as-is.
		while (!g.lines.empty() && g.lines.back().find_first_not_of(" \t\r\f\v") == string::npos) {
			g.lines.pop_back();
		}
	}
	return true;
}

static unique_ptr<GlobalTableFunctionState> ReadPdfLinesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadPdfLinesBindData>();
	auto g = make_uniq<ReadPdfLinesState>();
	if (!bind.files.empty()) {
		LinesOpenFile(context, bind, *g);
		LinesLoadPage(*g, bind.opt);
	}
	return std::move(g);
}

static void ReadPdfLinesScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadPdfLinesBindData>();
	auto &g = data_p.global_state->Cast<ReadPdfLinesState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (!g.doc) {
			break;
		}
		if (g.line_idx >= g.lines.size()) {
			g.page_idx++;
			if (g.page_idx >= g.last_page_0) {
				g.file_idx++;
				if (g.file_idx >= bind.files.size()) {
					g.doc.reset();
					break;
				}
				LinesOpenFile(context, bind, g);
			}
			LinesLoadPage(g, bind.opt);
			continue;
		}
		output.SetValue(0, count, Value(g.current_file));
		output.SetValue(1, count, Value::INTEGER(g.page_idx + 1));
		output.SetValue(2, count, Value::INTEGER((int)g.line_idx + 1));
		output.SetValue(3, count, Value(g.lines[g.line_idx]));
		g.line_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_elements -> one row per layout element
//   (file, page_number, element_idx, element_type, text, font_size,
//    bbox_x0, bbox_y0, bbox_x1, bbox_y1)
//
// Deterministic geometry over poppler-cpp's positioned word list
// (page::text_list with font info). No OCR path in v1: pages without a
// native text layer emit no elements. Pipeline: words -> lines -> blocks
// -> classified elements. All thresholds live in the named constants
// below; the rules are the spec.
//
// SEGMENTATION CONTRACT
//  1. LINE CLUSTERING: words sorted by (y0, x0); a word joins the current
//     line when its vertical overlap with the line bbox is at least
//     ELEM_LINE_OVERLAP_MIN_RATIO of the shorter of the two heights.
//     Words within a line are re-sorted by x0 and joined with spaces.
//     (Multi-column pages: words on the same visual row across columns
//     merge into one line — a documented v1 limitation.)
//  2. BLOCK SEGMENTATION: lines in top-down order start a new block when
//     any of these fire:
//       a. GAP BREAK: vertical gap (line.y0 - prev.y1) exceeds
//          ELEM_BLOCK_GAP_RATIO x the median line height on the page;
//       b. FONT BREAK: the line's dominant font size differs from the
//          previous line's by more than ELEM_FONT_CHANGE_RATIO
//          (relative to the previous line's size);
//       c. LIST BREAK: the line begins with a list marker (see rule 4)
//          — every list item becomes its own block regardless of gaps.
//  3. BODY SIZE: the document's body font size is the modal font size
//     weighted by character count across every word in the scanned page
//     range (the size that renders the most characters wins; ties go to
//     the smaller size for determinism).
//
// CLASSIFICATION CONTRACT (first match wins, in this order)
//  4. heading    : EITHER (a) block's dominant font size >=
//                  ELEM_HEADING_SIZE_RATIO x body size AND total text
//                  shorter than ELEM_HEADING_MAX_CHARS characters,
//                  OR (b) ALL-CAPS rule: the block has fewer than
//                  ELEM_CAPS_HEADING_MAX_WORDS words, at least
//                  ELEM_CAPS_HEADING_MIN_ALPHA alphabetic (ASCII A-Za-z)
//                  characters, and at least ELEM_CAPS_HEADING_UPPER_RATIO
//                  of those alphabetic characters are uppercase —
//                  REGARDLESS of font size. This catches short shouty
//                  section headers ("PROFESSIONAL SUMMARY") set at body
//                  size. Because heading is checked first, an all-caps
//                  block that also opens with a list marker classifies
//                  as heading, not list_item.
//  5. list_item  : block's first line starts with a bullet glyph
//                  (one of • – ▪, or '-' / '*' followed by a space) or a
//                  numeric marker: 1-3 digits then '.' or ')' then a
//                  space / end of text.
//  6. paragraph  : any remaining block with at least
//                  ELEM_MIN_PARAGRAPH_WORDS words.
//  7. other      : everything else (page numbers, isolated fragments).
//
// font_size is the block's dominant size (modal by character count) and
// NULL when poppler reports no font info for any word in the block.
//===--------------------------------------------------------------------===//

// Rule 1: min vertical-overlap ratio for a word to join a line.
static constexpr double ELEM_LINE_OVERLAP_MIN_RATIO = 0.5;
// Rule 2a: vertical gap > this fraction of median line height starts a new block.
static constexpr double ELEM_BLOCK_GAP_RATIO = 0.6;
// Rule 2b: relative font-size change > this fraction starts a new block.
static constexpr double ELEM_FONT_CHANGE_RATIO = 0.15;
// Rule 4: heading must be at least this multiple of the body font size...
static constexpr double ELEM_HEADING_SIZE_RATIO = 1.15;
// ...and shorter than this many characters (long large-print runs are not headings).
static constexpr size_t ELEM_HEADING_MAX_CHARS = 200;
// Rule 6: blocks with fewer words than this fall through to 'other'.
static constexpr size_t ELEM_MIN_PARAGRAPH_WORDS = 3;
// Rule 4b (ALL-CAPS heading): a block with fewer than this many words...
static constexpr size_t ELEM_CAPS_HEADING_MAX_WORDS = 6;
// ...at least this many ASCII alphabetic characters (filters "42", "IV")...
static constexpr size_t ELEM_CAPS_HEADING_MIN_ALPHA = 4;
// ...where at least this fraction of the alphabetic characters are
// uppercase is a heading regardless of font size.
static constexpr double ELEM_CAPS_HEADING_UPPER_RATIO = 0.8;

struct ElemWord {
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
	string text;
	double font_size = 0.0;
	bool has_font = false;
};

struct ElemLine {
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
	std::vector<ElemWord> words;
	string text;
	double font_size = 0.0; // dominant (modal by char count); 0 => unknown
};

struct PdfElementRow {
	int page_number = 0; // 1-based
	int element_idx = 0; // 1-based within page, reading order
	string element_type; // heading | paragraph | list_item | other
	string text;
	double font_size = 0.0;
	bool has_font = false;
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

// Character-count-weighted font-size histogram; keys are sizes rounded to
// 0.01pt so float jitter from poppler does not split one logical size.
using ElemFontHistogram = std::map<long long, size_t>;

static void ElemFontTally(ElemFontHistogram &hist, double font_size, size_t chars) {
	hist[(long long)std::llround(font_size * 100.0)] += chars;
}

// Modal font size (rule 3): most characters wins; ties -> smaller size
// (std::map iterates keys ascending, so the first strict max is the smallest).
static double ElemModalFontSize(const ElemFontHistogram &hist) {
	double best_size = 0.0;
	size_t best_chars = 0;
	for (auto &entry : hist) {
		if (entry.second > best_chars) {
			best_chars = entry.second;
			best_size = entry.first / 100.0;
		}
	}
	return best_size;
}

// Rule 4b: short ALL-CAPS block at any font size (see contract above).
// Only ASCII letters are counted — multi-byte UTF-8 letters neither help
// nor hurt the ratio (documented limitation: "RÉSUMÉ" counts 5 of its 6
// letters).
static bool ElemIsAllCapsHeading(const string &block_text, size_t word_count) {
	if (word_count >= ELEM_CAPS_HEADING_MAX_WORDS) {
		return false;
	}
	size_t alpha_chars = 0, upper_chars = 0;
	for (unsigned char c : block_text) {
		if (isalpha(c)) {
			alpha_chars++;
			if (isupper(c)) {
				upper_chars++;
			}
		}
	}
	return alpha_chars >= ELEM_CAPS_HEADING_MIN_ALPHA &&
	       (double)upper_chars >= ELEM_CAPS_HEADING_UPPER_RATIO * (double)alpha_chars;
}

// Rule 5: does this line text open with a bullet glyph or numeric marker?
static bool ElemIsListMarkerLine(const string &line_text) {
	size_t i = line_text.find_first_not_of(" \t");
	if (i == string::npos) {
		return false;
	}
	// Multi-byte bullet glyphs (UTF-8): • U+2022, – U+2013, ▪ U+25AA.
	static const char *kBulletGlyphs[] = {"\xE2\x80\xA2", "\xE2\x80\x93", "\xE2\x96\xAA"};
	for (auto glyph : kBulletGlyphs) {
		if (line_text.compare(i, strlen(glyph), glyph) == 0) {
			return true;
		}
	}
	// ASCII '-' / '*' count only when followed by a space ("-5 degrees" and
	// "*emphasis*" are prose, "- item" is a bullet).
	if ((line_text[i] == '-' || line_text[i] == '*') && i + 1 < line_text.size() && line_text[i + 1] == ' ') {
		return true;
	}
	// Numeric marker: 1-3 digits, then '.' or ')', then space or end of text.
	size_t d = i;
	while (d < line_text.size() && isdigit((unsigned char)line_text[d]) && d - i < 3) {
		d++;
	}
	if (d > i && d < line_text.size() && (line_text[d] == '.' || line_text[d] == ')')) {
		return d + 1 >= line_text.size() || line_text[d + 1] == ' ';
	}
	return false;
}

// Rule 1: cluster one page's words into lines.
static std::vector<ElemLine> ElemBuildLines(std::vector<ElemWord> words) {
	std::vector<ElemLine> lines;
	std::sort(words.begin(), words.end(), [](const ElemWord &a, const ElemWord &b) {
		if (a.y0 != b.y0) {
			return a.y0 < b.y0;
		}
		return a.x0 < b.x0;
	});
	for (auto &w : words) {
		bool joined = false;
		if (!lines.empty()) {
			auto &line = lines.back();
			double overlap = MinValue<double>(w.y1, line.y1) - MaxValue<double>(w.y0, line.y0);
			double shorter = MinValue<double>(w.y1 - w.y0, line.y1 - line.y0);
			if (shorter > 0 && overlap >= ELEM_LINE_OVERLAP_MIN_RATIO * shorter) {
				line.x0 = MinValue<double>(line.x0, w.x0);
				line.y0 = MinValue<double>(line.y0, w.y0);
				line.x1 = MaxValue<double>(line.x1, w.x1);
				line.y1 = MaxValue<double>(line.y1, w.y1);
				line.words.push_back(std::move(w));
				joined = true;
			}
		}
		if (!joined) {
			ElemLine line;
			line.x0 = w.x0;
			line.y0 = w.y0;
			line.x1 = w.x1;
			line.y1 = w.y1;
			line.words.push_back(std::move(w));
			lines.push_back(std::move(line));
		}
	}
	// Finalize: order words by x0, join text, compute dominant font size.
	for (auto &line : lines) {
		std::sort(line.words.begin(), line.words.end(),
		          [](const ElemWord &a, const ElemWord &b) { return a.x0 < b.x0; });
		ElemFontHistogram line_hist;
		for (auto &w : line.words) {
			if (!line.text.empty()) {
				line.text += " ";
			}
			line.text += w.text;
			if (w.has_font) {
				ElemFontTally(line_hist, w.font_size, w.text.size());
			}
		}
		line.font_size = ElemModalFontSize(line_hist);
	}
	return lines;
}

// Rules 2 + 4-7: segment one page's lines into blocks and classify them.
// body_size is the document-wide modal size (rule 3); rows are appended in
// reading order with 1-based element_idx.
static void ElemEmitPageBlocks(const std::vector<ElemLine> &lines, int page_number, double body_size,
                               std::vector<PdfElementRow> &rows) {
	if (lines.empty()) {
		return;
	}
	std::vector<double> heights;
	heights.reserve(lines.size());
	for (auto &line : lines) {
		heights.push_back(line.y1 - line.y0);
	}
	double median_height = Median(heights);

	// Group line indices into blocks.
	std::vector<std::vector<size_t>> blocks;
	for (size_t li = 0; li < lines.size(); li++) {
		bool start_new = blocks.empty();
		if (!start_new) {
			const auto &prev = lines[blocks.back().back()];
			const auto &cur = lines[li];
			double gap = cur.y0 - prev.y1;
			bool gap_break = median_height > 0 && gap > ELEM_BLOCK_GAP_RATIO * median_height; // rule 2a
			bool font_break =
			    prev.font_size > 0 && cur.font_size > 0 &&
			    std::fabs(cur.font_size - prev.font_size) > ELEM_FONT_CHANGE_RATIO * prev.font_size; // rule 2b
			bool list_break = ElemIsListMarkerLine(cur.text);                                        // rule 2c
			start_new = gap_break || font_break || list_break;
		}
		if (start_new) {
			blocks.emplace_back();
		}
		blocks.back().push_back(li);
	}

	int element_idx = 0;
	for (auto &block : blocks) {
		PdfElementRow row;
		row.page_number = page_number;
		row.element_idx = ++element_idx;
		ElemFontHistogram block_hist;
		size_t word_count = 0;
		bool first = true;
		for (auto li : block) {
			const auto &line = lines[li];
			if (first) {
				row.x0 = line.x0;
				row.y0 = line.y0;
				row.x1 = line.x1;
				row.y1 = line.y1;
				first = false;
			} else {
				row.x0 = MinValue<double>(row.x0, line.x0);
				row.y0 = MinValue<double>(row.y0, line.y0);
				row.x1 = MaxValue<double>(row.x1, line.x1);
				row.y1 = MaxValue<double>(row.y1, line.y1);
				row.text += " ";
			}
			row.text += line.text;
			word_count += line.words.size();
			for (auto &w : line.words) {
				if (w.has_font) {
					ElemFontTally(block_hist, w.font_size, w.text.size());
				}
			}
		}
		row.font_size = ElemModalFontSize(block_hist);
		row.has_font = row.font_size > 0;

		const string &first_line_text = lines[block.front()].text;
		if ((row.has_font && body_size > 0 && row.font_size >= ELEM_HEADING_SIZE_RATIO * body_size &&
		     row.text.size() < ELEM_HEADING_MAX_CHARS) ||
		    ElemIsAllCapsHeading(row.text, word_count)) {
			row.element_type = "heading"; // rule 4 (font size) or 4b (ALL-CAPS)
		} else if (ElemIsListMarkerLine(first_line_text)) {
			row.element_type = "list_item"; // rule 5
		} else if (word_count >= ELEM_MIN_PARAGRAPH_WORDS) {
			row.element_type = "paragraph"; // rule 6
		} else {
			row.element_type = "other"; // rule 7
		}
		rows.push_back(std::move(row));
	}
}

// Load one file and materialize its full element list. Two passes over the
// collected words: pass 1 builds per-page lines and the document font
// histogram (rule 3 needs the whole document), pass 2 segments + classifies.
// The poppler critical section covers this call only — the scan loop below
// emits from the materialized vector without touching poppler.
static void ElementsProcessFile(ClientContext &context, const string &path, const PdfOptions &opt,
                                std::vector<PdfElementRow> &rows) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	rows.clear();
	string bytes;
	ReadAllBytes(context, path, bytes);
	auto doc = LoadDoc(bytes, opt.password, path);
	int page_count = doc->pages();
	int first_0 = opt.first_page > 0 ? opt.first_page - 1 : 0;
	int last_0 = opt.last_page < 0 ? page_count : MinValue<int>(opt.last_page, page_count);

	std::vector<std::pair<int, std::vector<ElemLine>>> page_lines; // (1-based page, lines)
	ElemFontHistogram doc_hist;
	for (int p = first_0; p < last_0; p++) {
		unique_ptr<poppler::page> page(doc->create_page(p));
		if (!page) {
			continue;
		}
		std::vector<ElemWord> words;
		for (auto &b : page->text_list(poppler::page::text_list_include_font)) {
			ElemWord w;
			w.text = UStringToUtf8(b.text());
			if (w.text.find_first_not_of(" \t\r\n\f\v") == string::npos) {
				continue;
			}
			auto r = b.bbox();
			w.x0 = r.x();
			w.y0 = r.y();
			w.x1 = r.x() + r.width();
			w.y1 = r.y() + r.height();
			w.has_font = b.has_font_info();
			w.font_size = w.has_font ? b.get_font_size() : 0.0;
			if (w.has_font) {
				ElemFontTally(doc_hist, w.font_size, w.text.size());
			}
			words.push_back(std::move(w));
		}
		if (!words.empty()) {
			page_lines.emplace_back(p + 1, ElemBuildLines(std::move(words)));
		}
	}
	double body_size = ElemModalFontSize(doc_hist);
	for (auto &pl : page_lines) {
		ElemEmitPageBlocks(pl.second, pl.first, body_size, rows);
	}
}

struct ReadPdfElementsBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};

struct ReadPdfElementsState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	idx_t row_idx = 0;
	std::vector<PdfElementRow> rows;
	string current_file;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadPdfElementsBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfElementsBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::DOUBLE,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE};
	names = {"file",      "page_number", "element_idx", "element_type", "text",
	         "font_size", "bbox_x0",     "bbox_y0",     "bbox_x1",      "bbox_y1"};
	return std::move(result);
}

static void ElementsOpenFile(ClientContext &context, const ReadPdfElementsBindData &bind, ReadPdfElementsState &g) {
	g.current_file = bind.files[g.file_idx];
	g.row_idx = 0;
	ElementsProcessFile(context, g.current_file, bind.opt, g.rows);
}

static unique_ptr<GlobalTableFunctionState> ReadPdfElementsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadPdfElementsBindData>();
	auto g = make_uniq<ReadPdfElementsState>();
	if (!bind.files.empty()) {
		ElementsOpenFile(context, bind, *g);
	}
	return std::move(g);
}

static void ReadPdfElementsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadPdfElementsBindData>();
	auto &g = data_p.global_state->Cast<ReadPdfElementsState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (g.file_idx >= bind.files.size()) {
			break;
		}
		if (g.row_idx >= g.rows.size()) {
			g.file_idx++;
			if (g.file_idx >= bind.files.size()) {
				break;
			}
			ElementsOpenFile(context, bind, g);
			continue;
		}
		auto &row = g.rows[g.row_idx];
		output.SetValue(0, count, Value(g.current_file));
		output.SetValue(1, count, Value::INTEGER(row.page_number));
		output.SetValue(2, count, Value::INTEGER(row.element_idx));
		output.SetValue(3, count, Value(row.element_type));
		output.SetValue(4, count, Value(row.text));
		output.SetValue(5, count, row.has_font ? Value::DOUBLE(row.font_size) : Value(LogicalType::DOUBLE));
		output.SetValue(6, count, Value::DOUBLE(row.x0));
		output.SetValue(7, count, Value::DOUBLE(row.y0));
		output.SetValue(8, count, Value::DOUBLE(row.x1));
		output.SetValue(9, count, Value::DOUBLE(row.y1));
		g.row_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// pdf_chunks -> retrieval-ready chunks over the element grain
//   (file, chunk_idx, text, page_start, page_end, n_chars, heading)
//
// Deterministic packing of read_pdf_elements output (reuses
// ElementsProcessFile — same segmentation, same classifier, same per-file
// poppler critical section; chunking itself never touches poppler).
//
// CHUNKING CONTRACT
//  C1. PACKING: elements are consumed in reading order and packed into a
//      chunk until appending the next element (plus the 1-char '\n'
//      joiner) would push the chunk's core text past chunk_size bytes.
//  C2. NO MID-ELEMENT SPLITS: an element is never split across chunks —
//      UNLESS the element alone exceeds chunk_size, in which case it is
//      pre-split at the last ASCII whitespace at or before the limit,
//      repeatedly, into pieces each <= chunk_size bytes. A whitespace-free
//      run longer than chunk_size is hard-cut at a UTF-8 codepoint
//      boundary.
//  C3. HEADINGS GLUE FORWARD: a 'heading' element never ends a chunk. If
//      packing would leave a heading last, the heading moves to the front
//      of the next chunk instead. If the heading is the ONLY element in
//      the chunk, the next element is glued to it even when that exceeds
//      chunk_size (the one documented budget exception; a trailing
//      heading at end-of-file is emitted as-is — there is no next chunk).
//  C4. OVERLAP: each chunk after the first is prefixed with the trailing
//      `overlap` bytes of the previous chunk's emitted text, trimmed
//      forward to the next whitespace boundary so no word is cut, then
//      joined to the core with '\n'. The prefix is always a suffix of the
//      previous chunk's text column. Chunk 1 has no prefix.
//  C5. SIZE INVARIANT: n_chars (UTF-8 codepoints of the full emitted
//      text, overlap included) <= chunk_size + overlap + 1 (the +1 is the
//      '\n' joining overlap to core), except when C3's glue exception
//      fires (then core may reach one heading + '\n' + chunk_size).
//      Budgets are counted in bytes; n_chars is codepoints, so the
//      invariant holds a fortiori for non-ASCII text.
//  C6. HEADING COLUMN: the text of the most recent 'heading' element at
//      or before the chunk's first non-overlap element (a chunk that
//      starts with a heading reports that heading). NULL before the
//      first heading of the file.
//  C7. chunk_idx is 1-based and dense per file; page_start/page_end are
//      the min/max element pages contributing to the chunk's core.
//===--------------------------------------------------------------------===//

// Defaults for the pdf_chunks named parameters.
static constexpr int32_t CHUNK_DEFAULT_SIZE = 1200;
static constexpr int32_t CHUNK_DEFAULT_OVERLAP = 150;

struct PdfChunkRow {
	int chunk_idx = 0; // 1-based per file
	string text;
	int page_start = 0;
	int page_end = 0;
	int64_t n_chars = 0;
	bool has_heading = false;
	string heading;
};

// One packable piece: a whole element, or one slice of an oversized one.
struct ChunkUnit {
	string text;
	int page = 0;
	bool is_heading = false;
	bool has_heading = false; // heading context (C6) at this unit
	string heading;
};

static bool ChunkIsAsciiSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int64_t Utf8CodepointCount(const string &text) {
	int64_t count = 0;
	for (unsigned char c : text) {
		if ((c & 0xC0) != 0x80) {
			count++;
		}
	}
	return count;
}

// C2: split one oversized element text into pieces each <= limit bytes,
// cutting at the last ASCII whitespace at or before the limit (hard cut at
// a codepoint boundary only when a single "word" exceeds the limit).
static std::vector<string> ChunkSplitLongText(const string &text, size_t limit) {
	std::vector<string> pieces;
	size_t pos = 0;
	while (text.size() - pos > limit) {
		size_t cut = string::npos;
		for (size_t i = pos + limit; i > pos; i--) {
			if (ChunkIsAsciiSpace(text[i])) {
				cut = i;
				break;
			}
		}
		if (cut == string::npos) {
			// whitespace-free run: hard cut, backed up to a codepoint boundary
			cut = pos + limit;
			while (cut > pos && ((unsigned char)text[cut] & 0xC0) == 0x80) {
				cut--;
			}
			if (cut == pos) {
				// limit smaller than one codepoint: split it rather than loop
				cut = pos + limit;
			}
			pieces.push_back(text.substr(pos, cut - pos));
			pos = cut;
		} else {
			pieces.push_back(text.substr(pos, cut - pos));
			pos = cut + 1; // skip the whitespace we cut at
		}
		while (pos < text.size() && ChunkIsAsciiSpace(text[pos])) {
			pos++;
		}
	}
	if (pos < text.size()) {
		pieces.push_back(text.substr(pos));
	}
	return pieces;
}

// C4: trailing `overlap` bytes of the emitted text, advanced to the next
// whitespace boundary so no word is cut. Always a suffix of `text`.
static string ChunkComputeOverlap(const string &text, size_t overlap) {
	if (overlap == 0) {
		return string();
	}
	if (text.size() <= overlap) {
		return text;
	}
	size_t start = text.size() - overlap;
	if (!ChunkIsAsciiSpace(text[start - 1])) {
		while (start < text.size() && !ChunkIsAsciiSpace(text[start])) {
			start++;
		}
	}
	while (start < text.size() && ChunkIsAsciiSpace(text[start])) {
		start++;
	}
	return text.substr(start);
}

// Pack one file's elements into chunks (rules C1-C7 above).
static void ChunksProcessFile(ClientContext &context, const string &path, const PdfOptions &opt, size_t chunk_size,
                              size_t overlap, std::vector<PdfChunkRow> &out) {
	out.clear();
	std::vector<PdfElementRow> elements;
	ElementsProcessFile(context, path, opt, elements);

	// Elements -> units, tracking the running heading context (C6) and
	// pre-splitting oversized elements (C2).
	std::vector<ChunkUnit> units;
	bool have_heading = false;
	string current_heading;
	for (auto &elem : elements) {
		bool is_heading = elem.element_type == "heading";
		if (is_heading) {
			have_heading = true;
			current_heading = elem.text;
		}
		for (auto &piece : elem.text.size() > chunk_size ? ChunkSplitLongText(elem.text, chunk_size)
		                                                 : std::vector<string> {elem.text}) {
			ChunkUnit unit;
			unit.text = std::move(piece);
			unit.page = elem.page_number;
			unit.is_heading = is_heading;
			unit.has_heading = have_heading;
			unit.heading = current_heading;
			units.push_back(std::move(unit));
		}
	}

	std::vector<ChunkUnit> cur;
	size_t cur_len = 0;
	string prev_overlap;
	int chunk_idx = 0;
	auto flush = [&]() {
		if (cur.empty()) {
			return;
		}
		string core;
		int page_start = cur.front().page, page_end = cur.front().page;
		for (auto &unit : cur) {
			if (!core.empty()) {
				core += "\n";
			}
			core += unit.text;
			page_start = MinValue<int>(page_start, unit.page);
			page_end = MaxValue<int>(page_end, unit.page);
		}
		PdfChunkRow row;
		row.chunk_idx = ++chunk_idx;
		row.text = prev_overlap.empty() ? core : prev_overlap + "\n" + core;
		row.page_start = page_start;
		row.page_end = page_end;
		row.n_chars = Utf8CodepointCount(row.text);
		row.has_heading = cur.front().has_heading;
		row.heading = cur.front().heading;
		prev_overlap = ChunkComputeOverlap(row.text, overlap);
		out.push_back(std::move(row));
		cur.clear();
		cur_len = 0;
	};

	for (auto &unit : units) {
		size_t appended = cur.empty() ? unit.text.size() : cur_len + 1 + unit.text.size();
		if (!cur.empty() && appended > chunk_size) {
			if (cur.back().is_heading) {
				if (cur.size() == 1) {
					// C3 glue exception: lone heading takes the unit anyway
					cur.push_back(unit);
					cur_len = appended;
					continue;
				}
				// C3: pop the trailing heading, flush, restart from it
				ChunkUnit moved = std::move(cur.back());
				cur.pop_back();
				flush();
				cur_len = moved.text.size();
				cur.push_back(std::move(moved));
				cur.push_back(unit);
				cur_len += 1 + unit.text.size(); // may exceed (C3 glue)
				continue;
			}
			flush();
		}
		cur_len = cur.empty() ? unit.text.size() : cur_len + 1 + unit.text.size();
		cur.push_back(unit);
	}
	flush(); // a trailing heading at EOF is emitted as-is (C3)
}

struct PdfChunksBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
	size_t chunk_size = CHUNK_DEFAULT_SIZE;
	size_t overlap = CHUNK_DEFAULT_OVERLAP;
};

struct PdfChunksState : public GlobalTableFunctionState {
	idx_t file_idx = 0;
	idx_t row_idx = 0;
	std::vector<PdfChunkRow> rows;
	string current_file;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PdfChunksBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfChunksBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	int32_t chunk_size = CHUNK_DEFAULT_SIZE;
	int32_t overlap = CHUNK_DEFAULT_OVERLAP;
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "chunk_size") {
			chunk_size = IntegerValue::Get(kv.second);
		} else if (key == "overlap") {
			overlap = IntegerValue::Get(kv.second);
		}
	}
	if (chunk_size < 1) {
		throw InvalidInputException("pdf_chunks: chunk_size must be >= 1 (got %d)", chunk_size);
	}
	if (overlap < 0) {
		throw InvalidInputException("pdf_chunks: overlap must be >= 0 (got %d)", overlap);
	}
	if (overlap >= chunk_size) {
		throw InvalidInputException("pdf_chunks: overlap (%d) must be smaller than chunk_size (%d)", overlap,
		                            chunk_size);
	}
	result->chunk_size = (size_t)chunk_size;
	result->overlap = (size_t)overlap;
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR};
	names = {"file", "chunk_idx", "text", "page_start", "page_end", "n_chars", "heading"};
	return std::move(result);
}

static void PdfChunksOpenFile(ClientContext &context, const PdfChunksBindData &bind, PdfChunksState &g) {
	g.current_file = bind.files[g.file_idx];
	g.row_idx = 0;
	ChunksProcessFile(context, g.current_file, bind.opt, bind.chunk_size, bind.overlap, g.rows);
}

static unique_ptr<GlobalTableFunctionState> PdfChunksInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<PdfChunksBindData>();
	auto g = make_uniq<PdfChunksState>();
	if (!bind.files.empty()) {
		PdfChunksOpenFile(context, bind, *g);
	}
	return std::move(g);
}

static void PdfChunksScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfChunksBindData>();
	auto &g = data_p.global_state->Cast<PdfChunksState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (g.file_idx >= bind.files.size()) {
			break;
		}
		if (g.row_idx >= g.rows.size()) {
			g.file_idx++;
			if (g.file_idx >= bind.files.size()) {
				break;
			}
			PdfChunksOpenFile(context, bind, g);
			continue;
		}
		auto &row = g.rows[g.row_idx];
		output.SetValue(0, count, Value(g.current_file));
		output.SetValue(1, count, Value::INTEGER(row.chunk_idx));
		output.SetValue(2, count, Value(row.text));
		output.SetValue(3, count, Value::INTEGER(row.page_start));
		output.SetValue(4, count, Value::INTEGER(row.page_end));
		output.SetValue(5, count, Value::INTEGER((int32_t)row.n_chars));
		output.SetValue(6, count, row.has_heading ? Value(row.heading) : Value(LogicalType::VARCHAR));
		g.row_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// read_pdf_tables  -> one row per table row, cells as VARCHAR[]
// Backed by ReconstructPageGrid over poppler-cpp page::text_list() positions.
//===--------------------------------------------------------------------===//
struct ReadPdfTablesBindData : public TableFunctionData {
	vector<string> files;
	PdfOptions opt;
};
struct TableRowOut {
	string file;
	int page;
	int table_index;
	int row_index;
	std::vector<std::string> cells;
};
struct ReadPdfTablesState : public GlobalTableFunctionState {
	vector<TableRowOut> rows;
	idx_t idx = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadPdfTablesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadPdfTablesBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	ParseNamed(input.named_parameters, result->opt);
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER,
	                LogicalType::LIST(LogicalType::VARCHAR)};
	names = {"filename", "page", "table_index", "row_index", "cells"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ReadPdfTablesInit(ClientContext &context, TableFunctionInitInput &input) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	auto &bind = input.bind_data->Cast<ReadPdfTablesBindData>();
	auto st = make_uniq<ReadPdfTablesState>();
	for (auto &f : bind.files) {
		string bytes;
		unique_ptr<poppler::document> doc;
		try {
			ReadAllBytes(context, f, bytes);
			doc = LoadDoc(bytes, bind.opt.password, f);
		} catch (const std::exception &e) {
			// unwrap duckdb exceptions (their .what() is JSON) to expose the clean inner message
			// e.g. "read_pdf_tables: read_pdf: could not open '...'"
			ErrorData ed(e);
			string msg = ed.HasError() ? ed.RawMessage() : string(e.what());
			throw InvalidInputException("read_pdf_tables: %s", msg.c_str());
		}
		int page_count = doc->pages();
		int first0 = bind.opt.first_page > 0 ? bind.opt.first_page - 1 : 0;
		int last0 = bind.opt.last_page < 0 ? page_count : MinValue<int>(bind.opt.last_page, page_count);
		// Lattice ruling lines: collect axis-aligned rules from every page content
		// stream once via qpdf (poppler-cpp exposes no path API). Best-effort — any
		// failure leaves the whitespace/stream column model as the sole path.
		std::vector<pdf_qpdf::RuledSegment> ruling_segments;
		try {
			ruling_segments = pdf_qpdf::ExtractRulingLines(bytes, bind.opt.password);
		} catch (...) {
			ruling_segments.clear();
		}
		// table_index is a running counter over the tabular pages of this document
		// (each tabular page yields one reconstructed grid).
		int table_index = 0;
		for (int p = first0; p < last0; p++) {
			unique_ptr<poppler::page> page(doc->create_page(p));
			if (!page) {
				continue;
			}
			std::vector<PdfWord> words;
			if (bind.opt.force_ocr) {
				// Prefer OCR; fall back to native when the raster is blank.
				auto ocr_words =
				    OcrPageWords(page.get(), bind.opt.ocr_language, bind.opt.ocr_dpi, bind.opt.ocr_psm,
				                 bind.opt.ocr_oem, bind.opt.tessdata_dir, bind.opt.ocr_preprocess, bind.opt.ocr_retry,
				                 /*best_effort=*/true);
				for (auto &ow : ocr_words) {
					PdfWord w;
					w.xMin = ow.x0;
					w.yMin = ow.y0;
					w.xMax = ow.x1;
					w.yMax = ow.y1;
					w.text = ow.text;
					words.push_back(std::move(w));
				}
				if (words.empty()) {
					for (auto &b : page->text_list()) {
						auto r = b.bbox();
						PdfWord w;
						w.xMin = r.x();
						w.yMin = r.y();
						w.xMax = r.x() + r.width();
						w.yMax = r.y() + r.height();
						w.text = UStringToUtf8(b.text());
						words.push_back(std::move(w));
					}
				}
			} else {
				for (auto &b : page->text_list()) {
					auto r = b.bbox();
					PdfWord w;
					w.xMin = r.x();
					w.yMin = r.y();
					w.xMax = r.x() + r.width();
					w.yMax = r.y() + r.height();
					w.text = UStringToUtf8(b.text());
					words.push_back(std::move(w));
				}
				if (words.empty() && bind.opt.auto_ocr) {
					auto ocr_words = OcrPageWords(page.get(), bind.opt.ocr_language, bind.opt.ocr_dpi, bind.opt.ocr_psm,
					                              bind.opt.ocr_oem, bind.opt.tessdata_dir, bind.opt.ocr_preprocess,
					                              bind.opt.ocr_retry, /*best_effort=*/true);
					for (auto &ow : ocr_words) {
						PdfWord w;
						w.xMin = ow.x0;
						w.yMin = ow.y0;
						w.xMax = ow.x1;
						w.yMax = ow.y1;
						w.text = ow.text;
						words.push_back(std::move(w));
					}
				}
			}
			// Lattice first when the page content stream has ruling lines (qpdf);
			// ReconstructPageGrid falls back to the whitespace/stream model.
			RulingLines rules = RulesForPage(ruling_segments, p);
			auto grid = ReconstructPageGrid(std::move(words), &rules);
			if (grid.size() < 2 || grid.front().size() < 2) {
				continue;
			}
			for (idx_t r = 0; r < grid.size(); r++) {
				st->rows.push_back(TableRowOut {f, p + 1, table_index, (int)r, grid[r]});
			}
			table_index++;
		}
	}
	return std::move(st);
}

static void ReadPdfTablesScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &st = data_p.global_state->Cast<ReadPdfTablesState>();
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.idx < st.rows.size()) {
		auto &row = st.rows[st.idx];
		output.SetValue(0, count, Value(row.file));
		output.SetValue(1, count, Value::INTEGER(row.page));
		output.SetValue(2, count, Value::INTEGER(row.table_index));
		output.SetValue(3, count, Value::INTEGER(row.row_index));
		vector<Value> cells;
		for (auto &c : row.cells) {
			cells.push_back(Value(c));
		}
		output.SetValue(4, count, Value::LIST(LogicalType::VARCHAR, std::move(cells)));
		st.idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// pdf_to_text(path[, layout]) — whole-document text as a single VARCHAR.
// Library-backed (poppler-cpp); no external process. layout is one of
// 'reading' (default) | 'physical' | 'raw'.
//===--------------------------------------------------------------------===//
static string DocToText(poppler::document &doc, const string &layout_str) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	auto layout = LayoutFromString(layout_str, false);
	string out;
	int n = doc.pages();
	for (int p = 0; p < n; p++) {
		unique_ptr<poppler::page> page(doc.create_page(p));
		if (!page) {
			continue;
		}
		if (!out.empty()) {
			out.push_back('\n');
		}
		out += UStringToUtf8(page->text(poppler::rectf(), layout));
	}
	return out;
}

static string DocToTextPath(ClientContext &ctx, const string &path, const string &layout_str) {
	string bytes;
	ReadAllBytes(ctx, path, bytes);
	auto doc = LoadDoc(bytes, "", path);
	return DocToText(*doc, layout_str);
}

static void PdfToTextFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t path) {
		return StringVector::AddString(result, DocToTextPath(context, path.GetString(), "reading"));
	});
}
static void PdfToTextLayoutFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t path, string_t layout) {
		    return StringVector::AddString(result, DocToTextPath(context, path.GetString(), layout.GetString()));
	    });
}

//===--------------------------------------------------------------------===//
// pdf_to_html / pdf_to_xml / pdf_to_svg
//
// All three are library-backed (poppler-cpp); NO external process is spawned.
// Geometry comes from page::text_list() / page::page_rect(), reusing exactly the
// word/bbox handling that backs read_pdf_words (top-left origin, PDF points).
//===--------------------------------------------------------------------===//

// Removes a path on scope exit (best-effort; never throws).
struct TempFileGuard {
	std::string path;
	explicit TempFileGuard(std::string p) : path(std::move(p)) {
	}
	~TempFileGuard() {
		if (!path.empty()) {
			std::remove(path.c_str());
		}
	}
	TempFileGuard(const TempFileGuard &) = delete;
	TempFileGuard &operator=(const TempFileGuard &) = delete;
	TempFileGuard(TempFileGuard &&other) noexcept : path(std::move(other.path)) {
		other.path.clear();
	}
	TempFileGuard &operator=(TempFileGuard &&other) noexcept {
		if (this != &other) {
			if (!path.empty()) {
				std::remove(path.c_str());
			}
			path = std::move(other.path);
			other.path.clear();
		}
		return *this;
	}
};

// Holds a poppler document and its backing temp file. Destruction order (doc first, then guard)
// ensures poppler releases any file handles before std::remove.
struct PdfDocHandle {
	unique_ptr<poppler::document> doc; // declared first → destroyed first
	TempFileGuard guard;               // destroyed second → file removed after doc is gone
};

// Open a document for a scalar render function, matching pdf_to_text's error
// style (InvalidInputException for missing/corrupt, encrypted-without-password).
// Now routes through DuckDB VFS via ReadAllBytes + LoadDoc (bytes) for s3:// etc.
static PdfDocHandle LoadRenderDoc(const string &fn, const string &path, ClientContext &ctx) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	string bytes;
	try {
		ReadAllBytes(ctx, path, bytes);
		// Call LoadDoc on the VFS bytes (as specified); validates and exercises raw path, then we use
		// load_from_file on a materialized temp (from the VFS bytes) for consistent poppler recovery.
		auto validation = LoadDoc(bytes, "", path);
		(void)validation;
#ifdef _WIN32
		const char sep = '\\';
#else
		const char sep = '/';
#endif
		string unique = BaseUUID::ToString(UUID::GenerateRandomUUID());
		string tmp_path = TempDir() + sep + "pdf_render_" + unique + ".pdf";
		{
			std::ofstream of(tmp_path, std::ios::binary);
			of.write(bytes.data(), bytes.size());
		}
		TempFileGuard guard(tmp_path);
		unique_ptr<poppler::document> doc(poppler::document::load_from_file(tmp_path));
		if (!doc) {
			throw InvalidInputException("%s: could not open '%s' (missing, corrupt, or not a PDF)", fn, path);
		}
		if (doc->is_locked() || doc->is_encrypted()) {
			throw InvalidInputException("%s: '%s' is encrypted (use read_pdf with password := '...')", fn, path);
		}
		return PdfDocHandle {std::move(doc), std::move(guard)};
	} catch (const InvalidInputException &) {
		throw;
	} catch (...) {
		throw InvalidInputException("%s: could not open '%s' (missing, corrupt, or not a PDF)", fn, path);
	}
}

// New BLOB loader for the BLOB overloads of the scalar PDF functions. Accepts raw PDF bytes
// (BLOBs arrive as string_t in executors). Mirrors LoadDoc validation but with BLOB-specific
// InvalidInputException messages (no password support for BLOBs).
static PdfDocHandle LoadBlobDoc(const string &fn, const char *data, idx_t size) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	if (size > (idx_t)NumericLimits<int>::Maximum()) {
		throw InvalidInputException("%s: BLOB input is too large (%llu bytes; max ~2 GiB)", fn,
		                            (unsigned long long)size);
	}
	string bytes(data, size);
	// Materialize BLOB bytes to temp + load_from_file for consistent recovery (raw load can be flaky on
	// certain PDFs under verifier modes). Keep temp for doc life.
#ifdef _WIN32
	const char sep = '\\';
#else
	const char sep = '/';
#endif
	string unique = BaseUUID::ToString(UUID::GenerateRandomUUID());
	string tmp_path = TempDir() + sep + "pdf_blob_" + unique + ".pdf";
	{
		std::ofstream of(tmp_path, std::ios::binary);
		of.write(bytes.data(), bytes.size());
	}
	TempFileGuard guard(tmp_path);
	auto doc = unique_ptr<poppler::document>(poppler::document::load_from_file(tmp_path));
	if (!doc) {
		throw InvalidInputException("%s: input BLOB is not a valid PDF (%llu bytes)", fn, (unsigned long long)size);
	}
	if (doc->is_locked() || doc->is_encrypted()) {
		throw InvalidInputException("%s: BLOB input is encrypted (cannot decrypt without a password)", fn);
	}
	if (doc->pages() <= 0) {
		throw InvalidInputException("%s: BLOB input has no readable pages", fn);
	}
	return PdfDocHandle {std::move(doc), std::move(guard)};
}

// Escape the five predefined XML entities. Also used for HTML (a strict superset
// for text/attribute contexts of the characters we emit).
static void AppendXmlEscaped(string &out, const string &s) {
	for (char c : s) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '"':
			out += "&quot;";
			break;
		case '\'':
			out += "&apos;";
			break;
		default:
			out.push_back(c);
			break;
		}
	}
}

// Format a coordinate compactly (avoids locale issues / trailing noise).
static string FmtCoord(double v) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%.2f", v);
	return string(buf);
}

//===--------------------------------------------------------------------===//
// pdf_to_xml(path) — pdftoxml-style: <pdf2xml><page ..><word ..>..</word>..
//===--------------------------------------------------------------------===//
static string DocToXml(poppler::document &doc) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<pdf2xml>\n";
	int n = doc.pages();
	for (int p = 0; p < n; p++) {
		unique_ptr<poppler::page> page(doc.create_page(p));
		if (!page) {
			continue;
		}
		auto rect = page->page_rect();
		out += "  <page number=\"" + std::to_string(p + 1) + "\" width=\"" + FmtCoord(rect.width()) + "\" height=\"" +
		       FmtCoord(rect.height()) + "\">\n";
		for (auto &b : page->text_list(poppler::page::text_list_include_font)) {
			auto r = b.bbox();
			out += "    <word xMin=\"" + FmtCoord(r.x()) + "\" yMin=\"" + FmtCoord(r.y()) + "\" xMax=\"" +
			       FmtCoord(r.x() + r.width()) + "\" yMax=\"" + FmtCoord(r.y() + r.height()) + "\">";
			AppendXmlEscaped(out, UStringToUtf8(b.text()));
			out += "</word>\n";
		}
		out += "  </page>\n";
	}
	out += "</pdf2xml>\n";
	return out;
}

static string DocToXml(ClientContext &ctx, const string &path) {
	auto handle = LoadRenderDoc("pdf_to_xml", path, ctx);
	return DocToXml(*handle.doc);
}

static void PdfToXmlFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t path) {
		return StringVector::AddString(result, DocToXml(context, path.GetString()));
	});
}

//===--------------------------------------------------------------------===//
// pdf_to_html(path) — one absolutely-positioned <div class="page"> per page,
// each word an absolutely-positioned <span> at its bbox (PDF points -> px).
//===--------------------------------------------------------------------===//
static string DocToHtml(poppler::document &doc) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	string out = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\"/>\n"
	             "<style>.page{position:relative;border:1px solid #ccc;margin:8px auto;}"
	             ".page span{position:absolute;white-space:pre;}</style>\n</head>\n<body>\n";
	int n = doc.pages();
	for (int p = 0; p < n; p++) {
		unique_ptr<poppler::page> page(doc.create_page(p));
		if (!page) {
			continue;
		}
		auto rect = page->page_rect();
		out += "<div class=\"page\" id=\"page" + std::to_string(p + 1) + "\" style=\"width:" + FmtCoord(rect.width()) +
		       "px;height:" + FmtCoord(rect.height()) + "px;\">\n";
		for (auto &b : page->text_list(poppler::page::text_list_include_font)) {
			auto r = b.bbox();
			double fs = b.has_font_info() ? b.get_font_size() : (r.height() > 0 ? r.height() : 10.0);
			out += "<span style=\"left:" + FmtCoord(r.x()) + "px;top:" + FmtCoord(r.y()) +
			       "px;font-size:" + FmtCoord(fs) + "px;\">";
			AppendXmlEscaped(out, UStringToUtf8(b.text()));
			out += "</span>\n";
		}
		out += "</div>\n";
	}
	out += "</body>\n</html>\n";
	return out;
}

static string DocToHtml(ClientContext &ctx, const string &path) {
	auto handle = LoadRenderDoc("pdf_to_html", path, ctx);
	return DocToHtml(*handle.doc);
}

static void PdfToHtmlFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t path) {
		return StringVector::AddString(result, DocToHtml(context, path.GetString()));
	});
}

//===--------------------------------------------------------------------===//
// pdf_to_svg(path, page[, dpi]) — REAL raster render of ONE 1-based page.
//
// The page is rasterized with poppler::page_renderer (the same render path the
// OCR code uses) and saved to a unique temp file as PNG via image::save(...,
// "png"). poppler's PNG writer uses libpng, which is ALREADY linked through
// poppler — no external process, no new dependency. The PNG bytes are read back,
// base64-encoded, and embedded as a <image href="data:image/png;base64,..."/>
// inside an <svg> sized to the page's POINT dimensions, so the raster scales to
// the page box. The temp file is always removed (RAII guard), even on error.
//===--------------------------------------------------------------------===//

// Portable temp directory (avoids std::filesystem, which is unavailable on some
// of the extension's build targets). Honors the standard env vars, falls back to
// /tmp (POSIX) or the current dir (Windows, where TEMP/TMP are virtually always
// set). Returns a directory path WITHOUT a trailing separator.
static string TempDir() {
	const char *candidates[] = {
#ifdef _WIN32
	    std::getenv("TEMP"), std::getenv("TMP"), "."
#else
	    std::getenv("TMPDIR"), std::getenv("TMP"), "/tmp"
#endif
	};
	for (const char *c : candidates) {
		if (c && *c) {
			string dir(c);
			while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) {
				dir.pop_back();
			}
			return dir;
		}
	}
	return ".";
}

// Shared PNG rasterization + temp-file roundtrip used by BOTH pdf_to_svg (to embed base64 raster)
// and pdf_to_png (to return raw BLOB). Exact same render hints, UUID naming, TempFileGuard,
// image::save + binary read + magic validation as the original pdf_to_svg path.
static string RenderPageToPngBytes(poppler::document &doc, int32_t page_no, int32_t dpi, const string &fn_name) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());

	unique_ptr<poppler::page> page(doc.create_page(page_no - 1));
	if (!page) {
		throw IOException("%s: could not read page %d", fn_name.c_str(), (int)page_no);
	}

	// Rasterize the page (same render hints as the OCR path and original pdf_to_svg).
	poppler::page_renderer renderer;
	renderer.set_render_hint(poppler::page_renderer::antialiasing, true);
	renderer.set_render_hint(poppler::page_renderer::text_antialiasing, true);
	poppler::image img = renderer.render_page(page.get(), dpi, dpi);
	if (!img.is_valid()) {
		throw IOException("%s: failed to render page %d", fn_name.c_str(), (int)page_no);
	}

	// Write the raster to a unique temp file as PNG. image::save() needs a real
	// on-disk path; we build one from a portable temp dir plus a random UUID for a
	// thread-safe, cross-platform unique name, and always remove it on scope exit.
#ifdef _WIN32
	const char sep = '\\';
#else
	const char sep = '/';
#endif
	string unique = BaseUUID::ToString(UUID::GenerateRandomUUID());
	string tmp_path = TempDir() + sep + fn_name + "_" + unique + ".png";
	TempFileGuard guard(tmp_path);

	if (!img.save(tmp_path, "png", dpi)) {
		// poppler returns false if it was built without a PNG writer, or on I/O
		// failure. Fail loudly — never silently degrade.
		throw IOException("%s: poppler could not write a PNG for page %d (this poppler build may lack "
		                  "the PNG image writer)",
		                  fn_name.c_str(), (int)page_no);
	}

	// Read the PNG bytes back.
	std::ifstream in(tmp_path, std::ios::binary);
	if (!in) {
		throw IOException("%s: could not reopen rendered PNG for page %d", fn_name.c_str(), (int)page_no);
	}
	string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();
	if (png.size() < 8 || static_cast<unsigned char>(png[0]) != 0x89 || png[1] != 'P' || png[2] != 'N' ||
	    png[3] != 'G') {
		throw IOException("%s: rendered output for page %d is not a valid PNG", fn_name.c_str(), (int)page_no);
	}
	return png;
}

static string DocToSvg(poppler::document &doc, int32_t page_no, int32_t dpi) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	int n = doc.pages();
	if (page_no < 1 || page_no > n) {
		throw IOException("pdf_to_svg: page %d is out of range (document has %d page%s)", (int)page_no, n,
		                  n == 1 ? "" : "s");
	}
	if (dpi <= 0) {
		throw InvalidInputException("pdf_to_svg: dpi must be positive (got %d)", (int)dpi);
	}
	// Delegate the actual raster + temp PNG + read bytes to the shared helper (no duplication).
	string png = RenderPageToPngBytes(doc, page_no, dpi, "pdf_to_svg");

	// Base64-encode the PNG via DuckDB's blob helper.
	string b64 = Blob::ToBase64(string_t(png.data(), static_cast<uint32_t>(png.size())));

	// We still need a page for the rect; recreate (cheap) or could thread the rect out of helper.
	unique_ptr<poppler::page> page(doc.create_page(page_no - 1));
	if (!page) {
		throw IOException("pdf_to_svg: could not read page %d", (int)page_no);
	}
	auto rect = page->page_rect();
	string w = FmtCoord(rect.width());
	string h = FmtCoord(rect.height());
	string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	out += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" + w +
	       "\" height=\"" + h + "\" viewBox=\"0 0 " + w + " " + h + "\">\n";
	out += "<image width=\"" + w + "\" height=\"" + h + "\" href=\"data:image/png;base64," + b64 + "\"/>\n";
	out += "</svg>\n";
	return out;
}

static string DocToSvg(ClientContext &ctx, const string &path, int32_t page_no, int32_t dpi) {
	auto handle = LoadRenderDoc("pdf_to_svg", path, ctx);
	int n = handle.doc->pages();
	if (page_no < 1 || page_no > n) {
		throw IOException("pdf_to_svg: page %d is out of range for '%s' (document has %d page%s)", (int)page_no, path,
		                  n, n == 1 ? "" : "s");
	}
	if (dpi <= 0) {
		throw InvalidInputException("pdf_to_svg: dpi must be positive (got %d)", (int)dpi);
	}
	return DocToSvg(*handle.doc, page_no, dpi);
}

static void PdfToSvgFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t path, int32_t page_no) {
		    return StringVector::AddString(result, DocToSvg(context, path.GetString(), page_no, 150));
	    });
}
static void PdfToSvgDpiFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	TernaryExecutor::Execute<string_t, int32_t, int32_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t path, int32_t page_no, int32_t dpi) {
		    return StringVector::AddString(result, DocToSvg(context, path.GetString(), page_no, dpi));
	    });
}

//===--------------------------------------------------------------------===//
// BLOB overload scalar functions. These accept PDF content directly as BLOB
// (no path, no VFS). They call LoadBlobDoc then the shared *core* DocToX(doc&, ...)
// implementations so there is zero duplicated rendering logic.
//===--------------------------------------------------------------------===//

static void PdfToTextBlobFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t blob) {
		auto handle = LoadBlobDoc("pdf_to_text", blob.GetDataUnsafe(), blob.GetSize());
		return StringVector::AddString(result, DocToText(*handle.doc, "reading"));
	});
}
static void PdfToTextBlobLayoutFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t blob, string_t layout) {
		    auto handle = LoadBlobDoc("pdf_to_text", blob.GetDataUnsafe(), blob.GetSize());
		    return StringVector::AddString(result, DocToText(*handle.doc, layout.GetString()));
	    });
}

static void PdfToXmlBlobFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t blob) {
		auto handle = LoadBlobDoc("pdf_to_xml", blob.GetDataUnsafe(), blob.GetSize());
		return StringVector::AddString(result, DocToXml(*handle.doc));
	});
}

static void PdfToHtmlBlobFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t blob) {
		auto handle = LoadBlobDoc("pdf_to_html", blob.GetDataUnsafe(), blob.GetSize());
		return StringVector::AddString(result, DocToHtml(*handle.doc));
	});
}

static void PdfToSvgBlobFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t blob, int32_t page_no) {
		    auto handle = LoadBlobDoc("pdf_to_svg", blob.GetDataUnsafe(), blob.GetSize());
		    return StringVector::AddString(result, DocToSvg(*handle.doc, page_no, 150));
	    });
}
static void PdfToSvgBlobDpiFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<string_t, int32_t, int32_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t blob, int32_t page_no, int32_t dpi) {
		    auto handle = LoadBlobDoc("pdf_to_svg", blob.GetDataUnsafe(), blob.GetSize());
		    return StringVector::AddString(result, DocToSvg(*handle.doc, page_no, dpi));
	    });
}

//===--------------------------------------------------------------------===//
// pdf_to_png(path VARCHAR, page INTEGER[, dpi INTEGER]) -> BLOB
// pdf_to_png(data BLOB, page INTEGER[, dpi INTEGER]) -> BLOB
//
// Real raster render of a single 1-based page to PNG bytes (as BLOB).
// Reuses LoadRenderDoc/LoadBlobDoc, RenderPageToPngBytes (the exact raster+
// TempFileGuard+image::save path from pdf_to_svg), and error text conventions.
// Default dpi matches pdf_to_svg (150). Validation 1..2400 per ocr_dpi style.
//===--------------------------------------------------------------------===//

static string DocToPng(poppler::document &doc, int32_t page_no, int32_t dpi) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	int n = doc.pages();
	if (page_no < 1 || page_no > n) {
		throw IOException("pdf_to_png: page %d is out of range (document has %d page%s)", (int)page_no, n,
		                  n == 1 ? "" : "s");
	}
	if (dpi < 1 || dpi > 2400) {
		throw InvalidInputException("pdf_to_png: dpi must be between 1 and 2400 (got %d)", (int)dpi);
	}
	return RenderPageToPngBytes(doc, page_no, dpi, "pdf_to_png");
}

static string DocToPng(ClientContext &ctx, const string &path, int32_t page_no, int32_t dpi) {
	auto handle = LoadRenderDoc("pdf_to_png", path, ctx);
	int n = handle.doc->pages();
	if (page_no < 1 || page_no > n) {
		throw IOException("pdf_to_png: page %d is out of range for '%s' (document has %d page%s)", (int)page_no, path,
		                  n, n == 1 ? "" : "s");
	}
	if (dpi < 1 || dpi > 2400) {
		throw InvalidInputException("pdf_to_png: dpi must be between 1 and 2400 (got %d)", (int)dpi);
	}
	return DocToPng(*handle.doc, page_no, dpi);
}

static void PdfToPngFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t path, int32_t page_no) {
		    auto png = DocToPng(context, path.GetString(), page_no, 150);
		    return StringVector::AddStringOrBlob(result, string_t(png.data(), static_cast<uint32_t>(png.size())));
	    });
}
static void PdfToPngDpiFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	TernaryExecutor::Execute<string_t, int32_t, int32_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t path, int32_t page_no, int32_t dpi) {
		    auto png = DocToPng(context, path.GetString(), page_no, dpi);
		    return StringVector::AddStringOrBlob(result, string_t(png.data(), static_cast<uint32_t>(png.size())));
	    });
}

static void PdfToPngBlobFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t blob, int32_t page_no) {
		    auto handle = LoadBlobDoc("pdf_to_png", blob.GetDataUnsafe(), blob.GetSize());
		    auto png = DocToPng(*handle.doc, page_no, 150);
		    return StringVector::AddStringOrBlob(result, string_t(png.data(), static_cast<uint32_t>(png.size())));
	    });
}
static void PdfToPngBlobDpiFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<string_t, int32_t, int32_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t blob, int32_t page_no, int32_t dpi) {
		    auto handle = LoadBlobDoc("pdf_to_png", blob.GetDataUnsafe(), blob.GetSize());
		    auto png = DocToPng(*handle.doc, page_no, dpi);
		    return StringVector::AddStringOrBlob(result, string_t(png.data(), static_cast<uint32_t>(png.size())));
	    });
}

//===--------------------------------------------------------------------===//
// to_pdf(input_path[, output_path]) — convert an office/markup document
// (docx, doc, odt, rtf, html, odp, pptx, xlsx, ...) to a PDF by shelling out AT
// RUNTIME to LibreOffice's `soffice`.
//
// This is a RUNTIME process spawn (popen), NOT a build/link dependency: the
// extension compiles and CI stays green on machines with no converter
// installed. If LibreOffice is absent at call time, we throw a clear, actionable
// IOException (mirroring the graceful-require pattern the OCR path uses).
//===--------------------------------------------------------------------===//

// Removes a directory tree on scope exit (best-effort; never throws). Used for
// the per-call LibreOffice user-profile dir and the temp output dir.
struct TempDirGuard {
	std::string path;
	explicit TempDirGuard(std::string p) : path(std::move(p)) {
	}
	~TempDirGuard() {
		if (path.empty()) {
			return;
		}
		// Recursive remove via the OS shell — portable and dependency-free. Quote
		// the path; best-effort, so ignore the result.
#ifdef _WIN32
		string cmd = "rmdir /s /q \"" + path + "\" >nul 2>&1";
#else
		string cmd = "rm -rf \"" + path + "\" >/dev/null 2>&1";
#endif
		int rc = std::system(cmd.c_str());
		(void)rc;
	}
	TempDirGuard(const TempDirGuard &) = delete;
	TempDirGuard &operator=(const TempDirGuard &) = delete;
};

// True if `path` names an existing executable file.
static bool IsExecutable(const string &path) {
	if (path.empty()) {
		return false;
	}
#ifdef _WIN32
	return _access(path.c_str(), 0) == 0; // existence; Windows has no X bit
#else
	return access(path.c_str(), X_OK) == 0;
#endif
}

// Resolve a bare command name (no separator) against the entries of $PATH,
// returning the first executable match, or empty string if none.
static string WhichOnPath(const string &name) {
	const char *path_env = std::getenv("PATH");
	if (!path_env || !*path_env) {
		return string();
	}
#ifdef _WIN32
	const char list_sep = ';';
	const char dir_sep = '\\';
#else
	const char list_sep = ':';
	const char dir_sep = '/';
#endif
	string paths(path_env);
	size_t start = 0;
	while (start <= paths.size()) {
		size_t end = paths.find(list_sep, start);
		string dir = (end == string::npos) ? paths.substr(start) : paths.substr(start, end - start);
		if (!dir.empty()) {
			string candidate = dir;
			if (candidate.back() != dir_sep) {
				candidate += dir_sep;
			}
			candidate += name;
			if (IsExecutable(candidate)) {
				return candidate;
			}
		}
		if (end == string::npos) {
			break;
		}
		start = end + 1;
	}
	return string();
}

// Locate the LibreOffice binary (graceful-require). Resolution order, first hit
// wins:
//   1. env LIBREOFFICE_PATH  (explicit absolute path to the binary)
//   2. `soffice` on $PATH
//   3. `libreoffice` on $PATH
//   4. macOS app bundle: /Applications/LibreOffice.app/Contents/MacOS/soffice
//   5. none -> throw a clear, actionable IOException naming LibreOffice + install.
static string ResolveLibreOffice() {
	const char *explicit_path = std::getenv("LIBREOFFICE_PATH");
	if (explicit_path && *explicit_path && IsExecutable(explicit_path)) {
		return string(explicit_path);
	}
	string soffice = WhichOnPath("soffice");
	if (!soffice.empty()) {
		return soffice;
	}
	string libreoffice = WhichOnPath("libreoffice");
	if (!libreoffice.empty()) {
		return libreoffice;
	}
	static const char *kBundle = "/Applications/LibreOffice.app/Contents/MacOS/soffice";
	if (IsExecutable(kBundle)) {
		return string(kBundle);
	}
	throw IOException(
	    "to_pdf: could not find a LibreOffice converter. Install LibreOffice — macOS: `brew install --cask "
	    "libreoffice`; Debian/Ubuntu: `apt-get install libreoffice`; Windows: https://www.libreoffice.org/download. "
	    "The `soffice`/`libreoffice` binary is auto-detected on $PATH (and the macOS app bundle); if yours is in a "
	    "non-standard location, set the LIBREOFFICE_PATH environment variable to the binary's absolute path.");
}

// Split a path into directory, basename-without-extension, and extension.
// Coordinate-system-free string surgery (no std::filesystem, which does not
// compile on this target). A path with no directory separator has empty `dir`.
static void SplitPath(const string &path, string &dir, string &stem, string &ext) {
	size_t slash = path.find_last_of("/\\");
	string fname = (slash == string::npos) ? path : path.substr(slash + 1);
	dir = (slash == string::npos) ? string() : path.substr(0, slash);
	size_t dot = fname.find_last_of('.');
	if (dot == string::npos || dot == 0) {
		stem = fname;
		ext = string();
	} else {
		stem = fname.substr(0, dot);
		ext = fname.substr(dot); // includes the leading '.'
	}
}

// Run a command line, capturing combined stdout+stderr, returning the child's
// exit status. Uses popen/_popen — a runtime process spawn, not a linked lib.
static int RunCapture(const string &cmd, string &output) {
	output.clear();
	FILE *pipe = popen(cmd.c_str(), "r");
	if (!pipe) {
		throw IOException("to_pdf: failed to spawn the LibreOffice converter process");
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
		output.append(buf, n);
	}
	return pclose(pipe);
}

// Convert `input_path` to a PDF written at `output_path`, returning output_path.
// LibreOffice cannot name its output directly (it writes
// <outdir>/<input-stem>.pdf), so we convert into a unique temp outdir and then
// move the produced file to `output_path`. A unique per-call user-profile dir
// avoids the "another LibreOffice instance is running" lock.
static string ConvertToPdf(const string &input_path, const string &output_path) {
	{
		std::ifstream input_check(input_path.c_str(), std::ios::binary);
		if (!input_check.good()) {
			throw IOException("to_pdf: input file '%s' does not exist or is not readable", input_path);
		}
	}
	string soffice = ResolveLibreOffice();

#ifdef _WIN32
	const char sep = '\\';
#else
	const char sep = '/';
#endif
	string base = TempDir();
	string unique = BaseUUID::ToString(UUID::GenerateRandomUUID());
	string profile_dir = base + sep + "to_pdf_profile_" + unique;
	string out_dir = base + sep + "to_pdf_out_" + unique;
	TempDirGuard profile_guard(profile_dir);
	TempDirGuard out_guard(out_dir);

	// Build the command. UserInstallation must be a file:// URL. Quote all paths.
	// Capture combined output (2>&1) so a failure surfaces soffice's diagnostics.
	string cmd = "\"" + soffice + "\" --headless -env:UserInstallation=file://" + profile_dir +
	             " --convert-to pdf --outdir \"" + out_dir + "\" \"" + input_path + "\" 2>&1";
	string captured;
	int rc = RunCapture(cmd, captured);

	// LibreOffice names the output <out_dir>/<input-stem>.pdf.
	string in_dir, in_stem, in_ext;
	SplitPath(input_path, in_dir, in_stem, in_ext);
	string produced = out_dir + sep + in_stem + ".pdf";

	std::ifstream check(produced, std::ios::binary);
	if (!check.good()) {
		throw IOException("to_pdf: LibreOffice did not produce a PDF for '%s' (exit status %d). soffice output: %s",
		                  input_path, rc, captured.empty() ? "(none)" : captured);
	}

	// Validate the produced file starts with the PDF magic bytes; fail loud
	// otherwise (mirrors the PNG-magic check in pdf_to_svg).
	char magic[5] = {0};
	check.read(magic, 5);
	check.close();
	if (string(magic, 5) != "%PDF-") {
		throw IOException("to_pdf: the file LibreOffice produced for '%s' is not a valid PDF (missing %%PDF- header)",
		                  input_path);
	}

	// Move produced -> output_path. std::rename fails across filesystems, so fall
	// back to a byte copy + remove of the source.
	std::remove(output_path.c_str());
	if (std::rename(produced.c_str(), output_path.c_str()) != 0) {
		std::ifstream src(produced, std::ios::binary);
		std::ofstream dst(output_path, std::ios::binary | std::ios::trunc);
		if (!src || !dst) {
			throw IOException("to_pdf: could not write the converted PDF to '%s'", output_path);
		}
		dst << src.rdbuf();
		if (!dst.good()) {
			throw IOException("to_pdf: failed while writing the converted PDF to '%s'", output_path);
		}
		src.close();
		dst.close();
		std::remove(produced.c_str());
	}
	return output_path;
}

// Default output path: the input path with its extension swapped to `.pdf`.
static string DefaultPdfOutput(const string &input_path) {
	string dir, stem, ext;
	SplitPath(input_path, dir, stem, ext);
#ifdef _WIN32
	const char sep = '\\';
#else
	const char sep = '/';
#endif
	string out = dir.empty() ? stem : (dir + sep + stem);
	out += ".pdf";
	return out;
}

static void ToPdfFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		string input_path = in.GetString();
		return StringVector::AddString(result, ConvertToPdf(input_path, DefaultPdfOutput(input_path)));
	});
}
static void ToPdfOutFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t in, string_t out) {
		    return StringVector::AddString(result, ConvertToPdf(in.GetString(), out.GetString()));
	    });
}

//===--------------------------------------------------------------------===//
// write_pdf(content [, output_path]) — NATIVE in-process PDF author using libharu.
// Two overloads:
//   write_pdf(content) -> writes to temp .pdf (TempDir + UUID), returns path
//   write_pdf(content, output_path) -> writes to given path, returns it
// NULL (content or output_path) propagates to NULL result (via executor).
// Rendering: Letter (612x792), 0.75in=54pt margins, Helvetica 10pt, \n-split +
// word-wrap + auto-paginate. Tabs expanded to 4 spaces. Empty content yields
// one blank (valid) page. On any libharu or I/O failure: IOException (no silent
// truncation).
//===--------------------------------------------------------------------===//

// libharu error context for the C callback.
struct HpdfError {
	HPDF_STATUS error_no = 0;
	HPDF_STATUS detail_no = 0;
};

static void HpdfErrorHandler(HPDF_STATUS error_no, HPDF_STATUS detail_no, void *user_data) {
	if (user_data) {
		auto *e = static_cast<HpdfError *>(user_data);
		e->error_no = error_no;
		e->detail_no = detail_no;
	}
}

// RAII: guarantee HPDF_Free even if we throw.
struct HpdfDocGuard {
	HPDF_Doc pdf = nullptr;
	explicit HpdfDocGuard(HPDF_Doc p) : pdf(p) {
	}
	~HpdfDocGuard() {
		if (pdf) {
			HPDF_Free(pdf);
		}
	}
	HpdfDocGuard(const HpdfDocGuard &) = delete;
	HpdfDocGuard &operator=(const HpdfDocGuard &) = delete;
};

static string ExpandTabs(const string &s) {
	string r;
	r.reserve(s.size() + (s.size() / 4));
	for (char c : s) {
		if (c == '\t') {
			r += "    ";
		} else {
			r.push_back(c);
		}
	}
	return r;
}

// Word-wrap a single logical line using the page's current font metrics.
static std::vector<string> WrapLine(HPDF_Page page, const string &line, double max_width) {
	std::vector<string> out;
	string l = line;
	// strip trailing CR from \r\n
	if (!l.empty() && l.back() == '\r') {
		l.pop_back();
	}
	if (l.empty()) {
		out.emplace_back();
		return out;
	}
	std::istringstream iss(l);
	std::vector<string> words;
	string w;
	while (iss >> w) {
		words.push_back(std::move(w));
	}
	if (words.empty()) {
		out.emplace_back();
		return out;
	}
	string cur;
	for (const auto &word : words) {
		string trial = cur.empty() ? word : (cur + " " + word);
		double tw = HPDF_Page_TextWidth(page, trial.c_str());
		if (tw > max_width && !cur.empty()) {
			out.push_back(cur);
			cur = word;
		} else {
			cur = std::move(trial);
		}
	}
	if (!cur.empty()) {
		out.push_back(cur);
	}
	return out;
}

struct PdfWriteOptions {
	string title;
	string author;
	string header;
	string footer;
	double font_size = 10.0;
	string page_size = "letter";
	double margin = 54.0;
};

// Core text->PDF renderer using libharu.
// Extracted so both write_pdf scalar AND the COPY (FORMAT pdf) CopyFunction
// finalize can use exactly the same logic with no behavior divergence.
// Defaults (letter/10pt/54pt/no meta/hf) exactly reproduce prior behavior for BWC.
static void RenderTextToPdf(const string &content, const string &output_path,
                            const PdfWriteOptions &opts = PdfWriteOptions()) {
	HpdfError err_ctx;
	HPDF_Doc pdf = HPDF_New(HpdfErrorHandler, &err_ctx);
	if (!pdf) {
		throw IOException("write_pdf: HPDF_New failed to create document (libharu error %u:%u).", err_ctx.error_no,
		                  err_ctx.detail_no);
	}
	HpdfDocGuard guard(pdf);

	HPDF_STATUS st;

	// Set metadata (for COPY options; write_pdf defaults are empty -> no change)
	if (!opts.title.empty()) {
		HPDF_SetInfoAttr(pdf, HPDF_INFO_TITLE, opts.title.c_str());
	}
	if (!opts.author.empty()) {
		HPDF_SetInfoAttr(pdf, HPDF_INFO_AUTHOR, opts.author.c_str());
	}

	// Resolve page size (support letter/a4/legal); use HPDF_* enums for SetSize, known dims for layout math
	string ps = StringUtil::Lower(opts.page_size);
	double PAGE_W;
	double PAGE_H;
	HPDF_PageSizes hpdf_size;
	if (ps == "letter") {
		PAGE_W = 612.0;
		PAGE_H = 792.0;
		hpdf_size = HPDF_PAGE_SIZE_LETTER;
	} else if (ps == "a4") {
		PAGE_W = 595.276;
		PAGE_H = 841.89;
		hpdf_size = HPDF_PAGE_SIZE_A4;
	} else if (ps == "legal") {
		PAGE_W = 612.0;
		PAGE_H = 1008.0;
		hpdf_size = HPDF_PAGE_SIZE_LEGAL;
	} else {
		// Should be validated upstream for COPY; for safety fall back (keeps BWC for write_pdf default)
		PAGE_W = 612.0;
		PAGE_H = 792.0;
		hpdf_size = HPDF_PAGE_SIZE_LETTER;
	}

	const double MARGIN = opts.margin;
	const double TEXT_W = PAGE_W - 2.0 * MARGIN;
	const double TOP = PAGE_H - MARGIN;
	const double BOTTOM = MARGIN;
	const double LINE_H = opts.font_size + 4.0; // preserve 14pt leading at default 10pt

	HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", NULL);
	if (!font) {
		throw IOException("write_pdf: HPDF_GetFont('Helvetica') failed.");
	}

	// Header/footer font size (font_size-2, min 6) used inside margin bands
	auto draw_centered = [&](HPDF_Page p, const string &text, double y, double fs) {
		if (text.empty())
			return;
		HPDF_Page_SetFontAndSize(p, font, fs);
		double tw = HPDF_Page_TextWidth(p, text.c_str());
		double x = (PAGE_W - tw) / 2.0;
		HPDF_STATUS bst = HPDF_Page_BeginText(p);
		if (bst != HPDF_OK) {
			throw IOException("write_pdf: HPDF_Page_BeginText failed (status %u).", bst);
		}
		bst = HPDF_Page_TextOut(p, x, y, text.c_str());
		if (bst != HPDF_OK) {
			throw IOException("write_pdf: HPDF_Page_TextOut failed (status %u).", bst);
		}
		bst = HPDF_Page_EndText(p);
		if (bst != HPDF_OK) {
			throw IOException("write_pdf: HPDF_Page_EndText failed (status %u).", bst);
		}
	};

	int page_num = 0;

	auto create_page = [&](HPDF_Page &pg) {
		pg = HPDF_AddPage(pdf);
		page_num++;
		HPDF_STATUS st = HPDF_Page_SetSize(pg, hpdf_size, HPDF_PAGE_PORTRAIT);
		if (st != HPDF_OK) {
			throw IOException("write_pdf: HPDF_Page_SetSize on new page failed (status %u).", st);
		}
		HPDF_Page_SetFontAndSize(pg, font, opts.font_size);
		// draw header inside top margin band (if any)
		if (!opts.header.empty()) {
			double hfsz = std::max(6.0, opts.font_size - 2.0);
			draw_centered(pg, opts.header, PAGE_H - (MARGIN / 2.0), hfsz);
			HPDF_Page_SetFontAndSize(pg, font, opts.font_size); // restore for body text
		}
	};

	auto draw_footer = [&](HPDF_Page pg) {
		if (opts.footer.empty())
			return;
		double hfsz = std::max(6.0, opts.font_size - 2.0);
		string ftext = opts.footer;
		// clean optional {page} placeholder support
		size_t pos = 0;
		while ((pos = ftext.find("{page}", pos)) != string::npos) {
			ftext.replace(pos, 6, std::to_string(page_num));
			pos += 1;
		}
		draw_centered(pg, ftext, MARGIN / 2.0, hfsz);
		HPDF_Page_SetFontAndSize(pg, font, opts.font_size); // restore (harmless if no body after)
	};

	HPDF_Page page = nullptr;
	create_page(page);

	double y = TOP;

	string expanded = ExpandTabs(content);
	std::istringstream lines(expanded);
	string line;
	bool emitted_any = false;

	while (std::getline(lines, line)) {
		auto wrapped = WrapLine(page, line, TEXT_W);
		for (const auto &wline : wrapped) {
			emitted_any = true;
			if (y < BOTTOM + LINE_H * 0.5) {
				draw_footer(page);
				create_page(page);
				y = TOP;
			}
			st = HPDF_Page_BeginText(page);
			if (st != HPDF_OK) {
				throw IOException("write_pdf: HPDF_Page_BeginText failed (status %u).", st);
			}
			st = HPDF_Page_TextOut(page, MARGIN, y, wline.c_str());
			if (st != HPDF_OK) {
				throw IOException("write_pdf: HPDF_Page_TextOut failed (status %u).", st);
			}
			st = HPDF_Page_EndText(page);
			if (st != HPDF_OK) {
				throw IOException("write_pdf: HPDF_Page_EndText failed (status %u).", st);
			}
			y -= LINE_H;
		}
	}

	// always draw footer on the (last) page; header already drawn at creation
	draw_footer(page);

	if (!emitted_any) {
		// empty content: one blank page (with possible header/footer) is already present — valid PDF
	}

	st = HPDF_SaveToFile(pdf, output_path.c_str());
	if (st != HPDF_OK) {
		throw IOException("write_pdf: HPDF_SaveToFile('%s') failed (status %u) — check that the path is writable and "
		                  "the parent directory exists.",
		                  output_path.c_str(), st);
	}

	// guard ~ calls HPDF_Free
}

static string WritePdfImpl(const string &content, const string &output_path) {
	RenderTextToPdf(content, output_path);
	return output_path;
}

static void WritePdfFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t content) {
#ifdef _WIN32
		const char sep = '\\';
#else
		const char sep = '/';
#endif
		string unique = BaseUUID::ToString(UUID::GenerateRandomUUID());
		string tmp_path = TempDir() + sep + "write_pdf_" + unique + ".pdf";
		return StringVector::AddString(result, WritePdfImpl(content.GetString(), tmp_path));
	});
}

static void WritePdfOutFun(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t content, string_t outpath) {
		    return StringVector::AddString(result, WritePdfImpl(content.GetString(), outpath.GetString()));
	    });
}

//===--------------------------------------------------------------------===//
// COPY ... TO 'file.pdf' (FORMAT pdf) — uses the same RenderTextToPdf core.
// Produces one text line per input row (columns joined by single space; NULLs become empty).
// Accumulates in global buffer (mutex-protected), writes via Render in finalize.
// Only copy_to (write); row order preserved by forcing REGULAR (serial) execution.
//===--------------------------------------------------------------------===//

struct PdfCopyBindData : public TableFunctionData {
	string file_path;
	vector<string> column_names;
	vector<LogicalType> column_types;
	PdfWriteOptions write_opts;
};

struct PdfCopyGlobalState : public GlobalFunctionData {
	string buffer;
	mutex lock;
};

struct PdfCopyLocalState : public LocalFunctionData {
	string buffer;
};

static string PdfGetStringOption(const vector<Value> &set, const string &lopt) {
	if (set.empty()) {
		return string();
	}
	Value v = set[0];
	if (v.type().id() == LogicalTypeId::LIST) {
		auto children = ListValue::GetChildren(v);
		if (children.empty()) {
			return string();
		}
		v = children[0];
	}
	if (v.IsNull()) {
		return string();
	}
	if (v.type().id() != LogicalTypeId::VARCHAR) {
		throw BinderException("\"%s\" expects a string argument", lopt.c_str());
	}
	return v.GetValue<string>();
}

static double PdfGetDoubleOption(const vector<Value> &set, const string &lopt) {
	if (set.empty()) {
		throw BinderException("\"%s\" expects a numeric argument", lopt.c_str());
	}
	Value v = set[0];
	if (v.type().id() == LogicalTypeId::LIST) {
		auto children = ListValue::GetChildren(v);
		if (children.empty()) {
			throw BinderException("\"%s\" expects a numeric argument", lopt.c_str());
		}
		v = children[0];
	}
	if (v.IsNull()) {
		throw BinderException("\"%s\" expects a numeric argument", lopt.c_str());
	}
	try {
		Value d = v.DefaultCastAs(LogicalType::DOUBLE);
		return d.GetValue<double>();
	} catch (...) {
		throw BinderException("\"%s\" expects a numeric value", lopt.c_str());
	}
}

static unique_ptr<FunctionData> PdfCopyBind(ClientContext &context, CopyFunctionBindInput &input,
                                            const vector<string> &names, const vector<LogicalType> &sql_types) {
	auto result = make_uniq<PdfCopyBindData>();
	result->file_path = input.info.file_path;
	result->column_names = names;
	result->column_types = sql_types;

	// Parse (case-insensitive keys: DuckDB populates via Lower in places; we Lower explicitly)
	// Unknown options now throw (house style); previously were silently ignored.
	for (auto &option : input.info.options) {
		string loption = StringUtil::Lower(option.first);
		auto &set = option.second;
		if (loption == "title") {
			result->write_opts.title = PdfGetStringOption(set, loption);
		} else if (loption == "author") {
			result->write_opts.author = PdfGetStringOption(set, loption);
		} else if (loption == "header") {
			result->write_opts.header = PdfGetStringOption(set, loption);
		} else if (loption == "footer") {
			result->write_opts.footer = PdfGetStringOption(set, loption);
		} else if (loption == "font_size") {
			double fs = PdfGetDoubleOption(set, loption);
			if (fs < 4.0 || fs > 72.0) {
				throw BinderException("font_size must be between 4 and 72");
			}
			result->write_opts.font_size = fs;
		} else if (loption == "page_size") {
			string ps = StringUtil::Lower(PdfGetStringOption(set, loption));
			if (ps != "letter" && ps != "a4" && ps != "legal") {
				throw BinderException("page_size must be one of letter, a4, legal");
			}
			result->write_opts.page_size = ps;
		} else if (loption == "margin") {
			double m = PdfGetDoubleOption(set, loption);
			if (m < 0.0 || m > 216.0) {
				throw BinderException("margin must be between 0 and 216");
			}
			result->write_opts.margin = m;
		} else {
			throw BinderException("Unrecognized option \"%s\" for COPY ... (FORMAT pdf)", option.first.c_str());
		}
	}

	// After options: if header or footer present, margin must be large enough to fit inside band
	if ((!result->write_opts.header.empty() || !result->write_opts.footer.empty()) &&
	    result->write_opts.margin < 24.0) {
		throw BinderException("margin too small for header/footer");
	}

	return std::move(result);
}

static unique_ptr<GlobalFunctionData> PdfCopyInitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                              const string &file_path) {
	auto result = make_uniq<PdfCopyGlobalState>();
	return std::move(result);
}

static unique_ptr<LocalFunctionData> PdfCopyInitializeLocal(ExecutionContext &context, FunctionData &bind_data) {
	return make_uniq<PdfCopyLocalState>();
}

static void PdfCopySink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                        LocalFunctionData &lstate, DataChunk &input) {
	auto &global = gstate.Cast<PdfCopyGlobalState>();
	lock_guard<mutex> glock(global.lock);

	idx_t ncols = input.ColumnCount();
	for (idx_t r = 0; r < input.size(); r++) {
		string line;
		for (idx_t c = 0; c < ncols; c++) {
			if (c > 0) {
				line += " "; // columns space-separated (documented)
			}
			Value val = input.GetValue(c, r);
			if (val.IsNull()) {
				continue;
			}
			try {
				Value str_val = val.DefaultCastAs(LogicalType::VARCHAR);
				if (!str_val.IsNull()) {
					line += StringValue::Get(str_val);
				}
			} catch (...) {
				line += val.ToString();
			}
		}
		line += "\n";
		global.buffer += line;
	}
}

static void PdfCopyCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                           LocalFunctionData &lstate) {
	// No-op: we append directly under lock in sink for simplicity and order safety.
	// (Per-local buffering + concat would also work; direct-to-global + lock is robust here.)
}

static CopyFunctionExecutionMode PdfCopyExecutionMode(bool preserve_insertion_order, bool supports_batch_index) {
	// Force serial to guarantee row order in the emitted text lines.
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

static void PdfCopyFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &global = gstate.Cast<PdfCopyGlobalState>();
	auto &bind = bind_data.Cast<PdfCopyBindData>();
	string path = bind.file_path;
	if (path.empty()) {
		throw IOException("COPY ... TO (FORMAT pdf): output file path is empty");
	}
	RenderTextToPdf(global.buffer, path, bind.write_opts);
}

//===--------------------------------------------------------------------===//
// pdf_to_markdown(path) — layout-aware GitHub-flavoured Markdown using
// poppler's positioned word list + font info (no AI). Headings by relative
// size, tables via ReconstructPageGrid, bold via "Bold" in font, lists by
// prefix, paragraphs by vertical gap. Deterministic geometry only.
//===--------------------------------------------------------------------===//

struct MdWord {
	double xMin = 0.0;
	double yMin = 0.0;
	double xMax = 0.0;
	double yMax = 0.0;
	string text;
	string font_name;
	double font_size = 0.0;
};

static string DocToMarkdown(ClientContext &context, const string &path) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	auto handle = LoadRenderDoc("pdf_to_markdown", path, context);
	auto &doc = handle.doc;
	int n = doc->pages();

	std::vector<std::vector<MdWord>> pages_words(n);
	std::vector<double> all_font_sizes;
	for (int p = 0; p < n; p++) {
		unique_ptr<poppler::page> page(doc->create_page(p));
		if (!page) {
			continue;
		}
		std::vector<MdWord> page_words;
		for (auto &b : page->text_list(poppler::page::text_list_include_font)) {
			auto r = b.bbox();
			MdWord w;
			w.xMin = r.x();
			w.yMin = r.y();
			w.xMax = r.x() + r.width();
			w.yMax = r.y() + r.height();
			w.text = UStringToUtf8(b.text());
			if (b.has_font_info()) {
				w.font_name = b.get_font_name();
				w.font_size = b.get_font_size();
			} else {
				w.font_size = (r.height() > 0 ? r.height() : 10.0);
			}
			if (w.font_size > 0) {
				all_font_sizes.push_back(w.font_size);
			}
			page_words.push_back(std::move(w));
		}
		pages_words[p] = std::move(page_words);
	}

	// Phase 2: body = mode of sizes rounded to nearest 0.5pt; smallest on tie
	double body_size = 10.0;
	if (!all_font_sizes.empty()) {
		std::vector<double> rounded;
		rounded.reserve(all_font_sizes.size());
		for (double fs : all_font_sizes) {
			rounded.push_back(std::round(fs * 2.0) / 2.0);
		}
		std::sort(rounded.begin(), rounded.end());
		double cur = rounded[0];
		int cnt = 1;
		int best_cnt = 1;
		double best = cur;
		for (size_t k = 1; k < rounded.size(); ++k) {
			if (std::fabs(rounded[k] - cur) < 0.001) {
				++cnt;
			} else {
				if (cnt > best_cnt || (cnt == best_cnt && cur < best)) {
					best_cnt = cnt;
					best = cur;
				}
				cur = rounded[k];
				cnt = 1;
			}
		}
		if (cnt > best_cnt || (cnt == best_cnt && cur < best)) {
			best = cur;
		}
		body_size = best;
	}

	// Phase 3: distinct heading sizes (>=1.15*body), sorted descending
	std::vector<double> heading_sizes;
	for (double fs : all_font_sizes) {
		double r = std::round(fs * 2.0) / 2.0;
		if (r >= 1.15 * body_size) {
			bool has = false;
			for (double v : heading_sizes) {
				if (std::fabs(v - r) < 0.01) {
					has = true;
					break;
				}
			}
			if (!has) {
				heading_sizes.push_back(r);
			}
		}
	}
	std::sort(heading_sizes.begin(), heading_sizes.end());
	std::reverse(heading_sizes.begin(), heading_sizes.end());

	auto get_heading_level = [&](double fs) -> int {
		double r = std::round(fs * 2.0) / 2.0;
		for (size_t i = 0; i < heading_sizes.size(); ++i) {
			if (std::fabs(r - heading_sizes[i]) < 0.01) {
				return (int)std::min<size_t>(i + 1, 4);
			}
		}
		return 0;
	};

	auto join_plain = [](const std::vector<MdWord> &row) -> string {
		string s;
		for (size_t i = 0; i < row.size(); ++i) {
			if (i > 0)
				s += " ";
			s += row[i].text;
		}
		return s;
	};

	auto format_row = [](const std::vector<MdWord> &row) -> string {
		string s;
		size_t i = 0;
		size_t m = row.size();
		while (i < m) {
			bool is_bold = row[i].font_name.find("Bold") != string::npos;
			if (is_bold) {
				string bspan;
				size_t j = i;
				while (j < m && row[j].font_name.find("Bold") != string::npos) {
					if (!bspan.empty())
						bspan += " ";
					bspan += row[j].text;
					++j;
				}
				if (!s.empty())
					s += " ";
				s += "**" + bspan + "**";
				i = j;
			} else {
				if (!s.empty())
					s += " ";
				s += row[i].text;
				++i;
			}
		}
		return s;
	};

	auto escape_pipe = [](const string &s) -> string {
		string o;
		o.reserve(s.size() + 4);
		for (char c : s) {
			if (c == '|')
				o += "\\|";
			else
				o += c;
		}
		return o;
	};

	auto emit_grid_table = [&](const std::vector<std::vector<string>> &grid, string &out_ref) {
		if (grid.empty())
			return;
		out_ref += "|";
		for (const auto &c : grid[0]) {
			out_ref += escape_pipe(c) + "|";
		}
		out_ref += "\n|";
		for (size_t c = 0; c < grid[0].size(); ++c) {
			out_ref += "---|";
		}
		out_ref += "\n";
		for (size_t ri = 1; ri < grid.size(); ++ri) {
			out_ref += "|";
			for (const auto &c : grid[ri]) {
				out_ref += escape_pipe(c) + "|";
			}
			out_ref += "\n";
		}
		out_ref += "\n";
	};

	string out;
	for (int p = 0; p < n; p++) {
		if (p > 0)
			out += "\n\n";
		auto page_words = std::move(pages_words[p]);
		if (page_words.empty())
			continue;

		// per-page row clustering (same rules as ReconstructPageGrid)
		std::vector<double> heights;
		for (const auto &w : page_words) {
			double h = w.yMax - w.yMin;
			if (h > 0)
				heights.push_back(h);
		}
		double med_h = Median(heights);
		if (med_h <= 0)
			med_h = 10.0;
		double row_tol = med_h * 0.5;

		std::sort(page_words.begin(), page_words.end(),
		          [](const MdWord &a, const MdWord &b) { return a.yMin < b.yMin; });

		std::vector<std::vector<MdWord>> rows;
		{
			std::vector<MdWord> cur;
			double row_anchor = page_words.front().yMin;
			for (auto &w : page_words) {
				if (cur.empty()) {
					cur.push_back(w);
					row_anchor = w.yMin;
				} else if (std::fabs(w.yMin - row_anchor) <= row_tol) {
					cur.push_back(w);
				} else {
					rows.push_back(cur);
					cur.clear();
					cur.push_back(w);
					row_anchor = w.yMin;
				}
			}
			if (!cur.empty())
				rows.push_back(cur);
		}
		for (auto &r : rows) {
			std::sort(r.begin(), r.end(), [](const MdWord &a, const MdWord &b) { return a.xMin < b.xMin; });
		}

		// table detection + y-suppression zone(s)
		// Collect words only from blocks of *consecutive* rows that have large
		// internal x-gaps (column-like spacing >100pt). This lets ReconstructPageGrid
		// see a clean >=3-row multi-col block even on mixed pages (prose sentences
		// and title have gaps too but isolated or <3 rows after y cluster -> gate fails).
		std::vector<PdfWord> pw_vec;
		std::vector<MdWord> cur_b;
		int best_block_rows = 0;
		for (size_t i = 0; i <= rows.size(); ++i) {
			bool is_end = (i == rows.size());
			bool is_cand = false;
			if (!is_end) {
				const auto &r = rows[i];
				if (r.size() >= 2) {
					std::vector<double> xs;
					for (const auto &w : r)
						xs.push_back(w.xMin);
					std::sort(xs.begin(), xs.end());
					double mg = 0;
					for (size_t j = 1; j < xs.size(); ++j)
						mg = std::max(mg, xs[j] - xs[j - 1]);
					if (mg > 100.0)
						is_cand = true;
				}
			}
			if (is_cand) {
				cur_b.insert(cur_b.end(), rows[i].begin(), rows[i].end());
			} else if (!cur_b.empty()) {
				std::vector<PdfWord> test_pw;
				for (const auto &m : cur_b) {
					PdfWord pw;
					pw.xMin = m.xMin;
					pw.yMin = m.yMin;
					pw.xMax = m.xMax;
					pw.yMax = m.yMax;
					pw.text = m.text;
					test_pw.push_back(pw);
				}
				auto test_g = ReconstructPageGrid(test_pw);
				if ((int)test_g.size() > best_block_rows && (int)test_g.size() >= 3) {
					best_block_rows = (int)test_g.size();
					pw_vec = std::move(test_pw);
				}
				cur_b.clear();
			}
		}
		auto grid = ReconstructPageGrid(pw_vec);
		std::vector<std::pair<double, double>> table_zones;
		// geometric y-range from the words of the detected table block (pw_vec);
		// a prose line is suppressed iff its y-interval overlaps a table zone.
		// (no text-value matching of cell contents)
		if (!pw_vec.empty()) {
			double gy0 = 1e9, gy1 = -1e9;
			for (const auto &w : pw_vec) {
				gy0 = std::min(gy0, w.yMin);
				gy1 = std::max(gy1, w.yMax);
			}
			if (gy0 < 1e8) {
				table_zones.emplace_back(gy0, gy1);
			}
		}

		// per-row classification + emission
		string para_buf;
		double prev_ymax = -1e9;
		bool table_emitted = false;
		for (const auto &row : rows) {
			if (row.empty())
				continue;
			double rymin = 1e9, rymax = -1e9;
			for (const auto &w : row) {
				rymin = std::min(rymin, w.yMin);
				rymax = std::max(rymax, w.yMax);
			}

			bool suppressed = false;
			for (const auto &z : table_zones) {
				if (rymin >= z.first - row_tol && rymin <= z.second + row_tol) {
					suppressed = true;
					break;
				}
			}
			if (suppressed) {
				if (!para_buf.empty()) {
					out += para_buf + "\n\n";
					para_buf.clear();
				}
				if (!grid.empty() && !table_emitted) {
					emit_grid_table(grid, out);
					table_emitted = true;
				}
				prev_ymax = -1e9;
				continue;
			}

			std::vector<double> fsr;
			for (const auto &w : row) {
				if (w.font_size > 0)
					fsr.push_back(w.font_size);
			}
			double mfs = Median(fsr);
			if (mfs <= 0)
				mfs = 10.0;

			string formatted = format_row(row);
			string plain = join_plain(row);

			int hl = 0;
			if (mfs >= 1.15 * body_size) {
				hl = get_heading_level(mfs);
			}
			bool is_h = hl > 0;

			bool is_l = false;
			if (plain.size() >= 2 && (plain.substr(0, 2) == "- " || plain.substr(0, 2) == "* ")) {
				is_l = true;
			} else {
				size_t k = 0;
				auto isdig = [](char c) {
					return c >= '0' && c <= '9';
				};
				while (k < plain.size() && isdig(plain[k]))
					++k;
				if (k > 0 && k + 1 < plain.size() && plain[k] == '.' && plain[k + 1] == ' ') {
					is_l = true;
				}
			}
			if (plain.size() >= 2 && (plain.substr(0, 2) == "• " || plain.substr(0, 2) == "◦ ")) {
				if (formatted.size() >= 2 && (formatted.substr(0, 2) == "• " || formatted.substr(0, 2) == "◦ ")) {
					formatted = "- " + formatted.substr(2);
				}
				is_l = true;
			}

			if (is_h) {
				if (!para_buf.empty()) {
					out += para_buf + "\n\n";
					para_buf.clear();
				}
				string htext = join_plain(row);
				out += string(hl, '#') + " " + htext + "\n\n";
				prev_ymax = -1e9;
				continue;
			}
			if (is_l) {
				if (!para_buf.empty()) {
					out += para_buf + "\n\n";
					para_buf.clear();
				}
				out += formatted + "\n";
				prev_ymax = -1e9;
				continue;
			}

			// body paragraph accumulation
			double gap = (prev_ymax > -1e8) ? (rymin - prev_ymax) : 99999.0;
			bool same_p = !para_buf.empty() && (gap < 1.5 * med_h);
			if (!same_p && !para_buf.empty()) {
				out += para_buf + "\n\n";
				para_buf.clear();
			}
			if (!para_buf.empty())
				para_buf += " ";
			para_buf += formatted;
			prev_ymax = rymax;
		}
		if (!para_buf.empty()) {
			out += para_buf + "\n";
		}
	}
	return out;
}

static void PdfToMarkdownFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t path) {
		return StringVector::AddString(result, DocToMarkdown(context, path.GetString()));
	});
}

//===--------------------------------------------------------------------===//
// Document-level operations via qpdf: pdf_merge / pdf_split / pdf_rotate
//
// These are FILE-level structural transforms (like the write path): inputs
// and outputs are local filesystem paths, kept exactly as given — no
// normalization, no VFS. Overwrite semantics match COPY TO (existing output
// is replaced); a missing output DIRECTORY is an error, never created.
//
// Locking: every whole qpdf operation runs under one dedicated global lock
// (QpdfMutex, inside qpdf_ops.cpp). This is deliberately NOT PopplerMutex:
// qpdf and poppler share no state, so the two libraries may run concurrently
// with each other, just not with themselves.
//===--------------------------------------------------------------------===//

// Parent directory of a path, honoring both separators; empty string means
// "bare filename in the current directory" (nothing to validate).
static string PdfOpsParentDir(const string &path) {
	auto pos = path.find_last_of("/\\");
	if (pos == string::npos) {
		return string();
	}
	return path.substr(0, pos);
}

// Filename without directory and without the last extension: '/a/b/x.pdf' -> 'x'.
static string PdfOpsStem(const string &path) {
	auto slash = path.find_last_of("/\\");
	string base = (slash == string::npos) ? path : path.substr(slash + 1);
	auto dot = base.find_last_of('.');
	if (dot != string::npos && dot > 0) {
		base = base.substr(0, dot);
	}
	return base;
}

static void PdfOpsCheckInputExists(const char *fn, const string &path) {
	auto fs = FileSystem::CreateLocal();
	if (!fs->FileExists(path)) {
		throw InvalidInputException("%s: input file '%s' does not exist", fn, path);
	}
}

static void PdfOpsCheckOutputDir(const char *fn, const string &dir) {
	if (dir.empty()) {
		return; // bare filename resolves to the process working directory
	}
	auto fs = FileSystem::CreateLocal();
	if (!fs->DirectoryExists(dir)) {
		throw InvalidInputException("%s: output directory '%s' does not exist", fn, dir);
	}
}

// pdf_merge(inputs LIST(VARCHAR), output VARCHAR) -> VARCHAR (the output path).
// Concatenates the inputs' pages in list order. Build the list in SQL — the
// glob recipe is: SELECT list(DISTINCT filename ORDER BY filename) FROM
// read_pdf_meta('dir/*.pdf').
static string PdfMergeImpl(const vector<string> &inputs, const string &output) {
	if (inputs.empty()) {
		throw InvalidInputException("pdf_merge: input list is empty");
	}
	for (auto &in_path : inputs) {
		PdfOpsCheckInputExists("pdf_merge", in_path);
	}
	PdfOpsCheckOutputDir("pdf_merge", PdfOpsParentDir(output));
	try {
		pdf_qpdf::Merge(inputs, output);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_merge: %s", string(e.what()));
	}
	return output;
}

static void PdfMergeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	// Value-based row loop: this is a file operation executed a handful of
	// times per query, not a hot path — clarity over vectorized throughput.
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		Value list_val = args.GetValue(0, row);
		Value out_val = args.GetValue(1, row);
		if (list_val.IsNull() || out_val.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		vector<string> inputs;
		for (auto &child : ListValue::GetChildren(list_val)) {
			if (child.IsNull()) {
				throw InvalidInputException("pdf_merge: input list contains a NULL path");
			}
			inputs.push_back(StringValue::Get(child));
		}
		result.SetValue(row, Value(PdfMergeImpl(inputs, StringValue::Get(out_val))));
	}
}

// pdf_rotate(input, output, degrees[, pages]) -> VARCHAR (the output path).
// degrees must be a multiple of 90 (added to each page's existing rotation);
// pages is 'all' (default) or a qpdf-style numeric range like '1-3,7' /
// 'z' (last page) / 'r2' (second-to-last).
static string PdfRotateImpl(const string &input, const string &output, int32_t degrees, const string &pages_spec) {
	if (degrees % 90 != 0) {
		throw InvalidInputException("pdf_rotate: degrees must be a multiple of 90 (got %d)", degrees);
	}
	PdfOpsCheckInputExists("pdf_rotate", input);
	PdfOpsCheckOutputDir("pdf_rotate", PdfOpsParentDir(output));
	if (input == output) {
		// qpdf reads the source lazily during write; writing over it would corrupt.
		throw InvalidInputException("pdf_rotate: output path must differ from input path '%s'", input);
	}
	try {
		// 'all' short-circuits here; anything else is qpdf's own CLI range
		// grammar, parsed inside pdf_qpdf::Rotate (throws with a descriptive
		// message on malformed / out-of-range specs).
		bool all_pages = StringUtil::Lower(pages_spec) == "all";
		pdf_qpdf::Rotate(input, output, degrees, all_pages, pages_spec);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_rotate: %s", string(e.what()));
	}
	return output;
}

static void PdfRotateFunInternal(DataChunk &args, Vector &result, bool has_pages_arg) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		Value in_val = args.GetValue(0, row);
		Value out_val = args.GetValue(1, row);
		Value deg_val = args.GetValue(2, row);
		Value pages_val = has_pages_arg ? args.GetValue(3, row) : Value("all");
		if (in_val.IsNull() || out_val.IsNull() || deg_val.IsNull() || pages_val.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		result.SetValue(row, Value(PdfRotateImpl(StringValue::Get(in_val), StringValue::Get(out_val),
		                                         IntegerValue::Get(deg_val), StringValue::Get(pages_val))));
	}
}

static void PdfRotateFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfRotateFunInternal(args, result, false);
}

static void PdfRotatePagesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfRotateFunInternal(args, result, true);
}

// pdf_split(input, output_dir) -> TABLE(page INTEGER, file VARCHAR).
// Writes one single-page PDF per page as <output_dir>/<input_stem>_p<N>.pdf
// (N zero-padded to the page-count width); one row per emitted file.
struct PdfSplitBindData : public TableFunctionData {
	string input;
	string output_dir;
};

struct PdfSplitState : public GlobalTableFunctionState {
	bool executed = false;
	idx_t emit_idx = 0;
	std::vector<std::pair<int, string>> emitted; // (page, file)
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PdfSplitBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfSplitBindData>();
	result->input = StringValue::Get(input.inputs[0]);
	result->output_dir = StringValue::Get(input.inputs[1]);
	return_types = {LogicalType::INTEGER, LogicalType::VARCHAR};
	names = {"page", "file"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> PdfSplitInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PdfSplitState>();
}

static void PdfSplitExecute(const string &input, const string &output_dir,
                            std::vector<std::pair<int, string>> &emitted) {
	PdfOpsCheckInputExists("pdf_split", input);
	{
		auto fs = FileSystem::CreateLocal();
		if (!fs->DirectoryExists(output_dir)) {
			throw InvalidInputException("pdf_split: output directory '%s' does not exist", output_dir);
		}
	}
	try {
		pdf_qpdf::Split(input, output_dir, PdfOpsStem(input), emitted);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_split: %s", string(e.what()));
	}
}

static void PdfSplitScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfSplitBindData>();
	auto &st = data_p.global_state->Cast<PdfSplitState>();
	if (!st.executed) {
		// Side effects happen at scan time (not bind), so EXPLAIN writes nothing.
		PdfSplitExecute(bind.input, bind.output_dir, st.emitted);
		st.executed = true;
	}
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.emit_idx < st.emitted.size()) {
		output.SetValue(0, count, Value::INTEGER(st.emitted[st.emit_idx].first));
		output.SetValue(1, count, Value(st.emitted[st.emit_idx].second));
		st.emit_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// pdf_split_blank(input, output_dir[, blank_threshold]) -> TABLE(document
// INTEGER, first_page INTEGER, last_page INTEGER, page_count INTEGER, file
// VARCHAR).
//
// Mailroom batch splitting: an office scanner produces ONE PDF that is really
// N documents separated by blank sheets. A page is a SEPARATOR when it has no
// extractable text AND its raster is (near-)white — text-empty alone would
// false-positive on a full-page-image scan (e.g. a scanned photo with no OCR
// layer). Detection runs here, on the poppler side; the qpdf side
// (pdf_qpdf::SplitRanges, qpdf_ops.cpp) only extracts the already-computed
// content-page ranges into separate files, exactly like pdf_split /
// pdf_qpdf::Split above but for multi-page ranges instead of one page each.
//===--------------------------------------------------------------------===//

// Renders the page at a low DPI (detection doesn't need print resolution) and
// reports whether the fraction of near-white pixels meets `threshold`. Shares
// the exact render hints as OcrPage / RenderPageToPngBytes; reads the raw
// argb32 buffer directly instead of round-tripping through PNG or tesseract.
static bool PageRendersNearWhite(poppler::page *page, double threshold) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	poppler::page_renderer renderer;
	renderer.set_render_hint(poppler::page_renderer::antialiasing, true);
	renderer.set_render_hint(poppler::page_renderer::text_antialiasing, true);

	const int32_t kBlankCheckDpi = 72; // low-res is enough to tell blank from content
	poppler::image img = renderer.render_page(page, kBlankCheckDpi, kBlankCheckDpi);
	if (!img.is_valid() || img.width() <= 0 || img.height() <= 0) {
		// Can't render — never claim blank on a page we couldn't inspect.
		return false;
	}

	const int width = img.width();
	const int height = img.height();
	const int stride = img.bytes_per_row();
	const auto *data = reinterpret_cast<const unsigned char *>(img.const_data());
	const unsigned char kNearWhiteChannel = 250; // out of 255, per color channel
	int64_t white_pixels = 0;
	const int64_t total_pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
	for (int y = 0; y < height; y++) {
		const unsigned char *row = data + static_cast<size_t>(y) * static_cast<size_t>(stride);
		for (int x = 0; x < width; x++) {
			// poppler renders argb32; alpha is always opaque for a page raster, so
			// checking the first 3 bytes (order-agnostic for a whiteness test —
			// white is 255 on every channel regardless of byte order) is enough.
			const unsigned char *px = row + static_cast<size_t>(x) * 4;
			if (px[0] >= kNearWhiteChannel && px[1] >= kNearWhiteChannel && px[2] >= kNearWhiteChannel) {
				white_pixels++;
			}
		}
	}
	return total_pixels > 0 && (static_cast<double>(white_pixels) / static_cast<double>(total_pixels)) >= threshold;
}

struct PdfSplitBlankBindData : public TableFunctionData {
	string input;
	string output_dir;
	double blank_threshold = 0.995;
};

struct PdfSplitBlankEmitted {
	int document = 0;
	int first_page = 0;
	int last_page = 0;
	int page_count = 0;
	string file;
};

struct PdfSplitBlankState : public GlobalTableFunctionState {
	bool executed = false;
	idx_t emit_idx = 0;
	std::vector<PdfSplitBlankEmitted> emitted;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> PdfSplitBlankBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfSplitBlankBindData>();
	result->input = StringValue::Get(input.inputs[0]);
	result->output_dir = StringValue::Get(input.inputs[1]);
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "blank_threshold") {
			result->blank_threshold = DoubleValue::Get(kv.second);
		}
	}
	if (!(result->blank_threshold > 0.0) || result->blank_threshold > 1.0) {
		throw InvalidInputException("pdf_split_blank: blank_threshold must be in (0, 1] (got %f)",
		                            result->blank_threshold);
	}
	return_types = {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER,
	                LogicalType::VARCHAR};
	names = {"document", "first_page", "last_page", "page_count", "file"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> PdfSplitBlankInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PdfSplitBlankState>();
}

// Detects separator pages, then delegates the actual file writing to qpdf.
static void PdfSplitBlankExecute(ClientContext &context, const string &input, const string &output_dir,
                                 double blank_threshold, std::vector<PdfSplitBlankEmitted> &emitted) {
	PdfOpsCheckInputExists("pdf_split_blank", input);
	{
		auto fs = FileSystem::CreateLocal();
		if (!fs->DirectoryExists(output_dir)) {
			throw InvalidInputException("pdf_split_blank: output directory '%s' does not exist", output_dir);
		}
	}

	string bytes;
	ReadAllBytes(context, input, bytes);
	auto doc = LoadDoc(bytes, "", input);
	const int page_count = doc->pages();

	std::vector<bool> is_separator(page_count, false);
	{
		PopplerDocGuard poppler_guard;
		for (int i = 0; i < page_count; i++) {
			unique_ptr<poppler::page> page(doc->create_page(i));
			if (!page) {
				continue; // unreadable page: never treat as a separator
			}
			string text = UStringToUtf8(page->text(poppler::rectf(), poppler::page::physical_layout));
			bool text_blank = text.find_first_not_of(" \t\r\n\f\v") == string::npos;
			is_separator[i] = text_blank && PageRendersNearWhite(page.get(), blank_threshold);
		}
	}

	// Contiguous runs of non-separator pages, 1-based inclusive. Leading /
	// trailing / consecutive separator pages simply never open (or immediately
	// close without opening) a run, so no empty document is ever emitted; an
	// all-blank file leaves `ranges` empty, so zero rows are emitted.
	std::vector<std::pair<int, int>> ranges;
	int range_start = -1;
	for (int i = 0; i < page_count; i++) {
		if (!is_separator[i]) {
			if (range_start == -1) {
				range_start = i;
			}
		} else if (range_start != -1) {
			ranges.emplace_back(range_start + 1, i);
			range_start = -1;
		}
	}
	if (range_start != -1) {
		ranges.emplace_back(range_start + 1, page_count);
	}

	std::vector<string> files;
	try {
		pdf_qpdf::SplitRanges(input, output_dir, PdfOpsStem(input), ranges, files);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_split_blank: %s", string(e.what()));
	}

	for (size_t k = 0; k < ranges.size(); k++) {
		PdfSplitBlankEmitted row;
		row.document = static_cast<int>(k) + 1;
		row.first_page = ranges[k].first;
		row.last_page = ranges[k].second;
		row.page_count = ranges[k].second - ranges[k].first + 1;
		row.file = files[k];
		emitted.push_back(std::move(row));
	}
}

static void PdfSplitBlankScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfSplitBlankBindData>();
	auto &st = data_p.global_state->Cast<PdfSplitBlankState>();
	if (!st.executed) {
		// Side effects happen at scan time (not bind), so EXPLAIN writes nothing.
		PdfSplitBlankExecute(context, bind.input, bind.output_dir, bind.blank_threshold, st.emitted);
		st.executed = true;
	}
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.emit_idx < st.emitted.size()) {
		auto &row = st.emitted[st.emit_idx];
		output.SetValue(0, count, Value::INTEGER(row.document));
		output.SetValue(1, count, Value::INTEGER(row.first_page));
		output.SetValue(2, count, Value::INTEGER(row.last_page));
		output.SetValue(3, count, Value::INTEGER(row.page_count));
		output.SetValue(4, count, Value(row.file));
		st.emit_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// qpdf suite: pdf_compress / pdf_encrypt / pdf_decrypt / pdf_pages
//
// Same conventions as pdf_merge/pdf_rotate: local filesystem paths exactly as
// given, existing output overwritten, missing output directory is an error,
// in-place (input == output) refused because qpdf reads the source lazily
// during write, and the whole operation runs under QpdfMutex (in qpdf_ops.cpp).
//===--------------------------------------------------------------------===//

// Shared preamble for the single-input scalar ops.
static void PdfOpsCheckInOut(const char *fn, const string &input, const string &output) {
	PdfOpsCheckInputExists(fn, input);
	PdfOpsCheckOutputDir(fn, PdfOpsParentDir(output));
	if (input == output) {
		// qpdf reads the source lazily during write; writing over it would corrupt.
		throw InvalidInputException("%s: output path must differ from input path '%s'", fn, input);
	}
}

// pdf_compress(input, output) -> VARCHAR (the output path).
// Structural optimization only: object streams, stream recompression, and
// linearization ("fast web view"). Image data is carried over as-is — this
// does NOT downsample or re-encode images.
static string PdfCompressImpl(const string &input, const string &output) {
	PdfOpsCheckInOut("pdf_compress", input, output);
	try {
		pdf_qpdf::Compress(input, output);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_compress: %s", string(e.what()));
	}
	return output;
}

static void PdfCompressFun(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		Value in_val = args.GetValue(0, row);
		Value out_val = args.GetValue(1, row);
		if (in_val.IsNull() || out_val.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		result.SetValue(row, Value(PdfCompressImpl(StringValue::Get(in_val), StringValue::Get(out_val))));
	}
}

// pdf_encrypt(input, output, user_password[, owner_password]) -> VARCHAR.
// AES-256 (R6) — the only password scheme the PDF 2.0 spec still endorses;
// qpdf's built-in native crypto provider implements it, so no external
// crypto library is needed. All permissions are allowed; an empty owner
// password falls back to the user password.
static string PdfEncryptImpl(const string &input, const string &output, const string &user_password,
                             const string &owner_password) {
	PdfOpsCheckInOut("pdf_encrypt", input, output);
	const string &owner = owner_password.empty() ? user_password : owner_password;
	try {
		pdf_qpdf::Encrypt(input, output, user_password, owner);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_encrypt: %s", string(e.what()));
	}
	return output;
}

static void PdfEncryptFunInternal(DataChunk &args, Vector &result, bool has_owner_arg) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		Value in_val = args.GetValue(0, row);
		Value out_val = args.GetValue(1, row);
		Value user_val = args.GetValue(2, row);
		Value owner_val = has_owner_arg ? args.GetValue(3, row) : Value("");
		if (in_val.IsNull() || out_val.IsNull() || user_val.IsNull() || owner_val.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		result.SetValue(row, Value(PdfEncryptImpl(StringValue::Get(in_val), StringValue::Get(out_val),
		                                          StringValue::Get(user_val), StringValue::Get(owner_val))));
	}
}

static void PdfEncryptFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfEncryptFunInternal(args, result, false);
}

static void PdfEncryptOwnerFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfEncryptFunInternal(args, result, true);
}

// pdf_decrypt(input, output, password) -> VARCHAR. Opens with the password
// (user or owner) and writes an unencrypted copy. A wrong password surfaces
// qpdf's "invalid password" as an InvalidInput error.
static string PdfDecryptImpl(const string &input, const string &output, const string &password) {
	PdfOpsCheckInOut("pdf_decrypt", input, output);
	try {
		pdf_qpdf::Decrypt(input, output, password);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_decrypt: %s", string(e.what()));
	}
	return output;
}

static void PdfDecryptFun(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		Value in_val = args.GetValue(0, row);
		Value out_val = args.GetValue(1, row);
		Value pw_val = args.GetValue(2, row);
		if (in_val.IsNull() || out_val.IsNull() || pw_val.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		result.SetValue(
		    row, Value(PdfDecryptImpl(StringValue::Get(in_val), StringValue::Get(out_val), StringValue::Get(pw_val))));
	}
}

// pdf_pages(input, output, ranges) -> VARCHAR. Extracts the page subset
// selected by a qpdf-style numeric range ('1-3,7' / 'z' = last / 'r2' =
// second-to-last) into a new document, in range order (repeats allowed).
static string PdfPagesImpl(const string &input, const string &output, const string &ranges) {
	PdfOpsCheckInOut("pdf_pages", input, output);
	try {
		pdf_qpdf::Pages(input, output, ranges);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_pages: %s", string(e.what()));
	}
	return output;
}

static void PdfPagesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		Value in_val = args.GetValue(0, row);
		Value out_val = args.GetValue(1, row);
		Value ranges_val = args.GetValue(2, row);
		if (in_val.IsNull() || out_val.IsNull() || ranges_val.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		result.SetValue(row, Value(PdfPagesImpl(StringValue::Get(in_val), StringValue::Get(out_val),
		                                        StringValue::Get(ranges_val))));
	}
}

// pdf_watermark(input, output, text[, opacity]) -> VARCHAR (the output path).
// Stamps `text` as a large diagonal gray watermark, on top of every page's
// content, as real (selectable) Helvetica text. `opacity` is the fill alpha,
// default 0.30, and must be in (0, 1].
static string PdfWatermarkImpl(const string &input, const string &output, const string &text, double opacity) {
	PdfOpsCheckInOut("pdf_watermark", input, output);
	if (!(opacity > 0.0 && opacity <= 1.0)) {
		throw InvalidInputException("pdf_watermark: opacity must be in (0, 1] (got %f)", opacity);
	}
	try {
		pdf_qpdf::Watermark(input, output, text, opacity);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_watermark: %s", string(e.what()));
	}
	return output;
}

static void PdfWatermarkFunInternal(DataChunk &args, Vector &result, bool has_opacity_arg) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		Value in_val = args.GetValue(0, row);
		Value out_val = args.GetValue(1, row);
		Value text_val = args.GetValue(2, row);
		Value opacity_val = has_opacity_arg ? args.GetValue(3, row) : Value::DOUBLE(0.30);
		if (in_val.IsNull() || out_val.IsNull() || text_val.IsNull() || opacity_val.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		result.SetValue(row, Value(PdfWatermarkImpl(StringValue::Get(in_val), StringValue::Get(out_val),
		                                            StringValue::Get(text_val), DoubleValue::Get(opacity_val))));
	}
}

static void PdfWatermarkFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfWatermarkFunInternal(args, result, false);
}

static void PdfWatermarkOpacityFun(DataChunk &args, ExpressionState &state, Vector &result) {
	PdfWatermarkFunInternal(args, result, true);
}

// pdf_bates(input, output, prefix, start_number) -> VARCHAR (the output path).
// Bates numbering: stamps prefix + a zero-padded (min 6 digits) sequential
// number at the bottom-right of each page as real Helvetica text.
static string PdfBatesImpl(const string &input, const string &output, const string &prefix, int64_t start_number) {
	PdfOpsCheckInOut("pdf_bates", input, output);
	try {
		pdf_qpdf::Bates(input, output, prefix, static_cast<long long>(start_number), nullptr);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_bates: %s", string(e.what()));
	}
	return output;
}

static void PdfBatesFun(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < args.size(); row++) {
		Value in_val = args.GetValue(0, row);
		Value out_val = args.GetValue(1, row);
		Value prefix_val = args.GetValue(2, row);
		Value start_val = args.GetValue(3, row);
		if (in_val.IsNull() || out_val.IsNull() || prefix_val.IsNull() || start_val.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		result.SetValue(row, Value(PdfBatesImpl(StringValue::Get(in_val), StringValue::Get(out_val),
		                                        StringValue::Get(prefix_val), BigIntValue::Get(start_val))));
	}
}

//===--------------------------------------------------------------------===//
// qpdf suite: pdf_form_fields / pdf_annotations (table functions)
//
// One VARCHAR path/glob argument resolved through the same ResolveFiles used
// by read_pdf. Rows are materialized on the first scan call (side effects and
// file reads at scan time, not bind — EXPLAIN touches nothing), serially,
// under QpdfMutex (in qpdf_ops.cpp).
//===--------------------------------------------------------------------===//

// Strip the leading '/' from a PDF name ('/Link' -> 'Link'); empty stays empty.
static string PdfNameToText(const string &name) {
	if (!name.empty() && name[0] == '/') {
		return name.substr(1);
	}
	return name;
}

struct PdfQpdfRowsBindData : public TableFunctionData {
	vector<string> files;
};

struct PdfQpdfRowsState : public GlobalTableFunctionState {
	bool executed = false;
	idx_t emit_idx = 0;
	std::vector<std::vector<Value>> rows;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<GlobalTableFunctionState> PdfQpdfRowsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PdfQpdfRowsState>();
}

static void PdfQpdfRowsEmit(PdfQpdfRowsState &st, DataChunk &output) {
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.emit_idx < st.rows.size()) {
		auto &row = st.rows[st.emit_idx];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		st.emit_idx++;
		count++;
	}
	output.SetCardinality(count);
}

// pdf_form_fields(files) -> one row per AcroForm field. page is NULL when the
// field has no widget annotation on any page; value is NULL when /V is unset.
static unique_ptr<FunctionData> PdfFormFieldsBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfQpdfRowsBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN};
	names = {"file", "page", "field_name", "field_type", "value", "is_required"};
	return std::move(result);
}

static string PdfFormFieldTypeText(const string &raw_type) {
	if (raw_type == "/Tx") {
		return "text";
	}
	if (raw_type == "/Btn") {
		return "button";
	}
	if (raw_type == "/Ch") {
		return "choice";
	}
	if (raw_type == "/Sig") {
		return "signature";
	}
	return raw_type; // unknown: pass qpdf's raw name through untouched
}

static void PdfFormFieldsExecute(const string &path, std::vector<std::vector<Value>> &rows) {
	PdfOpsCheckInputExists("pdf_form_fields", path);
	std::vector<pdf_qpdf::FormField> fields;
	try {
		fields = pdf_qpdf::ReadFormFields(path);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_form_fields: %s", string(e.what()));
	}
	for (auto &field : fields) {
		// A field with no widget annotation on any page keeps page NULL.
		Value page_val = field.has_page ? Value::INTEGER(field.page) : Value(LogicalType::INTEGER);
		Value value_val = field.has_value ? Value(field.value) : Value(LogicalType::VARCHAR);
		rows.push_back({Value(path), page_val, Value(field.name), Value(PdfFormFieldTypeText(field.type)), value_val,
		                Value::BOOLEAN(field.is_required)});
	}
}

static void PdfFormFieldsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfQpdfRowsBindData>();
	auto &st = data_p.global_state->Cast<PdfQpdfRowsState>();
	if (!st.executed) {
		for (auto &path : bind.files) {
			PdfFormFieldsExecute(path, st.rows);
		}
		st.executed = true;
	}
	PdfQpdfRowsEmit(st, output);
}

// pdf_annotations(files) -> one row per page annotation. uri is populated for
// Link annotations with an /A /URI action; contents is NULL when absent. This
// one function covers both "extract comments/highlights" and "extract
// hyperlinks" (WHERE subtype = 'Link').
static unique_ptr<FunctionData> PdfAnnotationsBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfQpdfRowsBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::DOUBLE};
	names = {"file", "page", "subtype", "contents", "uri", "rect_x0", "rect_y0", "rect_x1", "rect_y1"};
	return std::move(result);
}

static void PdfAnnotationsExecute(const string &path, std::vector<std::vector<Value>> &rows) {
	PdfOpsCheckInputExists("pdf_annotations", path);
	std::vector<pdf_qpdf::Annotation> annotations;
	try {
		annotations = pdf_qpdf::ReadAnnotations(path);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_annotations: %s", string(e.what()));
	}
	for (auto &annot : annotations) {
		Value contents_val = annot.has_contents ? Value(annot.contents) : Value(LogicalType::VARCHAR);
		Value uri_val = annot.has_uri ? Value(annot.uri) : Value(LogicalType::VARCHAR);
		rows.push_back({Value(path), Value::INTEGER(annot.page), Value(PdfNameToText(annot.subtype)), contents_val,
		                uri_val, Value::DOUBLE(annot.rect_x0), Value::DOUBLE(annot.rect_y0),
		                Value::DOUBLE(annot.rect_x1), Value::DOUBLE(annot.rect_y1)});
	}
}

static void PdfAnnotationsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfQpdfRowsBindData>();
	auto &st = data_p.global_state->Cast<PdfQpdfRowsState>();
	if (!st.executed) {
		for (auto &path : bind.files) {
			PdfAnnotationsExecute(path, st.rows);
		}
		st.executed = true;
	}
	PdfQpdfRowsEmit(st, output);
}

//===--------------------------------------------------------------------===//
// pdf_signatures(files) -> one row per filled AcroForm /Sig field.
//
// The qpdf signature-dictionary walk, ByteRange coverage, and OpenSSL CMS
// verification all live in qpdf_ops.cpp (the C++17 qpdf/openssl TU). This file
// only resolves the file glob, parses the raw /M date into a TIMESTAMP, and
// shapes the plain SignatureInfo structs into rows.
//===--------------------------------------------------------------------===//

struct PdfSignaturesBindData : public TableFunctionData {
	vector<string> files;
	string password;
};

// Parse a PDF date string (D:YYYYMMDDHHmmSS[offset...]) into a TIMESTAMP.
// Returns NULL on missing/unparseable input. Offset is ignored — values are
// surfaced as naive local timestamps matching the wall-clock fields in /M.
static Value PdfParseDateOrNull(const string &raw) {
	if (raw.empty()) {
		return Value(LogicalType::TIMESTAMP);
	}
	// Strip optional leading "D:"
	const char *p = raw.c_str();
	if (raw.size() >= 2 && (p[0] == 'D' || p[0] == 'd') && p[1] == ':') {
		p += 2;
	}
	// Need at least YYYYMMDD
	size_t len = strlen(p);
	if (len < 8) {
		return Value(LogicalType::TIMESTAMP);
	}
	auto take = [&](int n, int &out) -> bool {
		if (len < (size_t)n) {
			return false;
		}
		int v = 0;
		for (int i = 0; i < n; i++) {
			if (!std::isdigit(static_cast<unsigned char>(p[i]))) {
				return false;
			}
			v = v * 10 + (p[i] - '0');
		}
		out = v;
		p += n;
		len -= n;
		return true;
	};
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	if (!take(4, year) || !take(2, month) || !take(2, day)) {
		return Value(LogicalType::TIMESTAMP);
	}
	if (len >= 2) {
		take(2, hour);
	}
	if (len >= 2) {
		take(2, minute);
	}
	if (len >= 2) {
		take(2, second);
	}
	if (month < 1 || month > 12 || day < 1 || day > 31) {
		return Value(LogicalType::TIMESTAMP);
	}
	date_t date;
	if (!Date::TryFromDate((int32_t)year, (int32_t)month, (int32_t)day, date)) {
		return Value(LogicalType::TIMESTAMP);
	}
	dtime_t time = Time::FromTime((int32_t)hour, (int32_t)minute, (int32_t)second, 0);
	timestamp_t ts;
	if (!Timestamp::TryFromDatetime(date, time, ts)) {
		return Value(LogicalType::TIMESTAMP);
	}
	return Value::TIMESTAMP(ts);
}

static unique_ptr<FunctionData> PdfSignaturesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfSignaturesBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "password") {
			result->password = StringValue::Get(kv.second);
		}
	}
	// `file` is included for glob parity with pdf_form_fields / pdf_annotations.
	return_types = {LogicalType::VARCHAR,   LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::TIMESTAMP, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR,   LogicalType::BOOLEAN, LogicalType::BOOLEAN};
	names = {"file",   "field_name", "subfilter",         "signing_time", "signer_name",
	         "reason", "location",   "covers_whole_file", "verified"};
	return std::move(result);
}

static void PdfSignaturesExecute(const string &path, const string &password, std::vector<std::vector<Value>> &rows) {
	PdfOpsCheckInputExists("pdf_signatures", path);
	std::vector<pdf_qpdf::SignatureInfo> sigs;
	try {
		sigs = pdf_qpdf::ReadSignatures(path, password);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_signatures: %s", string(e.what()));
	}
	for (auto &sig : sigs) {
		// Empty string from the boundary means SQL NULL.
		Value subfilter = sig.subfilter.empty() ? Value(LogicalType::VARCHAR) : Value(sig.subfilter);
		Value signing_time = PdfParseDateOrNull(sig.signing_time_raw);
		Value signer_name = sig.signer_name.empty() ? Value(LogicalType::VARCHAR) : Value(sig.signer_name);
		Value reason = sig.reason.empty() ? Value(LogicalType::VARCHAR) : Value(sig.reason);
		Value location = sig.location.empty() ? Value(LogicalType::VARCHAR) : Value(sig.location);
		// verified is NULL when the signature dict carried no usable /Contents.
		Value verified = sig.has_verified ? Value::BOOLEAN(sig.verified) : Value(LogicalType::BOOLEAN);
		rows.push_back({Value(path), Value(sig.field_name), subfilter, signing_time, signer_name, reason, location,
		                Value::BOOLEAN(sig.covers_whole_file), verified});
	}
}

static void PdfSignaturesScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfSignaturesBindData>();
	auto &st = data_p.global_state->Cast<PdfQpdfRowsState>();
	if (!st.executed) {
		for (auto &path : bind.files) {
			PdfSignaturesExecute(path, bind.password, st.rows);
		}
		st.executed = true;
	}
	PdfQpdfRowsEmit(st, output);
}

// pdf_sign(input, output, cert_path, key_path) -> one row {output, field_name}.
//
// Creates an adbe.pkcs7.detached CMS signature over the whole file. The qpdf
// field construction, deterministic write, ByteRange patch, and OpenSSL CMS_sign
// all live in qpdf_ops.cpp (the C++17 qpdf/openssl TU). This file only validates
// paths, resolves the named parameters, and shapes the single result row. The
// signed output verifies through pdf_signatures with covers_whole_file = true and
// verified = true.
//===--------------------------------------------------------------------===//

struct PdfSignBindData : public TableFunctionData {
	string input;
	string output;
	string cert_path;
	string key_path;
	string key_password;
	string reason;
	string location;
	string signer_name;
	string field_name = "Signature1";
	string password;
};

static unique_ptr<FunctionData> PdfSignBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfSignBindData>();
	result->input = StringValue::Get(input.inputs[0]);
	result->output = StringValue::Get(input.inputs[1]);
	result->cert_path = StringValue::Get(input.inputs[2]);
	result->key_path = StringValue::Get(input.inputs[3]);
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (kv.second.IsNull()) {
			continue;
		}
		if (key == "key_password") {
			result->key_password = StringValue::Get(kv.second);
		} else if (key == "reason") {
			result->reason = StringValue::Get(kv.second);
		} else if (key == "location") {
			result->location = StringValue::Get(kv.second);
		} else if (key == "signer_name") {
			result->signer_name = StringValue::Get(kv.second);
		} else if (key == "field_name") {
			result->field_name = StringValue::Get(kv.second);
		} else if (key == "password") {
			result->password = StringValue::Get(kv.second);
		}
	}
	if (result->field_name.empty()) {
		result->field_name = "Signature1";
	}
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"output", "field_name"};
	return std::move(result);
}

static void PdfSignScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfSignBindData>();
	auto &st = data_p.global_state->Cast<PdfQpdfRowsState>();
	if (!st.executed) {
		PdfOpsCheckInOut("pdf_sign", bind.input, bind.output);
		PdfOpsCheckInputExists("pdf_sign", bind.cert_path);
		PdfOpsCheckInputExists("pdf_sign", bind.key_path);
		try {
			pdf_qpdf::SignDetached(bind.input, bind.output, bind.cert_path, bind.key_path, bind.key_password,
			                       bind.reason, bind.location, bind.signer_name, bind.field_name, bind.password);
		} catch (const std::exception &e) {
			throw InvalidInputException("pdf_sign: %s", string(e.what()));
		}
		st.rows.push_back({Value(bind.output), Value(bind.field_name)});
		st.executed = true;
	}
	PdfQpdfRowsEmit(st, output);
}

//===--------------------------------------------------------------------===//
// pdf_redact(input, output, redactions[, dpi := 200, password := '...'])
//   -> TABLE(page INTEGER, redacted BOOLEAN, boxes_applied INTEGER)
//
// TRUE (raster) redaction — the redacted text is removed, not merely hidden
// behind a drawn rectangle. `redactions` is a LIST(STRUCT(page INTEGER,
// x DOUBLE, y DOUBLE, w DOUBLE, h DOUBLE)) in PDF POINTS with the ORIGIN at the
// page's BOTTOM-LEFT (standard PDF user space): (x, y) is the lower-left corner
// of the box, w/h its width/height. `page` is 1-based.
//
// Every page carrying at least one box is re-rendered to an RGB raster (poppler,
// the same renderer read_pdf/pdf_to_png use), the boxes are painted solid black
// in the raster, and the page is rebuilt (qpdf side) as an image-only page whose
// only content is that raster. The whole page becomes an image, so NO text from
// a redacted page is extractable afterwards. Pages with no boxes are copied
// through untouched and keep their live text.
//
// Side effects (rendering + file write) happen at SCAN time, not bind, so a bare
// EXPLAIN writes nothing.
//===--------------------------------------------------------------------===//
struct RedactBox {
	int page = 0; // 1-based
	double x = 0, y = 0, w = 0, h = 0;
};

struct PdfRedactBindData : public TableFunctionData {
	string input;
	string output;
	string password;
	int32_t dpi = 200;
	vector<RedactBox> boxes;
};

struct PdfRedactState : public GlobalTableFunctionState {
	bool executed = false;
	idx_t emit_idx = 0;
	std::vector<std::vector<Value>> rows;
	idx_t MaxThreads() const override {
		return 1;
	}
};

// Read a STRUCT field by (case-insensitive) name; throws if absent.
static const Value &RedactStructField(const vector<string> &field_names, const vector<Value> &field_vals,
                                      const char *name) {
	for (idx_t i = 0; i < field_names.size(); i++) {
		if (StringUtil::Lower(field_names[i]) == name) {
			return field_vals[i];
		}
	}
	throw InvalidInputException("pdf_redact: redaction struct is missing required field '%s' "
	                            "(expected STRUCT(page INTEGER, x DOUBLE, y DOUBLE, w DOUBLE, h DOUBLE))",
	                            name);
}

static unique_ptr<FunctionData> PdfRedactBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfRedactBindData>();
	result->input = StringValue::Get(input.inputs[0]);
	result->output = StringValue::Get(input.inputs[1]);

	// redactions: LIST(STRUCT(page, x, y, w, h)).
	const Value &list_val = input.inputs[2];
	if (!list_val.IsNull()) {
		auto &child_type = ListType::GetChildType(list_val.type());
		if (child_type.id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException("pdf_redact: `redactions` must be a LIST of "
			                            "STRUCT(page INTEGER, x DOUBLE, y DOUBLE, w DOUBLE, h DOUBLE)");
		}
		auto &struct_children = StructType::GetChildTypes(child_type);
		vector<string> field_names;
		for (auto &kv : struct_children) {
			field_names.push_back(kv.first);
		}
		for (auto &el : ListValue::GetChildren(list_val)) {
			if (el.IsNull()) {
				throw InvalidInputException("pdf_redact: `redactions` list contains a NULL entry");
			}
			auto &field_vals = StructValue::GetChildren(el);
			RedactBox box;
			box.page = IntegerValue::Get(
			    RedactStructField(field_names, field_vals, "page").DefaultCastAs(LogicalType::INTEGER));
			box.x =
			    DoubleValue::Get(RedactStructField(field_names, field_vals, "x").DefaultCastAs(LogicalType::DOUBLE));
			box.y =
			    DoubleValue::Get(RedactStructField(field_names, field_vals, "y").DefaultCastAs(LogicalType::DOUBLE));
			box.w =
			    DoubleValue::Get(RedactStructField(field_names, field_vals, "w").DefaultCastAs(LogicalType::DOUBLE));
			box.h =
			    DoubleValue::Get(RedactStructField(field_names, field_vals, "h").DefaultCastAs(LogicalType::DOUBLE));
			if (box.page < 1) {
				throw InvalidInputException("pdf_redact: redaction page must be >= 1 (got %d)", box.page);
			}
			result->boxes.push_back(box);
		}
	}

	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "dpi") {
			result->dpi = IntegerValue::Get(kv.second.DefaultCastAs(LogicalType::INTEGER));
		} else if (key == "password") {
			result->password = StringValue::Get(kv.second);
		}
	}
	if (result->dpi < 1 || result->dpi > 2400) {
		throw InvalidInputException("pdf_redact: dpi must be between 1 and 2400 (got %d)", result->dpi);
	}

	return_types = {LogicalType::INTEGER, LogicalType::BOOLEAN, LogicalType::INTEGER};
	names = {"page", "redacted", "boxes_applied"};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> PdfRedactInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PdfRedactState>();
}

// Render one page to RGB and paint the given boxes black. `boxes` are in PDF
// points, origin bottom-left. Fills the RebuiltPage (top-down RGB, media dims).
static pdf_qpdf::RebuiltPage RenderRedactedPage(poppler::document &doc, int page_idx0, int dpi,
                                                const vector<const RedactBox *> &boxes) {
	std::lock_guard<std::recursive_mutex> poppler_guard(PopplerMutex());
	unique_ptr<poppler::page> page(doc.create_page(page_idx0));
	if (!page) {
		throw IOException("pdf_redact: could not read page %d", page_idx0 + 1);
	}
	poppler::page_renderer renderer;
	renderer.set_render_hint(poppler::page_renderer::antialiasing, true);
	renderer.set_render_hint(poppler::page_renderer::text_antialiasing, true);
	poppler::image img = renderer.render_page(page.get(), dpi, dpi);
	if (!img.is_valid()) {
		throw IOException("pdf_redact: failed to render page %d", page_idx0 + 1);
	}

	const int W = img.width();
	const int H = img.height();
	const int bpr = img.bytes_per_row();
	const char *data = img.const_data();
	const poppler::image::format_enum fmt = img.format();

	pdf_qpdf::RebuiltPage out;
	out.redacted = true;
	out.width = W;
	out.height = H;
	out.media_w = W * 72.0 / dpi;
	out.media_h = H * 72.0 / dpi;
	out.rgb.resize(static_cast<size_t>(W) * static_cast<size_t>(H) * 3);

	for (int y = 0; y < H; y++) {
		const unsigned char *row = reinterpret_cast<const unsigned char *>(data) + static_cast<size_t>(y) * bpr;
		for (int x = 0; x < W; x++) {
			unsigned char r = 0, g = 0, b = 0;
			switch (fmt) {
			case poppler::image::format_argb32: {
				uint32_t px;
				std::memcpy(&px, row + static_cast<size_t>(x) * 4, sizeof(px));
				r = (px >> 16) & 0xFF;
				g = (px >> 8) & 0xFF;
				b = px & 0xFF;
				break;
			}
			case poppler::image::format_rgb24:
				r = row[x * 3 + 0];
				g = row[x * 3 + 1];
				b = row[x * 3 + 2];
				break;
			case poppler::image::format_bgr24:
				b = row[x * 3 + 0];
				g = row[x * 3 + 1];
				r = row[x * 3 + 2];
				break;
			case poppler::image::format_gray8:
				r = g = b = row[x];
				break;
			case poppler::image::format_mono:
				r = g = b = (row[x / 8] & (0x80 >> (x % 8))) ? 0xFF : 0x00;
				break;
			default:
				throw IOException("pdf_redact: unsupported render format for page %d", page_idx0 + 1);
			}
			size_t o = (static_cast<size_t>(y) * W + x) * 3;
			out.rgb[o + 0] = static_cast<char>(r);
			out.rgb[o + 1] = static_cast<char>(g);
			out.rgb[o + 2] = static_cast<char>(b);
		}
	}

	// Paint each box solid black. Points -> pixels; flip y (PDF origin bottom-left,
	// raster row 0 at top). Clamp to the raster so off-page boxes never overrun.
	const double scale = dpi / 72.0;
	auto clampi = [](int v, int lo, int hi) {
		return v < lo ? lo : (v > hi ? hi : v);
	};
	for (const RedactBox *box : boxes) {
		int px0 = clampi(static_cast<int>(std::floor(box->x * scale)), 0, W);
		int px1 = clampi(static_cast<int>(std::ceil((box->x + box->w) * scale)), 0, W);
		int ry0 = clampi(static_cast<int>(std::floor(H - (box->y + box->h) * scale)), 0, H);
		int ry1 = clampi(static_cast<int>(std::ceil(H - box->y * scale)), 0, H);
		for (int y = ry0; y < ry1; y++) {
			for (int x = px0; x < px1; x++) {
				size_t o = (static_cast<size_t>(y) * W + x) * 3;
				out.rgb[o + 0] = 0;
				out.rgb[o + 1] = 0;
				out.rgb[o + 2] = 0;
			}
		}
	}
	return out;
}

static void PdfRedactExecute(ClientContext &context, const PdfRedactBindData &bind,
                             std::vector<std::vector<Value>> &rows) {
	PdfOpsCheckInputExists("pdf_redact", bind.input);
	PdfOpsCheckOutputDir("pdf_redact", PdfOpsParentDir(bind.output));
	if (bind.input == bind.output) {
		throw InvalidInputException("pdf_redact: output path must differ from input path '%s'", bind.input);
	}

	// Open the source with poppler to count pages and render redacted ones.
	string bytes;
	ReadAllBytes(context, bind.input, bytes);
	auto doc = LoadDoc(bytes, bind.password, bind.input);
	const int page_count = doc->pages();

	// Validate box page numbers and bucket boxes per page.
	for (auto &box : bind.boxes) {
		if (box.page > page_count) {
			throw InvalidInputException("pdf_redact: redaction page %d is out of range (document has %d page%s)",
			                            box.page, page_count, page_count == 1 ? "" : "s");
		}
	}

	std::vector<pdf_qpdf::RebuiltPage> rebuilt(static_cast<size_t>(page_count));
	std::vector<int> box_counts(static_cast<size_t>(page_count), 0);
	for (int p = 1; p <= page_count; p++) {
		vector<const RedactBox *> page_boxes;
		for (auto &box : bind.boxes) {
			if (box.page == p) {
				page_boxes.push_back(&box);
			}
		}
		if (page_boxes.empty()) {
			rebuilt[p - 1].redacted = false;
			continue;
		}
		rebuilt[p - 1] = RenderRedactedPage(*doc, p - 1, bind.dpi, page_boxes);
		box_counts[p - 1] = static_cast<int>(page_boxes.size());
	}

	try {
		pdf_qpdf::RebuildPagesAsImages(bind.input, bind.output, rebuilt, bind.password);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_redact: %s", string(e.what()));
	}

	for (int p = 1; p <= page_count; p++) {
		rows.push_back({Value::INTEGER(p), Value::BOOLEAN(rebuilt[p - 1].redacted), Value::INTEGER(box_counts[p - 1])});
	}
}

static void PdfRedactScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfRedactBindData>();
	auto &st = data_p.global_state->Cast<PdfRedactState>();
	if (!st.executed) {
		PdfRedactExecute(context, bind, st.rows);
		st.executed = true;
	}
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && st.emit_idx < st.rows.size()) {
		auto &row = st.rows[st.emit_idx];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		st.emit_idx++;
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// pdf_images(files) -> one row per embedded raster image XObject per page.
//
// The page walk, filter-chain inspection, encoded-bytes passthrough, and PNG
// re-wrapping all live in qpdf_ops.cpp (the C++17 qpdf TU). This file resolves
// the file glob, parses the `password` named param, and shapes the plain
// EmbeddedImage structs into rows (emitting `data` as a BLOB).
//===--------------------------------------------------------------------===//

struct PdfImagesBindData : public TableFunctionData {
	vector<string> files;
	string password;
};

static unique_ptr<FunctionData> PdfImagesBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PdfImagesBindData>();
	result->files = ResolveFiles(context, StringValue::Get(input.inputs[0]));
	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "password") {
			result->password = StringValue::Get(kv.second);
		}
	}
	// `file` is included for glob parity with the other qpdf-backed table funcs.
	return_types = {LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BLOB};
	names = {"file",       "page",   "image_index", "name", "width", "height", "bits_per_component",
	         "colorspace", "format", "data"};
	return std::move(result);
}

static void PdfImagesExecute(const string &path, const string &password, std::vector<std::vector<Value>> &rows) {
	PdfOpsCheckInputExists("pdf_images", path);
	std::vector<pdf_qpdf::EmbeddedImage> imgs;
	try {
		imgs = pdf_qpdf::ReadImages(path, password);
	} catch (const std::exception &e) {
		throw InvalidInputException("pdf_images: %s", string(e.what()));
	}
	for (auto &img : imgs) {
		Value colorspace = img.colorspace.empty() ? Value(LogicalType::VARCHAR) : Value(img.colorspace);
		Value data = Value::BLOB(const_data_ptr_cast(img.data.data()), img.data.size());
		rows.push_back({Value(path), Value::INTEGER(img.page), Value::INTEGER(img.image_index), Value(img.name),
		                Value::INTEGER(img.width), Value::INTEGER(img.height), Value::INTEGER(img.bits_per_component),
		                colorspace, Value(img.format), data});
	}
}

static void PdfImagesScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<PdfImagesBindData>();
	auto &st = data_p.global_state->Cast<PdfQpdfRowsState>();
	if (!st.executed) {
		for (auto &path : bind.files) {
			PdfImagesExecute(path, bind.password, st.rows);
		}
		st.executed = true;
	}
	PdfQpdfRowsEmit(st, output);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void LoadInternal(ExtensionLoader &loader) {
	TableFunction read_pdf("read_pdf", {LogicalType::VARCHAR}, ReadPdfScan, ReadPdfBind, ReadPdfInitGlobal,
	                       ReadPdfInitLocal);
	AddCommonNamedParams(read_pdf);
	// Registered individually (not in AddCommonNamedParams) so it is only
	// advertised on functions that actually implement the skip behavior.
	read_pdf.named_parameters["ignore_errors"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(read_pdf);

	TableFunction read_pdf_meta("read_pdf_meta", {LogicalType::VARCHAR}, ReadPdfMetaScan, ReadPdfMetaBind,
	                            ReadPdfMetaInit);
	read_pdf_meta.named_parameters["password"] = LogicalType::VARCHAR;
	read_pdf_meta.named_parameters["ignore_errors"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(read_pdf_meta);

	// inspection suite — read-only, serial per file, password is the only knob
	TableFunction pdf_info("pdf_info", {LogicalType::VARCHAR}, PdfInfoScan, PdfInfoBind, PdfInfoInit);
	pdf_info.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(pdf_info);

	TableFunction pdf_outline("pdf_outline", {LogicalType::VARCHAR}, PdfOutlineScan, PdfOutlineBind, PdfOutlineInit);
	pdf_outline.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(pdf_outline);

	TableFunction pdf_attachments("pdf_attachments", {LogicalType::VARCHAR}, PdfAttachmentsScan, PdfAttachmentsBind,
	                              PdfAttachmentsInit);
	pdf_attachments.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(pdf_attachments);

	// Forensic incremental-update revisions (byte-level startxref / /Prev walk)
	TableFunction pdf_revisions("pdf_revisions", {LogicalType::VARCHAR}, PdfRevisionsScan, PdfRevisionsBind,
	                            PdfRevisionsInit);
	pdf_revisions.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(pdf_revisions);

	TableFunction read_pdf_words("read_pdf_words", {LogicalType::VARCHAR}, ReadPdfWordsScan, ReadPdfWordsBind,
	                             ReadPdfWordsInit);
	AddCommonNamedParams(read_pdf_words);
	loader.RegisterFunction(read_pdf_words);

	TableFunction read_pdf_lines("read_pdf_lines", {LogicalType::VARCHAR}, ReadPdfLinesScan, ReadPdfLinesBind,
	                             ReadPdfLinesInit);
	AddCommonNamedParams(read_pdf_lines);
	loader.RegisterFunction(read_pdf_lines);

	// read_pdf_elements takes only the params it honors (native text layer
	// only in v1 — no OCR/layout knobs).
	TableFunction read_pdf_elements("read_pdf_elements", {LogicalType::VARCHAR}, ReadPdfElementsScan,
	                                ReadPdfElementsBind, ReadPdfElementsInit);
	read_pdf_elements.named_parameters["password"] = LogicalType::VARCHAR;
	read_pdf_elements.named_parameters["first_page"] = LogicalType::INTEGER;
	read_pdf_elements.named_parameters["last_page"] = LogicalType::INTEGER;
	loader.RegisterFunction(read_pdf_elements);

	// pdf_chunks packs the element grain into retrieval-ready chunks; it
	// honors the same file handling + page-range/password params as
	// read_pdf_elements plus its own chunk_size / overlap knobs.
	TableFunction pdf_chunks("pdf_chunks", {LogicalType::VARCHAR}, PdfChunksScan, PdfChunksBind, PdfChunksInit);
	pdf_chunks.named_parameters["chunk_size"] = LogicalType::INTEGER;
	pdf_chunks.named_parameters["overlap"] = LogicalType::INTEGER;
	pdf_chunks.named_parameters["password"] = LogicalType::VARCHAR;
	pdf_chunks.named_parameters["first_page"] = LogicalType::INTEGER;
	pdf_chunks.named_parameters["last_page"] = LogicalType::INTEGER;
	loader.RegisterFunction(pdf_chunks);

	TableFunction read_pdf_tables("read_pdf_tables", {LogicalType::VARCHAR}, ReadPdfTablesScan, ReadPdfTablesBind,
	                              ReadPdfTablesInit);
	AddCommonNamedParams(read_pdf_tables);
	loader.RegisterFunction(read_pdf_tables);

	ScalarFunctionSet pdf_to_text_set("pdf_to_text");
	pdf_to_text_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToTextFun));
	pdf_to_text_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToTextLayoutFun));
	pdf_to_text_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::VARCHAR, PdfToTextBlobFun));
	pdf_to_text_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToTextBlobLayoutFun));
	loader.RegisterFunction(pdf_to_text_set);

	ScalarFunctionSet pdf_to_html_set("pdf_to_html");
	pdf_to_html_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToHtmlFun));
	pdf_to_html_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::VARCHAR, PdfToHtmlBlobFun));
	loader.RegisterFunction(pdf_to_html_set);

	ScalarFunctionSet pdf_to_xml_set("pdf_to_xml");
	pdf_to_xml_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToXmlFun));
	pdf_to_xml_set.AddFunction(ScalarFunction({LogicalType::BLOB}, LogicalType::VARCHAR, PdfToXmlBlobFun));
	loader.RegisterFunction(pdf_to_xml_set);

	loader.RegisterFunction(
	    ScalarFunction("pdf_to_markdown", {LogicalType::VARCHAR}, LogicalType::VARCHAR, PdfToMarkdownFun));

	ScalarFunctionSet pdf_to_svg_set("pdf_to_svg");
	pdf_to_svg_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::INTEGER}, LogicalType::VARCHAR, PdfToSvgFun));
	pdf_to_svg_set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER},
	                                          LogicalType::VARCHAR, PdfToSvgDpiFun));
	pdf_to_svg_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::INTEGER}, LogicalType::VARCHAR, PdfToSvgBlobFun));
	pdf_to_svg_set.AddFunction(ScalarFunction({LogicalType::BLOB, LogicalType::INTEGER, LogicalType::INTEGER},
	                                          LogicalType::VARCHAR, PdfToSvgBlobDpiFun));
	loader.RegisterFunction(pdf_to_svg_set);

	ScalarFunctionSet pdf_to_png_set("pdf_to_png");
	pdf_to_png_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::INTEGER}, LogicalType::BLOB, PdfToPngFun));
	pdf_to_png_set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::INTEGER},
	                                          LogicalType::BLOB, PdfToPngDpiFun));
	pdf_to_png_set.AddFunction(
	    ScalarFunction({LogicalType::BLOB, LogicalType::INTEGER}, LogicalType::BLOB, PdfToPngBlobFun));
	pdf_to_png_set.AddFunction(ScalarFunction({LogicalType::BLOB, LogicalType::INTEGER, LogicalType::INTEGER},
	                                          LogicalType::BLOB, PdfToPngBlobDpiFun));
	loader.RegisterFunction(pdf_to_png_set);

	ScalarFunctionSet to_pdf_set("to_pdf");
	to_pdf_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, ToPdfFun));
	to_pdf_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, ToPdfOutFun));
	loader.RegisterFunction(to_pdf_set);

	ScalarFunctionSet write_pdf_set("write_pdf");
	write_pdf_set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, WritePdfFun));
	write_pdf_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, WritePdfOutFun));
	loader.RegisterFunction(write_pdf_set);

	// document-level qpdf operations
	loader.RegisterFunction(ScalarFunction("pdf_merge", {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::VARCHAR},
	                                       LogicalType::VARCHAR, PdfMergeFun));

	ScalarFunctionSet pdf_rotate_set("pdf_rotate");
	pdf_rotate_set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER},
	                                          LogicalType::VARCHAR, PdfRotateFun));
	pdf_rotate_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR},
	                   LogicalType::VARCHAR, PdfRotatePagesFun));
	loader.RegisterFunction(pdf_rotate_set);

	TableFunction pdf_split("pdf_split", {LogicalType::VARCHAR, LogicalType::VARCHAR}, PdfSplitScan, PdfSplitBind,
	                        PdfSplitInit);
	loader.RegisterFunction(pdf_split);

	TableFunction pdf_split_blank("pdf_split_blank", {LogicalType::VARCHAR, LogicalType::VARCHAR}, PdfSplitBlankScan,
	                              PdfSplitBlankBind, PdfSplitBlankInit);
	pdf_split_blank.named_parameters["blank_threshold"] = LogicalType::DOUBLE;
	loader.RegisterFunction(pdf_split_blank);

	// qpdf everyday suite
	loader.RegisterFunction(ScalarFunction("pdf_compress", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                       LogicalType::VARCHAR, PdfCompressFun));

	ScalarFunctionSet pdf_encrypt_set("pdf_encrypt");
	pdf_encrypt_set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                           LogicalType::VARCHAR, PdfEncryptFun));
	pdf_encrypt_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                   LogicalType::VARCHAR, PdfEncryptOwnerFun));
	loader.RegisterFunction(pdf_encrypt_set);

	loader.RegisterFunction(ScalarFunction("pdf_decrypt",
	                                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                       LogicalType::VARCHAR, PdfDecryptFun));

	loader.RegisterFunction(ScalarFunction("pdf_pages",
	                                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                       LogicalType::VARCHAR, PdfPagesFun));

	// pdf_watermark: 3-arg (default opacity 0.30) + 4-arg (explicit opacity).
	ScalarFunctionSet pdf_watermark_set("pdf_watermark");
	pdf_watermark_set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                             LogicalType::VARCHAR, PdfWatermarkFun));
	pdf_watermark_set.AddFunction(
	    ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE},
	                   LogicalType::VARCHAR, PdfWatermarkOpacityFun));
	loader.RegisterFunction(pdf_watermark_set);

	loader.RegisterFunction(ScalarFunction(
	    "pdf_bates", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT},
	    LogicalType::VARCHAR, PdfBatesFun));

	TableFunction pdf_form_fields("pdf_form_fields", {LogicalType::VARCHAR}, PdfFormFieldsScan, PdfFormFieldsBind,
	                              PdfQpdfRowsInit);
	loader.RegisterFunction(pdf_form_fields);

	TableFunction pdf_annotations("pdf_annotations", {LogicalType::VARCHAR}, PdfAnnotationsScan, PdfAnnotationsBind,
	                              PdfQpdfRowsInit);
	loader.RegisterFunction(pdf_annotations);

	// pdf_signatures: detect + verify existing digital signatures (no signing).
	TableFunction pdf_signatures("pdf_signatures", {LogicalType::VARCHAR}, PdfSignaturesScan, PdfSignaturesBind,
	                             PdfQpdfRowsInit);
	pdf_signatures.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(pdf_signatures);

	// pdf_images: extract embedded raster image XObjects (stored JPEG/bitmap).
	TableFunction pdf_images("pdf_images", {LogicalType::VARCHAR}, PdfImagesScan, PdfImagesBind, PdfQpdfRowsInit);
	pdf_images.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(pdf_images);

	// pdf_sign: create an adbe.pkcs7.detached CMS signature (inverse of
	// pdf_signatures). Single-row table function so it can carry named options.
	TableFunction pdf_sign("pdf_sign",
	                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                       PdfSignScan, PdfSignBind, PdfQpdfRowsInit);
	pdf_sign.named_parameters["key_password"] = LogicalType::VARCHAR;
	pdf_sign.named_parameters["reason"] = LogicalType::VARCHAR;
	pdf_sign.named_parameters["location"] = LogicalType::VARCHAR;
	pdf_sign.named_parameters["signer_name"] = LogicalType::VARCHAR;
	pdf_sign.named_parameters["field_name"] = LogicalType::VARCHAR;
	pdf_sign.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(pdf_sign);

	// pdf_redact: true raster redaction (removes text under the boxes, not a
	// black rectangle over live text). redactions in PDF points, origin
	// bottom-left. Side effects (render + write) happen at scan time.
	auto redact_struct = LogicalType::STRUCT({{"page", LogicalType::INTEGER},
	                                          {"x", LogicalType::DOUBLE},
	                                          {"y", LogicalType::DOUBLE},
	                                          {"w", LogicalType::DOUBLE},
	                                          {"h", LogicalType::DOUBLE}});
	TableFunction pdf_redact("pdf_redact",
	                         {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::LIST(redact_struct)},
	                         PdfRedactScan, PdfRedactBind, PdfRedactInit);
	pdf_redact.named_parameters["dpi"] = LogicalType::INTEGER;
	pdf_redact.named_parameters["password"] = LogicalType::VARCHAR;
	loader.RegisterFunction(pdf_redact);

	// COPY TO pdf
	CopyFunction pdf_copy("pdf");
	pdf_copy.extension = "pdf";
	pdf_copy.copy_to_bind = PdfCopyBind;
	pdf_copy.copy_to_initialize_global = PdfCopyInitializeGlobal;
	pdf_copy.copy_to_initialize_local = PdfCopyInitializeLocal;
	pdf_copy.copy_to_sink = PdfCopySink;
	pdf_copy.copy_to_combine = PdfCopyCombine;
	pdf_copy.copy_to_finalize = PdfCopyFinalize;
	pdf_copy.execution_mode = PdfCopyExecutionMode;
	loader.RegisterFunction(pdf_copy);
}

void PdfExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string PdfExtension::Name() {
	return "pdf";
}
std::string PdfExtension::Version() const {
#ifdef EXT_VERSION_PDF
	return EXT_VERSION_PDF;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(pdf, loader) {
	duckdb::LoadInternal(loader);
}
}
