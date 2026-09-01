#ifndef CCLEAN_FILTERS_HPP
#define CCLEAN_FILTERS_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace cclean {

namespace fs = std::filesystem;

// A number, optional fractional part, and one unit suffix: s, m, h, d, w.
// Truncated toward zero. Nothing else is accepted -- no sign, no exponent, no
// trailing character -- so "1dd" and "1d5d" are rejected rather than read as
// one day.
std::optional<std::chrono::seconds> parse_duration(const std::string& value);

// The same grammar over B, K/KiB, M/MiB, G/GiB, T/TiB, with a bare number
// meaning bytes. Fixed point throughout: going through double put the
// representable maximum out of reach and turned a value written at the
// boundary into a limit of zero.
std::optional<std::uintmax_t> parse_size(const std::string& value);

// now - limit is not safe to compute for a large --older-than: the filesystem
// clock counts nanoseconds in 64 bits, so subtracting a duration measured in
// centuries from it overflows. Both sides are reduced to whole seconds since
// the clock's own epoch instead, and the subtraction saturates.
bool is_older_than(
    fs::file_time_type when,
    fs::file_time_type now,
    std::chrono::seconds limit);

}  // namespace cclean

#endif  // CCLEAN_FILTERS_HPP
