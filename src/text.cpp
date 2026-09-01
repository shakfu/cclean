#include "cclean/text.hpp"

#include <cstdio>
#include <iterator>
#include <limits>

namespace cclean {

std::uintmax_t saturating_add(std::uintmax_t a, std::uintmax_t b) {
    if (b > std::numeric_limits<std::uintmax_t>::max() - a) {
        return std::numeric_limits<std::uintmax_t>::max();
    }

    return a + b;
}

std::string format_size(std::uintmax_t bytes) {
    static constexpr const char* units[] = {
        "B", "KiB", "MiB", "GiB", "TiB", "PiB"
    };

    double value = static_cast<double>(bytes);
    std::size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }

    char buffer[64];

    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu %s",
                      static_cast<unsigned long long>(bytes),
                      units[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
    }

    return buffer;
}

std::string display(const std::string& value) {
    std::string result;
    result.reserve(value.size());

    for (const unsigned char c : value) {
        if (c == '\\') {
            result += "\\\\";
        } else if (c < 0x20 || c == 0x7f) {
            char escaped[5];
            std::snprintf(escaped, sizeof(escaped), "\\x%02x", c);
            result += escaped;
        } else {
            result += static_cast<char>(c);
        }
    }

    return result;
}

std::string display(const fs::path& path) {
    return display(path.string());
}

namespace {

// Length of the UTF-8 sequence starting at `value[position]`, or 0 when the
// bytes there are not a well-formed, shortest-form, in-range sequence.
std::size_t utf8_sequence_length(const std::string& value,
                                 std::size_t position) {
    const auto byte = [&](std::size_t offset) {
        return static_cast<unsigned char>(value[position + offset]);
    };

    const unsigned char lead = byte(0);
    std::size_t length = 0;

    if (lead < 0x80) return 1;
    else if (lead >= 0xc2 && lead <= 0xdf) length = 2;
    else if (lead >= 0xe0 && lead <= 0xef) length = 3;
    else if (lead >= 0xf0 && lead <= 0xf4) length = 4;
    else return 0;

    if (position + length > value.size()) {
        return 0;
    }

    for (std::size_t i = 1; i < length; ++i) {
        if (byte(i) < 0x80 || byte(i) > 0xbf) {
            return 0;
        }
    }

    // Overlong forms and the surrogate and out-of-range blocks share a lead
    // byte with valid sequences, so they are separated on the second byte.
    if (lead == 0xe0 && byte(1) < 0xa0) return 0;
    if (lead == 0xed && byte(1) > 0x9f) return 0;
    if (lead == 0xf0 && byte(1) < 0x90) return 0;
    if (lead == 0xf4 && byte(1) > 0x8f) return 0;

    return length;
}

}  // namespace

std::string json_string(const std::string& value) {
    std::string result = "\"";
    std::size_t position = 0;

    while (position < value.size()) {
        const auto c = static_cast<unsigned char>(value[position]);

        if (c < 0x80) {
            switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char escaped[7];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    result += escaped;
                } else {
                    result += static_cast<char>(c);
                }
            }
            ++position;
            continue;
        }

        const std::size_t length = utf8_sequence_length(value, position);

        if (length == 0) {
            result += "\xef\xbf\xbd";  // U+FFFD REPLACEMENT CHARACTER
            ++position;
        } else {
            result.append(value, position, length);
            position += length;
        }
    }

    result += '"';
    return result;
}

}  // namespace cclean
