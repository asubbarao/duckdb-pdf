# Skip the local PDF build guard in community CI

## Body

The build guard is intended to validate the PDF artifact built by this local checkout. Community extension CI has its own DuckDB source and sets `EXPECTED_EXT_VERSION` through `extension-ci-tools`, so the old `require-env EXPECTED_EXT_VERSION` gate did not skip there.

Export `PDF_LOCAL_BUILD_GUARD=1` alongside `EXPECTED_EXT_VERSION` from this checkout's Makefile, and require that sentinel before the existing version check. This keeps the artifact-version assertion and prevents the community job from reaching the local-only DuckDB source assertion.

## Evidence

- Community CI job 138968520845 ran `make test_release`, then `python3 ./duckdb/scripts/ci/run_tests.py --fail-require-skip ./build/release/test/unittest test/*`; the `zz_build_guard.test` failure followed because `require-env EXPECTED_EXT_VERSION` passed. [Run log](https://github.com/duckdb/community-extensions/actions/runs/45543778181/job/138968520845)
- The Makefile exports `EXPECTED_EXT_VERSION` from the just-built extension. The extension-test skill repeats this recipe and test example; it was left unchanged per task scope.
- Validation attempt used the existing `duckdb-pdf-auto-ocr` test runner. Its compiled test tree loaded that worktree's older copy and skipped on `EXPECTED_EXT_VERSION`; selecting this worktree's absolute test path matched no tests. Skip/run behavior for the edited test file therefore remains unverified.
