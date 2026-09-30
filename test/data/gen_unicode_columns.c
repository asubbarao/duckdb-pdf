/* Generate a one-column page whose repeated word gaps resemble a gutter. */

#include <stdio.h>
#include <string.h>

static long object_offsets[7];

static void write_object(FILE *out, int number, const char *body) {
	object_offsets[number] = ftell(out);
	fprintf(out, "%d 0 obj\n%s\nendobj\n", number, body);
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "test/data/unicode_columns.pdf";
	const char *to_unicode = "/CIDInit /ProcSet findresource begin\n"
	                         "12 dict begin\n"
	                         "begincmap\n"
	                         "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
	                         "/CMapName /Adobe-Identity-UCS def\n"
	                         "/CMapType 2 def\n"
	                         "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
	                         "5 beginbfchar\n"
	                         "<61> <0061>\n<63> <0063>\n<65> <0065>\n<66> <0066>\n<E9> <00E9>\n"
	                         "endbfchar\n"
	                         "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
	char content[30000];
	char font[512];
	char page[256];
	char stream[31000];
	char cmap[2048];
	char xref[1024];
	long xref_offset;
	size_t content_size = 0;
	FILE *out = fopen(path, "wb");
	if (!out) {
		perror(path);
		return 1;
	}

	for (int row = 0; row < 20; row++) {
		float y = 740.0f - 25.0f * (float)row;
		content_size += (size_t)snprintf(content + content_size, sizeof(content) - content_size,
		                                 "BT\n/F1 10 Tf\n1 0 0 1 72 %.1f Tm\n(caf\xe9) Tj\n"
		                                 "1 0 0 1 104 %.1f Tm\n(caf\xe9) Tj\n"
		                                 "1 0 0 1 136 %.1f Tm\n(caf\xe9) Tj\n"
		                                 "1 0 0 1 168 %.1f Tm\n(caf\xe9) Tj\n"
		                                 "1 0 0 1 200 %.1f Tm\n(caf\xe9) Tj\n"
		                                 "1 0 0 1 232 %.1f Tm\n(caf\xe9) Tj\n"
		                                 "1 0 0 1 264.5 %.1f Tm\n(cafe) Tj\n"
		                                 "1 0 0 1 296.5 %.1f Tm\n(cafe) Tj\n"
		                                 "1 0 0 1 328.5 %.1f Tm\n(cafe) Tj\n"
		                                 "1 0 0 1 360.5 %.1f Tm\n(cafe) Tj\n"
		                                 "1 0 0 1 392.5 %.1f Tm\n(cafe) Tj\n"
		                                 "1 0 0 1 424.5 %.1f Tm\n(cafe) Tj\nET\n",
		                                 y, y, y, y, y, y, y, y, y, y, y, y);
	}

	fputs("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n", out);
	write_object(out, 1, "<< /Type /Catalog /Pages 2 0 R >>");
	write_object(out, 2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
	snprintf(page, sizeof(page),
	         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
	         "/Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >>");
	write_object(out, 3, page);
	snprintf(font, sizeof(font),
	         "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
	         "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>");
	write_object(out, 4, font);
	snprintf(cmap, sizeof(cmap), "<< /Length %zu >>\nstream\n%sendstream", strlen(to_unicode), to_unicode);
	write_object(out, 5, cmap);
	snprintf(stream, sizeof(stream), "<< /Length %zu >>\nstream\n%sendstream", content_size, content);
	write_object(out, 6, stream);

	xref_offset = ftell(out);
	fputs("xref\n0 7\n0000000000 65535 f \n", out);
	for (int i = 1; i <= 6; i++) {
		snprintf(xref, sizeof(xref), "%010ld 00000 n \n", object_offsets[i]);
		fputs(xref, out);
	}
	fprintf(out, "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref_offset);
	fclose(out);
	return 0;
}
