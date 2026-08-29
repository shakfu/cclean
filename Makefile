# A frontend over CMake. CMakeLists.txt is where the build is defined; these
# targets exist so that `make`, `make test` and `make install` keep working.
#
#   make                 configure and build into build/
#   make test            build, then run both suites through ctest
#   make install         install to /usr/local by default
#   make clean           remove build/
#
# Override the build directory or the install prefix on the command line:
#
#   make BUILD=out
#   make install PREFIX=$HOME/.local

BUILD ?= build
PREFIX ?= /usr/local
BUILD_TYPE ?= Release

.PHONY: all build test install clean

all: build

build:
	@cmake -S . -B $(BUILD) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX)
	@cmake --build $(BUILD) --parallel

test: build
	@ctest --test-dir $(BUILD) --output-on-failure

install: build
	@cmake --install $(BUILD) --strip

clean:
	@rm -rf $(BUILD)
