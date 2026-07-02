#include <hpdf.h>
#include <stdio.h>

int main(void) {
    HPDF_Doc pdf = HPDF_New(NULL, NULL);
    if (!pdf) { fprintf(stderr, "HPDF_New failed\n"); return 1; }

    HPDF_Page page = HPDF_AddPage(pdf);
    HPDF_Page_SetSize(page, HPDF_PAGE_SIZE_LETTER, HPDF_PAGE_PORTRAIT);

    float page_h = HPDF_Page_GetHeight(page);  /* 792 for letter */
    float margin = 72.0f;

    HPDF_Font hb24 = HPDF_GetFont(pdf, "Helvetica-Bold", NULL);
    HPDF_Font hb14 = HPDF_GetFont(pdf, "Helvetica-Bold", NULL);
    HPDF_Font h10  = HPDF_GetFont(pdf, "Helvetica", NULL);
    HPDF_Font hb10 = HPDF_GetFont(pdf, "Helvetica-Bold", NULL);

    float y = page_h - margin;

    /* 24pt Bold title */
    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, hb24, 24);
    HPDF_Page_TextOut(page, margin, y, "Markdown Fixture Title");
    HPDF_Page_EndText(page);
    y -= 36.0f;

    /* 14pt Bold subheading */
    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, hb14, 14);
    HPDF_Page_TextOut(page, margin, y, "Introduction Section");
    HPDF_Page_EndText(page);
    y -= 22.0f;

    /* 10pt body lines */
    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, h10, 10);
    HPDF_Page_TextOut(page, margin, y, "This document tests layout-aware markdown extraction.");
    HPDF_Page_EndText(page);
    y -= 16.0f;

    /* 10pt body with inline bold word mid-sentence */
    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, h10, 10);
    HPDF_Page_TextOut(page, margin, y, "The word");
    HPDF_Page_EndText(page);

    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, hb10, 10);
    HPDF_Page_TextOut(page, margin + 60.0f, y, "important");
    HPDF_Page_EndText(page);

    HPDF_Page_BeginText(page);
    HPDF_Page_SetFontAndSize(page, h10, 10);
    HPDF_Page_TextOut(page, margin + 120.0f, y, "is bold here.");
    HPDF_Page_EndText(page);
    y -= 22.0f;

    /* 3-item bullet list using "- " prefix (ASCII, avoids encoding edge cases) */
    const char *bullets[] = {"- item one", "- item two", "- item three"};
    for (int i = 0; i < 3; i++) {
        HPDF_Page_BeginText(page);
        HPDF_Page_SetFontAndSize(page, h10, 10);
        HPDF_Page_TextOut(page, margin, y, bullets[i]);
        HPDF_Page_EndText(page);
        y -= 16.0f;
    }
    y -= 8.0f;

    /* 3-column x 4-row aligned table at fixed x positions 72, 250, 430
       so column clustering reliably detects 3 columns.
       Row 0 = header; rows 1-3 = data. */
    float cols[3] = {72.0f, 250.0f, 430.0f};
    const char *table_data[4][3] = {
        {"Name",    "Quantity", "Price"},
        {"Apples",  "50",       "1.20"},
        {"Bananas", "30",       "0.50"},
        {"Cherries","100",      "3.99"},
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            HPDF_Page_BeginText(page);
            HPDF_Page_SetFontAndSize(page, h10, 10);
            HPDF_Page_TextOut(page, cols[c], y, table_data[r][c]);
            HPDF_Page_EndText(page);
        }
        y -= 16.0f;
    }

    HPDF_SaveToFile(pdf, "test/data/markdown_fixture.pdf");
    HPDF_Free(pdf);
    printf("Wrote test/data/markdown_fixture.pdf\n");
    return 0;
}
