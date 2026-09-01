#ifndef CCLEAN_CCLEAN_HPP
#define CCLEAN_CCLEAN_HPP

// Umbrella header. The library finds and removes build caches and editor
// debris; it does nothing with a terminal, prints nothing of its own, and
// reads no environment variable. A frontend supplies the argument parsing,
// the confirmation, and the human-readable output.
//
// The usual sequence:
//
//   Config config;                       // optional: find_config/load_config
//   ScanOptions options;
//   options.patterns = compile_patterns(true, config.patterns, {});
//   ScanResult result = scan(root, options);
//   for (const Target& t : result.targets) { ... remove_target(t, error); }

#include "cclean/config.hpp"
#include "cclean/defaults.hpp"
#include "cclean/filters.hpp"
#include "cclean/glob.hpp"
#include "cclean/json.hpp"
#include "cclean/project.hpp"
#include "cclean/remove.hpp"
#include "cclean/scan.hpp"
#include "cclean/target.hpp"
#include "cclean/text.hpp"

namespace cclean {

// The version this library was built with, or "unknown" outside CMake.
const char* version();

}  // namespace cclean

#endif  // CCLEAN_CCLEAN_HPP
