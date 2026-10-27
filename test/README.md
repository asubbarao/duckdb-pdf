# Testing this extension

Build from the repository root before testing:

```bash
make release
make test
PDF_NETWORK_TESTS=1 make test
make format-check
```

`test/sql` contains SQLLogicTests. The default run omits tests gated by
`PDF_NETWORK_TESTS` and `LIBREOFFICE_PATH`. Network tests fetch real PDFs and
install httpfs/quack into the build repository. To exercise Office conversion,
set `LIBREOFFICE_PATH` to your LibreOffice executable. Inspect skipped-test
reasons; a skipped dependency or zero selected cases is not validation.

The normal `make test` targets also run `office_conversion_test.cpp`: simulated
backends verify Microsoft → Apple → LibreOffice routing, stop-on-success and
retention of all export errors without requiring desktop apps. These are routing
tests, not evidence of real Excel/Word/PowerPoint or iWork exports. Those require
installed applications, an interactive macOS session and Automation permission.

The Makefile enables extension autoloading and `--fail-require-skip` in the
DuckDB test runner. DuckDB 2.0 isolates each test's extension installation;
companions must be available in the build repository. The build guard compares
the loaded extension's version against the artifact selected by the Makefile
and checks the pinned stable host identity. Do not replace these checks with a
successful CLI query against a globally installed binary.

For an explicit artifact check, start the built CLI from the repository root:

```bash
./build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/pdf/pdf.duckdb_extension';
SELECT * FROM pragma_version();
SELECT extension_name, loaded, extension_version
FROM duckdb_extensions() WHERE extension_name = 'pdf';
```

Debug builds use `make debug` then `make test_debug`. The Makefile currently
derives `EXPECTED_EXT_VERSION` from the release artifact, so keep that artifact
at the same source revision when running the debug suite.
