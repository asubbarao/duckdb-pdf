#!/usr/bin/env bash
# Prove bundled base-14 fonts make pdf_redact work WITHOUT system fontconfig.
#
# Forces poppler/fontconfig to see an empty font set (same env shape as the
# fail-loudly manual repro), then redacts test/data/redact_secret.pdf and checks:
#   1. pdf_redact succeeds (no missing-display-font IOException)
#   2. unredacted page text is still extractable from the output
#      (proves the raster is not blank — only bundled fonts can resolve Helvetica)
#
# Usage (from repo root, after `make release`):
#   bash scripts/test_font_starved_redact.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="${ROOT}/build/release/extension/pdf/pdf.duckdb_extension"
DUCKDB="${ROOT}/build/release/duckdb"
PDF="${ROOT}/test/data/redact_secret.pdf"

if [[ ! -x "$DUCKDB" ]]; then
  echo "missing duckdb binary: $DUCKDB (run: GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL=4 make release)" >&2
  exit 1
fi
if [[ ! -f "$EXT" ]]; then
  echo "missing extension: $EXT" >&2
  exit 1
fi
if [[ ! -f "$PDF" ]]; then
  echo "missing fixture: $PDF" >&2
  exit 1
fi

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/pdf_font_starved.XXXXXX")"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

# Empty fontconfig tree + empty HOME so CoreText/fontconfig find nothing useful.
mkdir -p "$WORKDIR/empty-fc/fonts" "$WORKDIR/empty-home"
cat >"$WORKDIR/empty-fc/fonts.conf" <<'XML'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>/nonexistent/duckdb_pdf_no_system_fonts</dir>
  <cachedir>/nonexistent/duckdb_pdf_no_font_cache</cachedir>
</fontconfig>
XML

OUT="$WORKDIR/redacted.pdf"
export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}"
# Keep PATH so duckdb can start; strip font discovery via FONTCONFIG_* + HOME.
# env -i is the only reliable way on macOS to stop CoreText/fontconfig fallbacks.
set +e
RESULT="$(
  env -i \
    PATH="$PATH" \
    HOME="$WORKDIR/empty-home" \
    FONTCONFIG_PATH="$WORKDIR/empty-fc" \
    FONTCONFIG_FILE="$WORKDIR/empty-fc/fonts.conf" \
    TMPDIR="${TMPDIR:-/tmp}" \
    TMP="${TMP:-/tmp}" \
    "$DUCKDB" -csv -c "
LOAD '$EXT';
SELECT page, redacted, boxes_applied
FROM pdf_redact('$PDF', '$OUT',
  [{'page': 2, 'x': 60.0, 'y': 590.0, 'w': 260.0, 'h': 38.0}]);
SELECT count(*) AS secret_gone
FROM read_pdf('$OUT', auto_ocr := false)
WHERE text LIKE '%SECRET%';
SELECT count(*) AS page_one_ok
FROM read_pdf('$OUT', auto_ocr := false)
WHERE text LIKE '%PAGE ONE PUBLIC HEADER%';
" 2>"$WORKDIR/stderr.txt"
)"
RC=$?
set -e

echo "--- duckdb stdout ---"
echo "$RESULT"
echo "--- duckdb stderr (tail) ---"
tail -n 40 "$WORKDIR/stderr.txt" || true

if [[ $RC -ne 0 ]]; then
  echo "FAIL: duckdb exited $RC under font-starved env (bundled fonts should make redaction succeed)" >&2
  exit 1
fi

if echo "$RESULT" | grep -qi 'could not find display fonts\|IO Error\|IOException'; then
  echo "FAIL: missing-display-font error still reached the client" >&2
  exit 1
fi

if ! echo "$RESULT" | grep -q '2,true,1\|2	true	1'; then
  # csv mode uses commas
  if ! echo "$RESULT" | grep -E -q '^2,true,1$'; then
    echo "FAIL: expected page 2 redacted=true boxes_applied=1" >&2
    exit 1
  fi
fi

# secret_gone should be 0; page_one_ok should be 1
if ! echo "$RESULT" | grep -E -q '^0$'; then
  echo "FAIL: expected secret_gone count 0 (secret still present or query failed)" >&2
  exit 1
fi
if ! echo "$RESULT" | grep -E -q '^1$'; then
  echo "FAIL: expected page_one_ok count 1 (unredacted text missing → blank raster?)" >&2
  exit 1
fi

if grep -qi "No display font\|Couldn't find a font\|Could not find a font" "$WORKDIR/stderr.txt"; then
  echo "FAIL: poppler still emitted missing-font errors under font-starved env" >&2
  exit 1
fi

echo "PASS: font-starved pdf_redact succeeded using only bundled base-14 fonts"
exit 0
