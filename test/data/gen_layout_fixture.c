/*
 * gen_layout_fixture.c — reproducible fixtures for two reading-order failures
 * measured on arXiv 2610.39302 (2026-11-08).
 *
 * Build and run from the repository root with libharu to regenerate
 * test/data/rotated_margin_stamp.pdf, two_columns_full_width_figure.pdf,
 * single_column_function_table.pdf, single_column_wide_table.pdf and
 * single_column_loss_run.pdf:
 *
 *   cc -o /tmp/gen_layout_fixture test/data/gen_layout_fixture.c -lhpdf && /tmp/gen_layout_fixture
 */
#include <hpdf.h>
#include <stdio.h>

static void write_text(HPDF_Page page, HPDF_Font font, float size, float x, float y, const char *text) {
	HPDF_Page_SetFontAndSize(page, font, size);
	HPDF_Page_BeginText(page);
	HPDF_Page_TextOut(page, x, y, text);
	HPDF_Page_EndText(page);
}

/* Text turned a quarter turn counter-clockwise: it reads bottom to top. */
static void write_turned_text(HPDF_Page page, HPDF_Font font, float size, float x, float y, const char *text) {
	HPDF_Page_SetFontAndSize(page, font, size);
	HPDF_Page_BeginText(page);
	HPDF_Page_SetTextMatrix(page, 0.0f, 1.0f, -1.0f, 0.0f, x, y);
	HPDF_Page_ShowText(page, text);
	HPDF_Page_EndText(page);
}

static int begin_page(HPDF_Doc *pdf, HPDF_Page *page, HPDF_Font *font) {
	*pdf = HPDF_New(NULL, NULL);
	if (!*pdf) {
		return 1;
	}
	*page = HPDF_AddPage(*pdf);
	HPDF_Page_SetSize(*page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
	*font = HPDF_GetFont(*pdf, "Helvetica", NULL);
	if (!*page || !*font) {
		HPDF_Free(*pdf);
		return 1;
	}
	return 0;
}

static int save(HPDF_Doc pdf, const char *path) {
	HPDF_STATUS status = HPDF_SaveToFile(pdf, path);
	HPDF_Free(pdf);
	return status == HPDF_OK ? 0 : 1;
}

/*
 * arXiv's margin stamp: 20pt type turned to read bottom to top beside an abstract of
 * 10pt lines on a 12pt pitch. Each turned word's box is 20pt wide and as tall as the
 * word is long, so it overlaps several lines of the abstract.
 */
static int write_rotated_margin_stamp(const char *path) {
	HPDF_Doc pdf;
	HPDF_Page page;
	HPDF_Font font;
	if (begin_page(&pdf, &page, &font) != 0) {
		return 1;
	}
	for (int i = 0; i < 20; i++) {
		char line[128];
		snprintf(line, sizeof(line), "Abstract line %02d reads left to right across the page without interruption.",
		         i + 1);
		write_text(page, font, 10.0f, 72.0f, 600.0f - 12.0f * (float)i, line);
	}
	write_turned_text(page, font, 20.0f, 44.0f, 380.0f, "arXiv:0000.00000v1 [cs.AI] 29 Oct 2026");
	return save(pdf, path);
}

/*
 * A two-column page under a full-width figure: axis labels and a caption cross the
 * gutter and carry about a quarter of the page's words, while the two body columns
 * below it carry the rest. arXiv 2610.39302 pages 6, 10 and 11 have this shape.
 */
static int write_two_columns_full_width_figure(const char *path) {
	HPDF_Doc pdf;
	HPDF_Page page;
	HPDF_Font font;
	if (begin_page(&pdf, &page, &font) != 0) {
		return 1;
	}
	write_text(page, font, 8.0f, 90.0f, 740.0f, "axis 0.1");
	write_text(page, font, 8.0f, 280.0f, 740.0f, "samples seen across both panels");
	write_text(page, font, 8.0f, 500.0f, 740.0f, "axis 0.9");
	for (int i = 0; i < 6; i++) {
		char line[160];
		snprintf(line, sizeof(line),
		         "Figure caption line %02d spans the full page width above both body columns and keeps going right.",
		         i + 1);
		write_text(page, font, 10.0f, 72.0f, 715.0f - 13.0f * (float)i, line);
	}
	for (int i = 0; i < 24; i++) {
		char left[64];
		char right[64];
		float y = 620.0f - 13.0f * (float)i;
		snprintf(left, sizeof(left), "left-body-%02d alpha beta gamma delta", i + 1);
		snprintf(right, sizeof(right), "right-body-%02d zeta eta theta iota", i + 1);
		write_text(page, font, 10.0f, 72.0f, y, left);
		write_text(page, font, 10.0f, 330.0f, y, right);
	}
	return save(pdf, path);
}

/*
 * The opposite case at the same crossing fraction: a single-column page whose prose
 * crosses a two-column Function | Description table, as on the DuckDB documentation's
 * function pages. The table's gap lines up like a gutter, and the prose holds less than
 * half of the words, but the sides are a narrow name column and a wide description
 * column, so the page stays one column and each row reads name then description.
 */
static int write_single_column_function_table(const char *path) {
	HPDF_Doc pdf;
	HPDF_Page page;
	HPDF_Font font;
	if (begin_page(&pdf, &page, &font) != 0) {
		return 1;
	}
	/* Five prose lines of 19 words beside 14 rows of 9: about 43% of the words sit on
	 * lines that cross the table's gap, inside the band the reading-column test decides. */
	for (int i = 0; i < 5; i++) {
		char line[160];
		snprintf(
		    line, sizeof(line),
		    "Prose line %02d explains the functions in the table below and runs across the whole width of the page.",
		    i + 1);
		float y = i < 3 ? 740.0f - 14.0f * (float)i : 380.0f - 14.0f * (float)(i - 3);
		write_text(page, font, 10.0f, 72.0f, y, line);
	}
	/* Names wide enough that each side clears the minimum column width, so only the
	 * reading-column test can keep the page in one piece. */
	for (int r = 0; r < 14; r++) {
		char name[48];
		char description[96];
		float y = 660.0f - 16.0f * (float)r;
		snprintf(name, sizeof(name), "function_name_number_%02d(value)", r + 1);
		snprintf(description, sizeof(description), "Returns what function %02d computes from its argument.", r + 1);
		write_text(page, font, 10.0f, 72.0f, y, name);
		write_text(page, font, 10.0f, 260.0f, y, description);
	}
	return save(pdf, path);
}

/*
 * A single-column page with a wide table above code, as on DuckDB blog page 603: the
 * table's middle gap lines up like a gutter, both halves are about as wide and as
 * word-dense as prose columns, but only the left side continues below the table. The
 * page stays one column and each table row reads left to right.
 */
static int write_single_column_wide_table(const char *path) {
	HPDF_Doc pdf;
	HPDF_Page page;
	HPDF_Font font;
	if (begin_page(&pdf, &page, &font) != 0) {
		return 1;
	}
	/* Ten prose lines put about a quarter of the words on lines crossing the table's gap:
	 * above the 20% at which a few figure labels are let through, as on page 603. */
	for (int i = 0; i < 10; i++) {
		char line[160];
		snprintf(line, sizeof(line),
		         "Prose line %02d explains the table and the code below it and runs across the whole page width.",
		         i + 1);
		float y = i < 5 ? 740.0f - 14.0f * (float)i : 120.0f - 14.0f * (float)(i - 5);
		write_text(page, font, 10.0f, 72.0f, y, line);
	}
	/* Cells of words rather than numbers, so the alphabetic-word test passes too and only
	 * the line-count test keeps this table in one piece. */
	for (int r = 0; r < 10; r++) {
		char left[64];
		char right[96];
		float y = 670.0f - 14.0f * (float)r;
		snprintf(left, sizeof(left), "row %02d keeps alpha beta gamma", r + 1);
		snprintf(right, sizeof(right), "delta epsilon zeta eta theta iota kappa done");
		write_text(page, font, 10.0f, 72.0f, y, left);
		write_text(page, font, 10.0f, 345.0f, y, right);
	}
	/* Code wide enough that the two sides are about equally wide, so only the line-count
	 * test separates this table from a pair of reading columns. */
	for (int c = 0; c < 26; c++) {
		char code[64];
		snprintf(code, sizeof(code), "SELECT a, b, c, d, e FROM table_%02d WHERE k = 1;", c + 1);
		write_text(page, font, 10.0f, 72.0f, 520.0f - 14.0f * (float)c, code);
	}
	return save(pdf, path);
}

/*
 * A loss-run table, as carriers print them: prose header and disclaimer lines across the
 * page, and rows whose policy and date cells sit left of a gap and whose dollar amounts
 * sit right of it. Both halves are about as wide and as many cells per row as prose
 * columns, and both run the full table, so only the alphabetic-word test tells them from
 * reading columns. The page stays one column and each row reads policy to total.
 */
static int write_single_column_loss_run(const char *path) {
	HPDF_Doc pdf;
	HPDF_Page page;
	HPDF_Font font;
	if (begin_page(&pdf, &page, &font) != 0) {
		return 1;
	}
	for (int i = 0; i < 8; i++) {
		char line[160];
		snprintf(line, sizeof(line),
		         "Disclaimer line %02d says the loss information below may not be current and runs across the page.",
		         i + 1);
		float y = i < 3 ? 740.0f - 14.0f * (float)i : 200.0f - 14.0f * (float)(i - 3);
		write_text(page, font, 10.0f, 72.0f, y, line);
	}
	for (int r = 0; r < 20; r++) {
		char left[64];
		char right[96];
		float y = 680.0f - 14.0f * (float)r;
		snprintf(left, sizeof(left), "POL D3501%02d %02d 08/15/20%02d 08/15/20%02d %d", r, r, r + 5, r + 6, r % 3);
		snprintf(right, sizeof(right), "$%d,183 $0 $1,135 $0 $0 $%d,318", r + 1, r + 2);
		write_text(page, font, 10.0f, 72.0f, y, left);
		write_text(page, font, 10.0f, 330.0f, y, right);
	}
	return save(pdf, path);
}

int main(int argc, char **argv) {
	const char *stamp_path = argc > 1 ? argv[1] : "test/data/rotated_margin_stamp.pdf";
	const char *figure_path = argc > 2 ? argv[2] : "test/data/two_columns_full_width_figure.pdf";
	const char *table_path = argc > 3 ? argv[3] : "test/data/single_column_function_table.pdf";
	const char *wide_table_path = argc > 4 ? argv[4] : "test/data/single_column_wide_table.pdf";
	const char *loss_run_path = argc > 5 ? argv[5] : "test/data/single_column_loss_run.pdf";
	if (write_rotated_margin_stamp(stamp_path) != 0) {
		return 1;
	}
	if (write_two_columns_full_width_figure(figure_path) != 0) {
		return 1;
	}
	if (write_single_column_function_table(table_path) != 0) {
		return 1;
	}
	if (write_single_column_wide_table(wide_table_path) != 0) {
		return 1;
	}
	return write_single_column_loss_run(loss_run_path);
}
