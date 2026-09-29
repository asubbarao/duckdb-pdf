/*
 * gen_table_fixtures.c — reproducible fixtures for read_pdf_tables lattice /
 * right-aligned column tests.
 *
 * Build & run (from repo root, needs libharu):
 *   cc -O2 -o /tmp/gen_table_fixtures test/data/gen_table_fixtures.c \
 *      $(pkg-config --cflags --libs libhpdf 2>/dev/null || echo "-lhpdf") \
 *      -I/opt/homebrew/include -L/opt/homebrew/lib -lhpdf
 *   /tmp/gen_table_fixtures test/data
 *
 * Writes:
 *   test/data/financial_right_aligned.pdf  — borderless, right-aligned amounts
 *   test/data/ruled_lattice.pdf            — fully bordered 3x4 lattice table
 *   test/data/mixed_prose_table.pdf        — prose around a borderless table
 */
#include <hpdf.h>
#include <stdio.h>
#include <string.h>

static void write_right_aligned(HPDF_Page page, HPDF_Font font, float size, float right_x, float y, const char *text) {
	HPDF_Page_SetFontAndSize(page, font, size);
	float w = HPDF_Page_TextWidth(page, text);
	HPDF_Page_BeginText(page);
	HPDF_Page_TextOut(page, right_x - w, y, text);
	HPDF_Page_EndText(page);
}

static void write_left(HPDF_Page page, HPDF_Font font, float size, float x, float y, const char *text) {
	HPDF_Page_SetFontAndSize(page, font, size);
	HPDF_Page_BeginText(page);
	HPDF_Page_TextOut(page, x, y, text);
	HPDF_Page_EndText(page);
}

/* Borderless financial table: labels left, amounts right-aligned on a common
 * xMax so naive xMin column clustering would shatter the numeric column. */
static int make_financial(const char *path) {
	HPDF_Doc pdf = HPDF_New(NULL, NULL);
	if (!pdf) {
		return 1;
	}
	HPDF_Page page = HPDF_AddPage(pdf);
	HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
	HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", NULL);
	HPDF_Font bold = HPDF_GetFont(pdf, "Helvetica-Bold", NULL);

	float label_x = 72.0f;
	float amount_right = 400.0f; /* common right edge for all amounts */
	float y = 720.0f;
	float row_h = 18.0f;

	write_left(page, bold, 11, label_x, y, "Account");
	write_right_aligned(page, bold, 11, amount_right, y, "Balance");
	y -= row_h;

	struct {
		const char *label;
		const char *amount;
	} rows[] = {
	    {"Cash", "1,234.56"},
	    {"Receivables", "89.00"},
	    {"Inventory", "12,500.75"},
	    {"Total assets", "13,824.31"},
	};
	for (int i = 0; i < 4; i++) {
		write_left(page, font, 10, label_x, y, rows[i].label);
		write_right_aligned(page, font, 10, amount_right, y, rows[i].amount);
		y -= row_h;
	}

	HPDF_STATUS st = HPDF_SaveToFile(pdf, path);
	HPDF_Free(pdf);
	return st != HPDF_OK;
}

/* Ruled / lattice table: outer border + internal grid lines (3 cols x 4 rows). */
static int make_ruled(const char *path) {
	HPDF_Doc pdf = HPDF_New(NULL, NULL);
	if (!pdf) {
		return 1;
	}
	HPDF_Page page = HPDF_AddPage(pdf);
	HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
	HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", NULL);
	HPDF_Font bold = HPDF_GetFont(pdf, "Helvetica-Bold", NULL);

	/* Table geometry in PDF user space (origin bottom-left). */
	const float x0 = 72.0f, x1 = 220.0f, x2 = 340.0f, x3 = 460.0f;
	const float y0 = 560.0f, y1 = 590.0f, y2 = 620.0f, y3 = 650.0f, y4 = 680.0f;
	/* rows top→bottom between y4..y3, y3..y2, y2..y1, y1..y0 */

	HPDF_Page_SetLineWidth(page, 0.8f);
	/* vertical rules */
	float vx[] = {x0, x1, x2, x3};
	for (int i = 0; i < 4; i++) {
		HPDF_Page_MoveTo(page, vx[i], y0);
		HPDF_Page_LineTo(page, vx[i], y4);
		HPDF_Page_Stroke(page);
	}
	/* horizontal rules */
	float hy[] = {y0, y1, y2, y3, y4};
	for (int i = 0; i < 5; i++) {
		HPDF_Page_MoveTo(page, x0, hy[i]);
		HPDF_Page_LineTo(page, x3, hy[i]);
		HPDF_Page_Stroke(page);
	}

	/* Cell text: pad a few points inside each cell, baseline near cell bottom+6 */
	const char *cells[4][3] = {
	    {"Region", "Units", "Revenue"},
	    {"North", "10", "1000"},
	    {"South", "20", "2500"},
	    {"West", "15", "1800"},
	};
	float col_left[] = {x0 + 6.0f, x1 + 6.0f, x2 + 6.0f};
	float row_base[] = {y3 + 8.0f, y2 + 8.0f, y1 + 8.0f, y0 + 8.0f}; /* top row first */

	for (int r = 0; r < 4; r++) {
		HPDF_Font f = (r == 0) ? bold : font;
		for (int c = 0; c < 3; c++) {
			write_left(page, f, 10, col_left[c], row_base[r], cells[r][c]);
		}
	}

	HPDF_STATUS st = HPDF_SaveToFile(pdf, path);
	HPDF_Free(pdf);
	return st != HPDF_OK;
}

/* Mixed prose and a borderless table: the table has one physical continuation
 * line so the reader must segment the page before reconstructing its grid. */
static int make_mixed_prose(const char *path) {
	HPDF_Doc pdf = HPDF_New(NULL, NULL);
	if (!pdf) {
		return 1;
	}
	HPDF_Page page = HPDF_AddPage(pdf);
	HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
	HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", NULL);
	HPDF_Font bold = HPDF_GetFont(pdf, "Helvetica-Bold", NULL);

	write_left(page, font, 10, 72.0f, 720.0f, "This paragraph explains the report before the data begins.");
	write_left(page, font, 10, 72.0f, 704.0f, "It is ordinary prose and must not become a table row.");
	write_left(page, font, 10, 72.0f, 672.0f, "A second paragraph gives context for the examples below.");
	write_left(page, font, 10, 72.0f, 656.0f, "Its ragged lines are separate from the aligned table columns.");

	const float x[] = {72.0f, 240.0f, 400.0f};
	const float y[] = {620.0f, 598.0f, 576.0f, 554.0f};
	const char *cells[4][3] = {
	    {"Category", "Example", "Considerations"},
	    {"Agents", "Prospecting agent", "Many inference calls"},
	    {"Chat", "Customer support chat", "Fast first token"},
	    {"Voice", "Real-time translation", "End-to-end latency"},
	};
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 3; c++) {
			write_left(page, r == 0 ? bold : font, 10, x[c], y[r], cells[r][c]);
		}
	}
	write_left(page, font, 10, x[2], 538.0f, "for natural conversation");

	write_left(page, font, 10, 72.0f, 506.0f, "This paragraph follows the table and is not tabular data.");
	write_left(page, font, 10, 72.0f, 490.0f, "The page ends with another ordinary prose line.");

	HPDF_STATUS st = HPDF_SaveToFile(pdf, path);
	HPDF_Free(pdf);
	return st != HPDF_OK;
}

int main(int argc, char **argv) {
	const char *dir = (argc > 1) ? argv[1] : "test/data";
	char path[1024];

	snprintf(path, sizeof(path), "%s/financial_right_aligned.pdf", dir);
	if (make_financial(path)) {
		fprintf(stderr, "failed to write %s\n", path);
		return 1;
	}
	fprintf(stdout, "wrote %s\n", path);

	snprintf(path, sizeof(path), "%s/ruled_lattice.pdf", dir);
	if (make_ruled(path)) {
		fprintf(stderr, "failed to write %s\n", path);
		return 1;
	}
	fprintf(stdout, "wrote %s\n", path);

	snprintf(path, sizeof(path), "%s/mixed_prose_table.pdf", dir);
	if (make_mixed_prose(path)) {
		fprintf(stderr, "failed to write %s\n", path);
		return 1;
	}
	fprintf(stdout, "wrote %s\n", path);
	return 0;
}
