/* Generate a minimal PDF whose text stream contains a ligature and an abutting run. */

#include <stdio.h>
#include <string.h>

static long object_offsets[7];

static void write_object(FILE *out, int number, const char *body) {
	object_offsets[number] = ftell(out);
	fprintf(out, "%d 0 obj\n%s\nendobj\n", number, body);
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "test/data/ligature.pdf";
	const char *to_unicode = "/CIDInit /ProcSet findresource begin\n"
	                          "12 dict begin\n"
	                          "begincmap\n"
	                          "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
	                          "/CMapName /Adobe-Identity-UCS def\n"
	                          "/CMapType 2 def\n"
	                          "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
	                          "1 beginbfchar\n<1F> <00660069>\nendbfchar\n"
	                          "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
	const char *content = "BT\n"
	                      "/F1 12 Tf\n"
	                      "1 0 0 1 36 150 Tm\n"
	                      "[(Pro\\037) 258 ( ) 42 (ling )] TJ\n"
	                      "1 0 0 1 36 120 Tm\n"
	                      "(hello) Tj\n"
	                      "1 0 0 1 67 120 Tm\n"
	                      "(world) Tj\n"
	                      "ET\n";
	char font[512];
	char page[256];
	char stream[1024];
	char cmap[1024];
	char xref[1024];
	long xref_offset;
	FILE *out = fopen(path, "wb");
	if (!out) {
		perror(path);
		return 1;
	}
	fputs("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n", out);
	write_object(out, 1, "<< /Type /Catalog /Pages 2 0 R >>");
	write_object(out, 2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
	snprintf(page, sizeof(page),
	         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] "
	                      "/Resources << /Font << /F1 4 0 R >> >> /Contents 6 0 R >>");
	write_object(out, 3, page);
	snprintf(font, sizeof(font),
	         "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
	         "/Encoding << /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [31 /f_i] >> "
	         "/ToUnicode 5 0 R >>");
	write_object(out, 4, font);
	snprintf(cmap, sizeof(cmap), "<< /Length %zu >>\nstream\n%sendstream", strlen(to_unicode), to_unicode);
	write_object(out, 5, cmap);
	snprintf(stream, sizeof(stream), "<< /Length %zu >>\nstream\n%sendstream", strlen(content), content);
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
