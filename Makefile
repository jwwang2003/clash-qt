# Developer command facade over CMake presets, Go and CTest.
# Requires GNU Make 3.81 or newer. 3.81 is the floor because it is what macOS
# ships and Apple will not ship GPLv3; Linux and Windows both carry 4.x. Recipes
# are therefore single, &&-chained commands: 3.81 silently ignores .ONESHELL and
# has no .SHELLFLAGS, and Windows has no /bin/sh to fall back on. Portable work
# is delegated to CMake rather than reimplemented here.
#
# Run `make help` for the command list.

PRESET ?= dev

# Dependency prefix. A machine can easily have two copies of a dependency -- on
# this one, Anaconda ships a yaml-cpp that shadows Homebrew's and links against
# Qt from a different prefix, which fails at link time with undefined YAML
# symbols rather than at configure time. Presets stay free of local paths;
# resolving the prefix is the adapter layer's job.
ifeq ($(origin CMAKE_PREFIX_PATH), undefined)
CMAKE_PREFIX_PATH := $(shell brew --prefix 2>/dev/null)
endif
ifneq ($(strip $(CMAKE_PREFIX_PATH)),)
PREFIX_ARG := -DCMAKE_PREFIX_PATH=$(CMAKE_PREFIX_PATH)
endif
BUILD_DIR ?= build/$(PRESET)
JOBS ?= 8
CMAKE ?= cmake
CTEST ?= ctest
GIT ?= git

.DEFAULT_GOAL := help
.PHONY: help doctor setup configure core module build run test test-integration \
        test-native package clean

help:
	@echo "clash-qt developer commands.  Usage: make <command> [PRESET=dev|release|headless]"
	@echo ""
	@echo "  help              This list."
	@echo "  doctor            Read-only prerequisite and platform diagnostics."
	@echo "  setup             Initialise recorded submodules and declared dependencies."
	@echo "  configure         Configure the selected preset."
	@echo "  core              Build mihomo from 3rdparty/mihomo only."
	@echo "  module            Build the component and the engine artifacts it needs."
	@echo "  build             Build the application and all runtime dependencies."
	@echo "  run               Build if necessary and launch with the staged core."
	@echo "  test              Build and run portable feature/architecture/UI tests."
	@echo "  test-integration  Build the local engine and run real component/core workflows."
	@echo "  test-native       Native and privileged tests. Opt-in; needs a disposable host."
	@echo "  package           Build and stage a platform distribution."
	@echo "  clean             Remove generated output for PRESET. Never touches source,"
	@echo "                    user data or the submodule checkout."
	@echo ""
	@echo "Variables:  PRESET (default dev)   BUILD_DIR   JOBS (default 8)   CMAKE_PREFIX_PATH"
	@echo "Prerequisites: GNU Make >= 3.81, CMake >= 3.21, Ninja, Go, Git, Qt 6.9+, yaml-cpp."
	@echo "Windows: use GNU Make from an initialised MSVC/Qt environment, not NMake."
	@echo ""
	@echo "Not in this milestone: capture-addons, test-capture (delivered by P8/P9)."

doctor:
	@$(CMAKE) -E make_directory build && $(CMAKE) -S scripts/build/doctor -B build/doctor $(PREFIX_ARG) -DSOURCE_ROOT=$(CURDIR) -DMAKE_VERSION_REPORT="$(MAKE_VERSION)" > build/doctor.log 2>&1 || (cat build/doctor.log && exit 1) && grep -E "^-- " build/doctor.log

setup:
	@$(GIT) submodule update --init --recursive 3rdparty/mihomo && echo "setup: recorded submodules initialised."

configure:
	@$(CMAKE) --preset $(PRESET) -B "$(BUILD_DIR)" $(PREFIX_ARG)

$(BUILD_DIR)/CMakeCache.txt:
	@$(CMAKE) --preset $(PRESET) -B "$(BUILD_DIR)" $(PREFIX_ARG)

core: $(BUILD_DIR)/CMakeCache.txt
	@$(CMAKE) --build $(BUILD_DIR) --target clash-qt-core -j $(JOBS)

# The loadable supervisor and the locally built engine it owns.
module: $(BUILD_DIR)/CMakeCache.txt
	@$(CMAKE) --build $(BUILD_DIR) --target clash_qt_backend_module -j $(JOBS) && $(MAKE) core PRESET=$(PRESET)

build: $(BUILD_DIR)/CMakeCache.txt
	@$(CMAKE) --build $(BUILD_DIR) -j $(JOBS)

run: build
	@$(CMAKE) -E env CLASH_QT_CORE_BINARY=$(CURDIR)/$(BUILD_DIR)/core/mihomo $(BUILD_DIR)/clash-qt

test: build
	@$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure --no-tests=error --label-exclude "native|privileged|benchmark|real-core"

test-integration: build core
	@$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure --no-tests=error --label-regex "real-core|integration"

test-native: build
	@echo "test-native runs privileged and network tests that require an exclusive," && \
	 echo "disposable host. Set CLASH_QT_NATIVE_HOST=1 to confirm this is not your" && \
	 echo "working machine, then re-run." && \
	 test -n "$(CLASH_QT_NATIVE_HOST)" && \
	 $(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure --no-tests=error --label-regex "native|privileged"

# Depends on core as well as build: the engine target is deliberately not in ALL,
# so a package built without it would ship without a managed engine.
package: build core
	@$(CMAKE) --install $(BUILD_DIR) --prefix $(BUILD_DIR)/stage && echo "package: staged in $(BUILD_DIR)/stage"

clean:
	@$(CMAKE) -E rm -rf $(BUILD_DIR) && echo "clean: removed $(BUILD_DIR) (source, user data and submodules untouched)."
