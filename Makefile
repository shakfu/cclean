# A frontend over CMake. CMakeLists.txt is where the build is defined; these
# targets exist so that `make`, `make test` and `make install` keep working.
#
#   make                 configure and build into build/
#   make test            build, then run both suites through ctest
#   make install         install the binary, the library and the headers
#   make clean           remove build/
#
# Override the build directory or the install prefix on the command line:
#
#   make BUILD=out
#   make install PREFIX=$HOME/.local

BUILD ?= build
PREFIX ?= /usr/local
BUILD_TYPE ?= Release

# CMake's Makefile generator decides what to recompile by comparing mtimes at
# one-second granularity. An edit that lands in the same second as the previous
# build is therefore never seen, and `make test` silently exercises the old
# binary: an edit-then-build loop missed 3 of 10 changes when measured. A
# checksum over the sources decides instead, and a mismatch cleans the tree
# before building. cksum is POSIX, so this needs nothing that CMake does not
# already require.
SOURCES := $(shell find include src cli tests -type f 2>/dev/null | sort) \
           CMakeLists.txt
STAMP = $(BUILD)/.source-checksum

.PHONY: all build test install clean

all: build

build:
	@cmake -S . -B $(BUILD) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX)
	@checksum=$$(cksum $(SOURCES) | cksum); \
	if [ ! -f $(STAMP) ] || [ "$$checksum" != "$$(cat $(STAMP))" ]; then \
		cmake --build $(BUILD) --target clean; \
	fi; \
	cmake --build $(BUILD) --parallel && \
	printf '%s\n' "$$checksum" > $(STAMP)

test: build
	@ctest --test-dir $(BUILD) --output-on-failure

install: build
	@cmake --install $(BUILD) --strip

clean:
	@rm -rf $(BUILD)
