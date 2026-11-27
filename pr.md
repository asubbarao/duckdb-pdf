# Skip the local PDF build guard in community CI

## Body

The build guard is intended to validate the PDF artifact built by this local checkout. Community extension CI builds against its own DuckDB (today v2.0.1, commit `8d78114e`), so the guard's pinned-host assertion (`v2.0.0`, source `d54cbbe3df`) fails there — community job 138968520845 failed on exactly that line.

The first attempt at a gate did not work: `require-env EXPECTED_EXT_VERSION` passes in community CI because this checkout's own `Makefile` exports it unconditionally, and community CI parses that same Makefile for `make test_release` (its test run carries this Makefile's `--fail-require-skip` flag and office-conversion prerequisites). An unconditional `PDF_LOCAL_BUILD_GUARD=1` export from the same Makefile would reach community CI the same way and skip nothing.

The Makefile now defines the pinned version once and checks every `duckdb_version` input in this repo's workflow against it while parsing. A mismatch is a hard Make error, so changing the workflow pin alone cannot skip the guard. The guard test reads its expected library version from the exported Makefile pin. The sentinel is exported only when `DUCKDB_VERSION` is unset (local runs) or matches the pin (this repo's CI). The community-extensions workflow skips the source consistency check and runs with `DUCKDB_VERSION=v2.0.1`, so the export is skipped and the guard file skips at `require-env PDF_LOCAL_BUILD_GUARD`.

## Evidence

- Community CI job 138968520845 (`build / linux_amd64`, run 45543778181): 68 passed, 1 failed — `test/sql/zz_build_guard.test:10`, `SELECT library_version = 'v2.0.0' AND starts_with(source_id, 'd54cbbe3df')` returned `0` instead of `true`. Its DuckDB is v2.0.1 / `8d78114e90a53823803660bf821f6073ac87da8c` (from the job's environment dump). [Run log](https://github.com/duckdb/community-extensions/actions/runs/45543778181/job/138968520845)
- `make -pn test_release` on this branch: `PDF_LOCAL_BUILD_GUARD = 1` is defined with `DUCKDB_VERSION` unset and with `DUCKDB_VERSION=v2.0.0`, and absent with `DUCKDB_VERSION=v2.0.1`. Make also fails during parsing if the workflow's pinned version differs from the Makefile pin.
- Skip/run behavior of the edited test file against a real test binary remains unverified locally (no built `unittest` binary in this worktree); this branch's own CI is the verification — it pins DuckDB v2.0.0, so the guard must run and pass there.

## Follow-up (not this PR)

Merging this does not by itself turn community-extensions #3788 green: that PR pins duckdb-pdf commit `03000e2` (0.10.1), which predates this fix. After merge, cut a 0.10.2 release and re-pin #3788's `extensions/pdf/description.yml` to the new commit.
