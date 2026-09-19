PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=pdf
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# The URL tests need httpfs, which is not linked into the test binary; the
# sqllogictest runner only loads it when autoloading is allowed. Without this
# they are skipped at `require httpfs` and never run at all.
export DUCKDB_TEST_AUTOLOADING=all

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Autoloading changes what `require pdf` means: instead of the extension linked
# into the test binary, the runner does INSTALL pdf FROM <local repo> and loads
# the result out of ~/.duckdb/extensions. INSTALL is a no-op when a copy is
# already there, so without this the suite silently tests whatever was built
# last time — a green run that never saw your changes. FORCE overwrites it.
FORCE_INSTALL_LOCAL = ./build/$(1)/duckdb -unsigned -c \
	"FORCE INSTALL $(EXT_NAME) FROM '$(PROJ_DIR)build/$(1)/repository';"

test_release_internal: force_install_release
test_debug_internal: force_install_debug
test_reldebug_internal: force_install_reldebug

force_install_release:
	@$(call FORCE_INSTALL_LOCAL,release)
force_install_debug:
	@$(call FORCE_INSTALL_LOCAL,debug)
force_install_reldebug:
	@$(call FORCE_INSTALL_LOCAL,reldebug)

.PHONY: force_install_release force_install_debug force_install_reldebug

# What `make test` must be testing: the version of the artifact just built.
# Compared against the loaded extension by test/sql/zz_build_guard.test, so a
# direct `unittest` invocation against a stale ~/.duckdb/extensions copy fails
# loudly instead of silently testing yesterday's build.
PDF_BUILT_VERSION = $(shell ./build/release/duckdb -unsigned -noheader -list -c \
	"LOAD '$(PROJ_DIR)build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension'; \
	 SELECT extension_version FROM duckdb_extensions() WHERE extension_name='$(EXT_NAME)';" 2>/dev/null | tail -1)
export EXPECTED_EXT_VERSION = $(PDF_BUILT_VERSION)
