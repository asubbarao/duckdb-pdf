# Bundled PDF base-14 substitute fonts

These OpenType fonts are the free URW++ core base-35 substitutes for PDF's
standard 14 fonts (Helvetica, Times, Courier, Symbol, ZapfDingbats families).
They are registered with poppler at extension load/render time so
`pdf_redact` / page rasterization works on community-extension binaries built
against vcpkg poppler **without** fontconfig or system display fonts.

## Source

- Upstream: [ArtifexSoftware/urw-base35-fonts](https://github.com/ArtifexSoftware/urw-base35-fonts)
- Release tag: **20200910**
- Files kept here: the 14 OTFs needed for base-14 coverage only (not the full
  base-35 set).

## Mapping (PDF name → file)

| PDF base-14 name        | File                        |
|-------------------------|-----------------------------|
| Helvetica               | NimbusSans-Regular.otf      |
| Helvetica-Bold          | NimbusSans-Bold.otf         |
| Helvetica-Oblique       | NimbusSans-Italic.otf       |
| Helvetica-BoldOblique   | NimbusSans-BoldItalic.otf   |
| Times-Roman             | NimbusRoman-Regular.otf     |
| Times-Bold              | NimbusRoman-Bold.otf        |
| Times-Italic            | NimbusRoman-Italic.otf      |
| Times-BoldItalic        | NimbusRoman-BoldItalic.otf  |
| Courier                 | NimbusMonoPS-Regular.otf    |
| Courier-Bold            | NimbusMonoPS-Bold.otf       |
| Courier-Oblique         | NimbusMonoPS-Italic.otf     |
| Courier-BoldOblique     | NimbusMonoPS-BoldItalic.otf |
| Symbol                  | StandardSymbolsPS.otf       |
| ZapfDingbats            | D050000L.otf                |

## License

AGPL-3.0 (see `LICENSE` and full `COPYING`), with Artifex's special exception
allowing inclusion of the font programs in PostScript/PDF documents. The
fonts are redistributed here as separate data files under their original
license; see `LICENSE`.

The extension embeds these bytes into the single `.duckdb_extension` binary
(community-extensions ships no companion data files), extracts them to a
process temp directory once, and registers them with poppler via
`GlobalParams::addFontFile` so resolution does not depend on fontconfig.
