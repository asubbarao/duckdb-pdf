# Updating DuckDB and publishing PDF

This tree targets DuckDB **v2.0.0**. Its C++ extension API is tied to that host
release; rebuilding against a new host can require source changes.

## Updating the host

1. Update the `duckdb` submodule to the intended release and the
   `extension-ci-tools` submodule to a compatible revision.
2. Update `DUCKDB_GIT_VERSION` and `OVERRIDE_GIT_DESCRIBE` in `Makefile`.
   Keep the source-host build settings: staged prebuilt libraries have previously
   reported an alpha identity despite being published for a stable commit.
3. Update all `duckdb_version`, `ci_tools_version` and reusable workflow refs in
   [MainDistributionPipeline.yml](../.github/workflows/MainDistributionPipeline.yml).
   Update its host commit comment and the host assertion in
   [zz_build_guard.test](../test/sql/zz_build_guard.test).
4. Rebuild, run the offline and opt-in network tests, and check the host and
   loaded artifact identities as described in [test/README.md](../test/README.md).
5. Verify all five supported distribution targets in CI: Linux AMD64/ARM64,
   macOS AMD64/ARM64 and Windows AMD64 (MSVC). A local pass does not establish
   cross-platform readiness.

For API changes, use host compiler diagnostics, the relevant DuckDB header
history, and [DuckDB release notes](https://github.com/duckdb/duckdb/releases).
Update the README, cookbook, examples, runtime catalog descriptions and tests
together when a public contract changes.

## Publishing the extension

Tag pushes matching `v*` trigger the workflow's GitHub Release asset job after
the distribution build. A GitHub Release does not update community installation.

[packaging/description.yml](../packaging/description.yml) is a staging descriptor.
Before submitting it to `duckdb/community-extensions`, set `extension.version`
and `repo.ref` to the intended release and immutable commit, then ensure its
examples work against that commit. Submit it as `extensions/pdf/description.yml`.
Do not describe the 19-name API as community-published until the registry change
and its artifacts are available.
