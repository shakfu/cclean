#include "cclean/filters.hpp"

#include <limits>

namespace cclean {

namespace {

// Exact fixed-point conversion of a non-negative decimal, scaled by
// `multiplier` and truncated toward zero. Going through `double` and then
// range-checking against static_cast<double>(max) does not work: neither
// INT64_MAX nor UINTMAX_MAX is representable, so the bound rounds up to 2^63
// and 2^64 and a value written at the boundary passed the check only to reach
// an out-of-range floating-to-integer conversion, which is undefined. That
// turned --larger-than 18446744073709551615B into a limit of zero, which
// matched every file instead of none -- the opposite of what a filter used as
// a safety boundary before deletion is for.
bool parse_scaled(
    const std::string& text,
    std::uintmax_t multiplier,
    std::uintmax_t limit,
    std::uintmax_t& result)
{
    // Past nine fractional digits neither a byte count nor a second count can
    // still differ. The rest are still validated, then dropped.
    constexpr int max_fraction_digits = 9;

    std::size_t position = 0;
    std::uintmax_t whole = 0;
    bool any_digit = false;
    bool overflow = false;

    for (; position < text.size() &&
           text[position] >= '0' && text[position] <= '9'; ++position) {
        any_digit = true;
        const auto digit = static_cast<std::uintmax_t>(text[position] - '0');
        if (whole > (limit - digit) / 10) {
            overflow = true;
        } else {
            whole = whole * 10 + digit;
        }
    }

    std::uintmax_t fraction = 0;
    std::uintmax_t denominator = 1;

    if (position < text.size() && text[position] == '.') {
        ++position;
        int used = 0;
        for (; position < text.size() &&
               text[position] >= '0' && text[position] <= '9'; ++position) {
            any_digit = true;
            if (used < max_fraction_digits) {
                fraction = fraction * 10 +
                           static_cast<std::uintmax_t>(text[position] - '0');
                denominator *= 10;
                ++used;
            }
        }
    }

    // A bare ".", a sign, an exponent, or any trailing character is a
    // rejection: the units are the only suffix the grammar allows.
    if (!any_digit || position != text.size() || overflow || multiplier == 0) {
        return false;
    }

    if (whole > limit / multiplier) {
        return false;
    }

    const std::uintmax_t value = whole * multiplier;

    // fraction < denominator <= 1e9 and remainder < denominator, so the second
    // product stays under 1e18 and cannot overflow. The first is checked.
    const std::uintmax_t quotient = multiplier / denominator;
    const std::uintmax_t remainder = multiplier % denominator;

    if (quotient != 0 && fraction > limit / quotient) {
        return false;
    }

    std::uintmax_t extra = fraction * quotient;
    const std::uintmax_t rest = (fraction * remainder) / denominator;

    if (rest > limit - extra || extra + rest > limit - value) {
        return false;
    }

    result = value + extra + rest;
    return true;
}

}  // namespace

std::optional<std::chrono::seconds> parse_duration(const std::string& value) {
    if (value.size() < 2) {
        return std::nullopt;
    }

    std::uintmax_t multiplier = 0;

    switch (value.back()) {
    case 's': multiplier = 1; break;
    case 'm': multiplier = 60; break;
    case 'h': multiplier = 60 * 60; break;
    case 'd': multiplier = 24 * 60 * 60; break;
    case 'w': multiplier = 7 * 24 * 60 * 60; break;
    default: return std::nullopt;
    }

    // The unit has to be the only thing left, or "1dd" and "1d5d" both read as
    // one day. parse_scaled rejects the leftover letter for us.
    std::uintmax_t seconds = 0;
    const auto limit =
        static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max());

    if (!parse_scaled(value.substr(0, value.size() - 1),
                      multiplier, limit, seconds)) {
        return std::nullopt;
    }

    return std::chrono::seconds(static_cast<std::int64_t>(seconds));
}

std::optional<std::uintmax_t> parse_size(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::size_t split = 0;
    while (split < value.size() &&
           ((value[split] >= '0' && value[split] <= '9') ||
            value[split] == '.')) {
        ++split;
    }

    const std::string suffix = value.substr(split);
    std::uintmax_t multiplier = 1;

    if (suffix == "B" || suffix.empty()) multiplier = 1;
    else if (suffix == "K" || suffix == "KiB") multiplier = 1024;
    else if (suffix == "M" || suffix == "MiB") multiplier = 1024ULL * 1024;
    else if (suffix == "G" || suffix == "GiB") multiplier = 1024ULL * 1024 * 1024;
    else if (suffix == "T" || suffix == "TiB") multiplier = 1024ULL * 1024 * 1024 * 1024;
    else return std::nullopt;

    std::uintmax_t bytes = 0;

    if (!parse_scaled(value.substr(0, split), multiplier,
                      std::numeric_limits<std::uintmax_t>::max(), bytes)) {
        return std::nullopt;
    }

    return bytes;
}

bool is_older_than(
    fs::file_time_type when,
    fs::file_time_type now,
    std::chrono::seconds limit)
{
    const auto to_seconds = [](fs::file_time_type point) {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   point.time_since_epoch()).count();
    };

    const std::int64_t when_seconds = to_seconds(when);
    const std::int64_t now_seconds = to_seconds(now);
    const std::int64_t limit_seconds = limit.count();

    // The cutoff can only fall below the representable range when the clock
    // epoch is itself in the future, and then nothing is old enough.
    if (now_seconds < 0 &&
        limit_seconds >
            now_seconds - std::numeric_limits<std::int64_t>::min()) {
        return false;
    }

    return when_seconds <= now_seconds - limit_seconds;
}

}  // namespace cclean
