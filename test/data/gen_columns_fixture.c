/*
 * gen_columns_fixture.c — reproducible two-column layout fixture.
 *
 * Build and run from the repository root with libharu to regenerate
 * test/data/two_columns_crossing.pdf and test/data/single_column_table.pdf.
 */
#include <hpdf.h>
#include <stdio.h>

static void write_text(HPDF_Page page, HPDF_Font font, float size, float x, float y, const char *text) {
	HPDF_Page_SetFontAndSize(page, font, size);
	HPDF_Page_BeginText(page);
	HPDF_Page_TextOut(page, x, y, text);
	HPDF_Page_EndText(page);
}

/*
 * A single-column page: full-width prose above and below a four-column table. The
 * table's column gaps line up like a gutter, but most of the page's words are prose
 * lines that cross them, so cutting there would slice each prose line in two.
 */
static int write_single_column_table(const char *path) {
	HPDF_Doc pdf = HPDF_New(NULL, NULL);
	if (!pdf) {
		return 1;
	}
	HPDF_Page page = HPDF_AddPage(pdf);
	HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
	HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", NULL);
	if (!page || !font) {
		HPDF_Free(pdf);
		return 1;
	}
	for (int i = 0; i < 14; i++) {
		char line[128];
		snprintf(line, sizeof(line),
		         "Prose line %02d runs across the whole page in a single column of text and never stops early.", i + 1);
		float y = i < 7 ? 740.0f - 18.0f * (float)i : 300.0f - 18.0f * (float)(i - 7);
		write_text(page, font, 10.0f, 72.0f, y, line);
	}
	for (int r = 0; r < 12; r++) {
		char c0[16], c1[16], c2[16], c3[16];
		float y = 590.0f - 16.0f * (float)r;
		snprintf(c0, sizeof(c0), "Type%02d", r);
		snprintf(c1, sizeof(c1), "Store%02d", r);
		snprintf(c2, sizeof(c2), "Cast%02d", r);
		snprintf(c3, sizeof(c3), "Example%02d", r);
		write_text(page, font, 10.0f, 72.0f, y, c0);
		write_text(page, font, 10.0f, 200.0f, y, c1);
		write_text(page, font, 10.0f, 330.0f, y, c2);
		write_text(page, font, 10.0f, 450.0f, y, c3);
	}
	HPDF_STATUS status = HPDF_SaveToFile(pdf, path);
	HPDF_Free(pdf);
	return status == HPDF_OK ? 0 : 1;
}

static int write_full_width_lines(const char *path) {
	HPDF_Doc pdf = HPDF_New(NULL, NULL);
	if (!pdf) {
		return 1;
	}
	HPDF_Page page = HPDF_AddPage(pdf);
	HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
	HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", NULL);
	if (!page || !font) {
		HPDF_Free(pdf);
		return 1;
	}
	write_text(page, font, 18.0f, 150.0f, 760.0f, "Full width title spans both columns");
	for (int i = 0; i < 10; i++) {
		char left[96], right[48];
		float y = 710.0f - 18.0f * (float)i;
		snprintf(left, sizeof(left), "left-section-one-line-%02d alpha beta gamma delta epsilon", i + 1);
		snprintf(right, sizeof(right), "right-section-one-line-%02d", i + 1);
		write_text(page, font, 10.0f, 72.0f, y, left);
		write_text(page, font, 10.0f, 360.0f, y, right);
	}
	write_text(page, font, 12.0f, 160.0f, 520.0f, "Full width line separates column sections");
	for (int i = 0; i < 10; i++) {
		char left[96], right[48];
		float y = 480.0f - 18.0f * (float)i;
		snprintf(left, sizeof(left), "left-section-two-line-%02d alpha beta gamma delta epsilon", i + 1);
		snprintf(right, sizeof(right), "right-section-two-line-%02d", i + 1);
		write_text(page, font, 10.0f, 72.0f, y, left);
		write_text(page, font, 10.0f, 360.0f, y, right);
	}
	HPDF_STATUS status = HPDF_SaveToFile(pdf, path);
	HPDF_Free(pdf);
	return status == HPDF_OK ? 0 : 1;
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "test/data/two_columns_crossing.pdf";
	const char *table_path = argc > 2 ? argv[2] : "test/data/single_column_table.pdf";
	const char *spanning_path = argc > 3 ? argv[3] : "test/data/full_width_lines.pdf";
	if (write_single_column_table(table_path) != 0) {
		return 1;
	}
	if (write_full_width_lines(spanning_path) != 0) {
		return 1;
	}
	HPDF_Doc pdf = HPDF_New(NULL, NULL);
	if (!pdf) {
		return 1;
	}
	HPDF_Page page = HPDF_AddPage(pdf);
	HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);
	HPDF_Font font = HPDF_GetFont(pdf, "Helvetica", NULL);
	if (!page || !font) {
		HPDF_Free(pdf);
		return 1;
	}

	for (int i = 0; i < 20; i++) {
		float y = 730.0f - 25.0f * (float)i;
		char left[64];
		char right[32];
		snprintf(left, sizeof(left), "left-column-line-%02d alpha beta gamma delta epsilon", i + 1);
		snprintf(right, sizeof(right), "right-column-line-%02d", i + 1);
		write_text(page, font, 10.0f, 72.0f, y, left);
		write_text(page, font, 10.0f, 330.0f, y, right);
	}
	write_text(page, font, 10.0f, 288.0f, 240.0f, "gutter-bridge");

	HPDF_STATUS status = HPDF_SaveToFile(pdf, path);
	HPDF_Free(pdf);
	return status == HPDF_OK ? 0 : 1;
}
