# Bundled PDF base-14 substitute fonts

These are free URW++ Type1 base-14 substitutes for PDF's standard 14 fonts
(Helvetica, Times, Courier, Symbol, ZapfDingbats families). They are embedded
into the extension binary and extracted at runtime so `pdf_redact` / page
rasterization works on community-extension builds against vcpkg poppler
**without** system display fonts.

## Why binary PFB + legacy filenames

vcpkg poppler often has no useful system fonts. `GlobalParams::setupBaseFonts(dir)`
looks for Ghostscript-era URW filenames (`n019003l.pfb`, …). Files here are
Artifex URW Type1 programs converted to **real binary PFB** (0x80 0x01
segments) — plain PFA text mislabeled as `.pfb` failed FreeType on community
Linux CI. At load we also write a minimal `fonts.conf` when fontconfig is
linked.

## Source

- Upstream: [ArtifexSoftware/urw-base35-fonts](https://github.com/ArtifexSoftware/urw-base35-fonts)
- Release tag: **20200910**
- Files kept: the 14 Type1 fonts needed for base-14 coverage only.

## Mapping (PDF name → file on disk)

| PDF base-14 name        | File (poppler setupBaseFonts) |
|-------------------------|-------------------------------|
| Helvetica               | `n019003l.pfb`                |
| Helvetica-Bold          | `n019004l.pfb`                |
| Helvetica-Oblique       | `n019023l.pfb`                |
| Helvetica-BoldOblique   | `n019024l.pfb`                |
| Times-Roman             | `n021003l.pfb`                |
| Times-Bold              | `n021004l.pfb`                |
| Times-Italic            | `n021023l.pfb`                |
| Times-BoldItalic        | `n021024l.pfb`                |
| Courier                 | `n022003l.pfb`                |
| Courier-Bold            | `n022004l.pfb`                |
| Courier-Oblique         | `n022023l.pfb`                |
| Courier-BoldOblique     | `n022024l.pfb`                |
| Symbol                  | `s050000l.pfb`                |
| ZapfDingbats            | `d050000l.pfb`                |

## License

AGPL-3.0 (see `LICENSE` and full `COPYING`), with Artifex's special exception
allowing inclusion of the font programs in PostScript/PDF documents.
