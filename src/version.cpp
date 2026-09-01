#include "cclean/cclean.hpp"

// Set by CMake from project(... VERSION ...), so the number is written in one
// place. A build made outside CMake reports "unknown".
#ifndef CCLEAN_VERSION
#define CCLEAN_VERSION "unknown"
#endif

namespace cclean {

const char* version() {
    return CCLEAN_VERSION;
}

}  // namespace cclean
