cmake_minimum_required(VERSION 3.16)

project(cclean
    VERSION 0.2.0
    DESCRIPTION "Removes build caches and editor debris from a directory tree"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release unless the caller asks otherwise. Without this a bare `cmake -S . -B
# build` produces an unoptimised binary, and the scan is the whole point.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

find_package(Threads REQUIRED)

# The library uses <dirent.h>, <fcntl.h> and <unistd.h> and the frontend uses
# <termios.h>, so these are the only compilers either can be built with anyway.
set(CCLEAN_WARNINGS -Wall -Wextra -pedantic)

# ---------------------------------------------------------------- library
#
# Finds, sizes and removes. It touches no terminal, prints nothing of its own,
# and reads no environment variable; a frontend supplies all of that.

add_library(cclean_core STATIC
    src/config.cpp
    src/defaults.cpp
    src/filters.cpp
    src/glob.cpp
    src/json.cpp
    src/project.cpp
    src/remove.cpp
    src/scan.cpp
    src/text.cpp
    src/toml.cpp
    src/version.cpp)

# Named cclean on disk; the target name differs only because the executable
# below has claimed it.
set_target_properties(cclean_core PROPERTIES OUTPUT_NAME cclean)

target_include_directories(cclean_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The version reaches the program from here, so project() above is the only
# place it is written. PRIVATE: only version.cpp reads it, and a consumer of
# the installed headers must not need the definition to compile.
target_compile_definitions(cclean_core
    PRIVATE CCLEAN_VERSION="${PROJECT_VERSION}")
target_compile_options(cclean_core PRIVATE ${CCLEAN_WARNINGS})
target_link_libraries(cclean_core PUBLIC Threads::Threads)

# --------------------------------------------------------------- frontend

add_executable(cclean
    cli/main.cpp
    cli/options.cpp
    cli/terminal.cpp)

target_compile_options(cclean PRIVATE ${CCLEAN_WARNINGS})
target_link_libraries(cclean PRIVATE cclean_core)

install(TARGETS cclean RUNTIME DESTINATION bin)
install(TARGETS cclean_core ARCHIVE DESTINATION lib)
install(DIRECTORY include/cclean DESTINATION include)

# ------------------------------------------------------------------ tests

option(CCLEAN_BUILD_TESTS "Build the test suites" ON)

if(CCLEAN_BUILD_TESTS)
    enable_testing()

    # The unit suite links the library and includes its public headers. It also
    # reaches two internal headers under src/, which are part of the library
    # but not of its installed API.
    add_executable(cclean_unit tests/unit.cpp)
    target_include_directories(cclean_unit
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(cclean_unit PRIVATE ${CCLEAN_WARNINGS})
    target_link_libraries(cclean_unit PRIVATE cclean_core)

    add_test(NAME unit COMMAND cclean_unit)

    # The command-line suite takes the binary to exercise as its argument, and
    # makes its own temporary directory, so it does not care about the cwd.
    add_test(
        NAME cli
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.sh $<TARGET_FILE:cclean>)
endif()
