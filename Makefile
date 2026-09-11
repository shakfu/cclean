# A frontend over CMake. CMakeLists.txt is where the build is defined; these
# targets exist so that `make`, `make test` and `make install` keep working.
#
#   make                 configure and build into build/
#   make test            build, then run both suites through ctest
#   make install         install the binary, the library and the headers
#   make install-bin     install the binary alone, under ~/.local by default
#   make clean           remove build/
#
# Override the build directory or either install prefix on the command line:
#
#   make BUILD=out
#   make install PREFIX=$HOME/.local
#   make install-bin BIN_PREFIX=/usr/local

BUILD ?= build
PREFIX ?= /usr/local
BIN_PREFIX ?= $(HOME)/.local
BUILD_TYPE ?= Release

# CMake's Makefile generator decides what to recompile by comparing mtimes at
# one-second granularity. An edit that lands in the same second as the previous
# build is therefore never seen, and `make test` silently exercises the old
# binary: an edit-then-build loop missed 3 of 10 changes when measured. A
# checksum over the sources decides instead, and a mismatch cleans the tree
# before building. cksum is POSIX, so this needs nothing that CMake does not
# already require.
#
# The list is built by find inside the recipe rather than by $(shell), so that
# a source path holding a space stays one argument: `-exec cksum {} +` passes
# the names to cksum directly, where a word-split expansion had made two
# arguments of one path. Sorting the checksum lines rather than the names
# keeps the result independent of the order find walks the tree in, and needs
# neither `sort -z` nor `xargs -0`, which are not POSIX.
STAMP = $(BUILD)/.source-checksum

.PHONY: all build test install install-bin clean

all: build

# $(BUILD) and $(PREFIX) come from the command line and are quoted everywhere
# they are expanded: an unquoted path holding a space became two arguments and
# the cmake invocation failed with a usage message.
build:
	@cmake -S . -B "$(BUILD)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DCMAKE_INSTALL_PREFIX="$(PREFIX)"
	@checksum=$$(find include src cli tests CMakeLists.txt -type f \
		-exec cksum {} + 2>/dev/null | sort | cksum); \
	if [ ! -f "$(STAMP)" ] || [ "$$checksum" != "$$(cat "$(STAMP)")" ]; then \
		cmake --build "$(BUILD)" --target clean; \
	fi; \
	cmake --build "$(BUILD)" --parallel && \
	printf '%s\n' "$$checksum" > "$(STAMP)"

test: build
	@ctest --test-dir "$(BUILD)" --output-on-failure

install: build
	@cmake --install "$(BUILD)" --strip

# The runtime component is the executable; cclean_core and the headers are the
# development component. --prefix overrides the CMAKE_INSTALL_PREFIX the build
# was configured with, so this needs no second build directory.
install-bin: build
	@cmake --install "$(BUILD)" --prefix "$(BIN_PREFIX)" \
		--component runtime --strip

clean:
	@rm -rf "$(BUILD)"
