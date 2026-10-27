PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=pdf
EXT_CONFIG=${PROJ_DIR}extension_config.cmake
DUCKDB_GIT_VERSION ?= v2.0.0
OVERRIDE_GIT_DESCRIBE ?= v2.0.0

# Staging archives can report an alpha version even for the release commit.
# Build the pinned stable host so extension repository paths and ABI agree.
DUCKDB_PREBUILT_LIBRARY :=
DUCKDB_PREBUILT_EXTENSIONS :=
EXT_FLAGS += -DPREBUILT_BINARY= -DPREBUILT_EXTENSIONS=

# The URL tests need httpfs, which is not linked into the test binary; the
# sqllogictest runner only loads it when autoloading is allowed. Without this
# they are skipped at `require httpfs` and never run at all.
export DUCKDB_TEST_AUTOLOADING=all

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# `make test` runs DuckDB's scripts/ci/run_tests.py. By default it reports a file
# skipped at `require pdf` as a pass, so a build whose test binary cannot load
# the extension goes green having run nothing (every CI job did, from the move to
# DuckDB 2.0 until this was added). --fail-require-skip fails the batch instead.
# Skips at `require-env` (opt-in network and LibreOffice tests) still pass.
ifneq ($(TEST_RUNNER),)
TEST_RUNNER += --fail-require-skip
endif

# The network tests need httpfs, which this tree does not build. In DuckDB 2.0
# the runner gives every test file a fresh HOME and autoinstalls from the build's
# own repository (build/<type>/repository/<version>/<platform>), so an httpfs
# installed into ~/.duckdb is never seen. Install the published one straight into
# that repository instead. Only when the network tests are on, and only where the
# tests actually run (not the outside-docker pass on Linux, which skips them).
ifneq ($(PDF_NETWORK_TESTS),)
ifneq ($(SKIP_TESTS),1)
test_release: httpfs_into_repository_release
test_debug: httpfs_into_repository_debug
test_reldebug: httpfs_into_repository_reldebug
endif
endif

INSTALL_HTTPFS_INTO_REPOSITORY = ./build/$(1)/duckdb -no-agent -c \
	"SET extension_directory = '$(PROJ_DIR)build/$(1)/repository'; INSTALL httpfs; INSTALL quack;"

httpfs_into_repository_release:
	$(call INSTALL_HTTPFS_INTO_REPOSITORY,release)
httpfs_into_repository_debug:
	$(call INSTALL_HTTPFS_INTO_REPOSITORY,debug)
httpfs_into_repository_reldebug:
	$(call INSTALL_HTTPFS_INTO_REPOSITORY,reldebug)

.PHONY: httpfs_into_repository_release httpfs_into_repository_debug httpfs_into_repository_reldebug

# What `make test` must be testing: the version of the artifact just built.
# Compared against the loaded extension by test/sql/zz_build_guard.test, so a
# test binary that loads some other copy of the extension fails loudly instead
# of silently testing yesterday's build.
PDF_BUILT_VERSION = $(shell ./build/release/duckdb -unsigned -no-agent -noheader -list -c \
	"LOAD '$(PROJ_DIR)build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension'; \
	 SELECT extension_version FROM duckdb_extensions() WHERE extension_name='$(EXT_NAME)';" 2>/dev/null | tail -1)
export EXPECTED_EXT_VERSION = $(PDF_BUILT_VERSION)

# Exercise format routing and backend failure behavior even without desktop apps.
define OFFICE_CONVERSION_TEST
test_$(1)_internal: office_conversion_test_$(1)
office_conversion_test_$(1):
	cmake --build build/$(1) --target pdf_office_conversion_test
	./build/$(1)/extension/pdf/pdf_office_conversion_test
.PHONY: office_conversion_test_$(1)
endef
$(foreach config,release debug reldebug,$(eval $(call OFFICE_CONVERSION_TEST,$(config))))
