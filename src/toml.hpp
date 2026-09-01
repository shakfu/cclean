#ifndef CCLEAN_SRC_TOML_HPP
#define CCLEAN_SRC_TOML_HPP

// Internal to the library. Not installed, and not part of the public API.
//
// Enough of TOML for .cclean.toml: top-level `key = value` lines, string
// arrays, arrays of two-string arrays, quoted strings and booleans. No tables,
// no escapes, no multi-line values. Anything the grammar does not accept is a
// rejection rather than a best-effort reading, because the values decide what
// a run deletes.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace cclean {
namespace toml {

std::string trim(const std::string& value);

bool parse_bool(const std::string& value, bool& result);

// A quoted scalar. Backslashes and newlines are rejected rather than
// interpreted, which is what lets the comment stripper treat quotes as a
// plain toggle.
bool parse_string(const std::string& value, std::string& result);

bool parse_string_array(
    const std::string& value,
    std::vector<std::string>& result);

// [["deps", "mix.exs"], ["vendor", "composer.lock"]]. Each element must be a
// plain name: a marker is looked up beside its directory, so a separator would
// reach outside it.
bool parse_pair_array(
    const std::string& value,
    std::vector<std::pair<std::string, std::string>>& result);

}  // namespace toml
}  // namespace cclean

#endif  // CCLEAN_SRC_TOML_HPP
