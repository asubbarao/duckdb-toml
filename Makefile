PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=toml
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Autoloading changes what `require <ext>` means: instead of the extension linked
# into the test binary, the runner does INSTALL FROM <local repo> and loads out of
# ~/.duckdb/extensions. INSTALL is a no-op when a copy is already there, so without
# this the suite silently tests whatever was built last time. FORCE overwrites it.
# -init /dev/null: a user's ~/.duckdbrc may load extensions this fresh build cannot.
FORCE_INSTALL_LOCAL = mkdir -p $(DUCKDB_EXT_HOME) && ./build/$(1)/duckdb -init /dev/null -unsigned -c \
	"FORCE INSTALL $(EXT_NAME) FROM '$(PROJ_DIR)build/$(1)/repository';"

# ~/.duckdb/extensions is shared by every checkout on the machine, so a sibling
# worktree's FORCE INSTALL can land under the build this suite then loads.
DUCKDB_EXT_HOME = $(PROJ_DIR)build/extension_home

test_release: force_install_release
test_debug: force_install_debug
test_reldebug: force_install_reldebug

force_install_release force_install_debug force_install_reldebug: export HOME := $(DUCKDB_EXT_HOME)
test_release test_debug test_reldebug: export HOME := $(DUCKDB_EXT_HOME)

force_install_release:
	@$(call FORCE_INSTALL_LOCAL,release)
force_install_debug:
	@$(call FORCE_INSTALL_LOCAL,debug)
force_install_reldebug:
	@$(call FORCE_INSTALL_LOCAL,reldebug)

# What `make test` must be testing: the version string of the artifact just built.
TOML_BUILT_VERSION = $(shell ./build/release/duckdb -init /dev/null -unsigned -noheader -list -c \
	"LOAD '$(PROJ_DIR)build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension'; \
	 SELECT extension_version FROM duckdb_extensions() WHERE extension_name='$(EXT_NAME)';" 2>/dev/null | tail -1)
export EXPECTED_EXT_VERSION = $(TOML_BUILT_VERSION)

.PHONY: force_install_release force_install_debug force_install_reldebug
