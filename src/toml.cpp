#include "toml.hpp"

namespace cclean {
namespace toml {

namespace {

void skip_blanks(const std::string& text, std::size_t& position) {
    while (position < text.size() &&
           (text[position] == ' ' || text[position] == '\t')) {
        ++position;
    }
}

// One comma between elements, no more and none before the first. TOML has no
// empty array element, and skipping every comma before the next value made
// [, "*.tmp"] and ["*.a",,, "*.b"] both parse: a typo in a committed config
// silently changed what a run would delete instead of being rejected. A single
// trailing comma is allowed, as TOML allows it.
bool parse_separator(
    const std::string& body,
    std::size_t& position,
    bool& more)
{
    skip_blanks(body, position);

    if (position == body.size()) {
        more = false;
        return true;
    }

    if (body[position] != ',') {
        return false;
    }

    ++position;
    skip_blanks(body, position);
    more = position != body.size();
    return true;
}

bool parse_quoted(
    const std::string& body,
    std::size_t& position,
    std::string& item)
{
    if (position >= body.size() || body[position] != '"') {
        return false;
    }

    ++position;
    item.clear();

    while (position < body.size()) {
        const char c = body[position++];

        if (c == '"') {
            return true;
        }

        if (c == '\\' || c == '\n' || c == '\r') {
            return false;
        }

        item += c;
    }

    return false;
}

// Index of the ']' closing the '[' at `position`, or npos. Scanning for the
// next ']' textually instead rejected a perfectly ordinary POSIX marker name
// containing that character, because the search did not know it was inside a
// string. Quotes and nesting are both tracked here; the string bodies
// themselves are still parsed by parse_quoted.
std::size_t find_array_end(const std::string& body, std::size_t position) {
    std::size_t depth = 0;
    bool quoted = false;

    for (; position < body.size(); ++position) {
        const char c = body[position];

        if (quoted) {
            if (c == '\\') {
                // Escapes are rejected by parse_quoted, so a backslash cannot
                // be hiding a quote; treating it as ordinary keeps the two
                // scanners agreeing on where the string ends.
                continue;
            }
            if (c == '"') {
                quoted = false;
            }
            continue;
        }

        if (c == '"') {
            quoted = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            if (--depth == 0) {
                return position;
            }
        }
    }

    return std::string::npos;
}

}  // namespace

std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_bool(const std::string& value, bool& result) {
    if (value == "true") {
        result = true;
        return true;
    }
    if (value == "false") {
        result = false;
        return true;
    }
    return false;
}

bool parse_string(const std::string& value, std::string& result) {
    const std::string text = trim(value);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return false;
    }
    result = text.substr(1, text.size() - 2);
    return result.find('\\') == std::string::npos &&
           result.find('\n') == std::string::npos &&
           result.find('\r') == std::string::npos;
}

bool parse_string_array(
    const std::string& value,
    std::vector<std::string>& result)
{
    const std::string text = trim(value);

    if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
        return false;
    }

    const std::string body = text.substr(1, text.size() - 2);
    std::size_t position = 0;

    skip_blanks(body, position);
    bool more = position != body.size();

    while (more) {
        std::string item;

        if (!parse_quoted(body, position, item)) {
            return false;
        }

        result.push_back(std::move(item));

        if (!parse_separator(body, position, more)) {
            return false;
        }
    }

    return true;
}

bool parse_pair_array(
    const std::string& value,
    std::vector<std::pair<std::string, std::string>>& result)
{
    const std::string text = trim(value);

    if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
        return false;
    }

    const std::string body = text.substr(1, text.size() - 2);
    std::size_t position = 0;

    skip_blanks(body, position);
    bool more = position != body.size();

    while (more) {
        if (body[position] != '[') {
            return false;
        }

        const std::size_t close = find_array_end(body, position);

        if (close == std::string::npos) {
            return false;
        }

        std::vector<std::string> pair;

        if (!parse_string_array(body.substr(position, close - position + 1),
                                pair) ||
            pair.size() != 2) {
            return false;
        }

        // The marker is looked up beside the directory, so it has to be a
        // plain file name. A separator would reach outside that directory.
        for (const std::string& part : pair) {
            if (part.empty() || part == "." || part == ".." ||
                part.find('/') != std::string::npos) {
                return false;
            }
        }

        result.emplace_back(pair[0], pair[1]);
        position = close + 1;

        if (!parse_separator(body, position, more)) {
            return false;
        }
    }

    return true;
}

}  // namespace toml
}  // namespace cclean
