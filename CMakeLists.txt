cmake_minimum_required(VERSION 3.16)

project(cclean
    VERSION 0.1.0
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

# The program uses <termios.h> and <unistd.h>, so these are the only compilers
# it can be built with anyway.
set(CCLEAN_WARNINGS -Wall -Wextra -pedantic)

add_executable(cclean src/cclean.cpp)
target_compile_options(cclean PRIVATE ${CCLEAN_WARNINGS})
target_link_libraries(cclean PRIVATE Threads::Threads)

install(TARGETS cclean RUNTIME DESTINATION bin)

option(CCLEAN_BUILD_TESTS "Build the test suites" ON)

if(CCLEAN_BUILD_TESTS)
    enable_testing()

    # unit.cpp includes src/cclean.cpp directly and renames its entry point,
    # which is how it reaches the static functions. It therefore reaches only
    # part of the file, hence -Wno-unused-function.
    add_executable(cclean_unit tests/unit.cpp)
    target_compile_options(cclean_unit PRIVATE
        ${CCLEAN_WARNINGS} -Wno-unused-function)
    target_link_libraries(cclean_unit PRIVATE Threads::Threads)

    add_test(NAME unit COMMAND cclean_unit)

    # The command-line suite takes the binary to exercise as its argument, and
    # makes its own temporary directory, so it does not care about the cwd.
    add_test(
        NAME cli
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.sh $<TARGET_FILE:cclean>)
endif()
