#include "cclean/glob.hpp"

#include <algorithm>

namespace cclean {

Glob::Glob(const std::string& pattern, Scope scope, Reason reason)
    : scope_(scope), reason_(reason)
{
    parse(pattern);
    classify();
}

void Glob::parse(const std::string& glob) {
    tokens_.reserve(glob.size());

    for (std::size_t i = 0; i < glob.size();) {
        const char c = glob[i];

        // "**/" matches zero or more directory components.
        if (c == '*' &&
            i + 2 < glob.size() &&
            glob[i + 1] == '*' &&
            glob[i + 2] == '/') {
            tokens_.push_back({Token::Type::AnyDirs, '\0'});
            i += 3;
            continue;
        }

        if (c == '*') {
            // Collapse runs of '*'; "a**b" and "a*b" accept the same strings,
            // and one token spares the matcher a split point.
            if (tokens_.empty() ||
                tokens_.back().type != Token::Type::AnySequence) {
                tokens_.push_back({Token::Type::AnySequence, '\0'});
            }
        } else if (c == '?') {
            tokens_.push_back({Token::Type::AnyOne, '\0'});
        } else {
            tokens_.push_back({Token::Type::Character, c});
        }

        ++i;
    }
}

// Detects the pattern shapes that a string compare can answer.
void Glob::classify() {
    const auto is_character = [](const Token& t) {
        return t.type == Token::Type::Character;
    };
    const auto is_sequence = [](const Token& t) {
        return t.type == Token::Type::AnySequence;
    };
    const auto collect = [this](std::size_t from, std::size_t to) {
        text_.clear();
        for (std::size_t i = from; i < to; ++i) {
            text_ += tokens_[i].character;
        }
    };

    const std::size_t n = tokens_.size();

    if (std::all_of(tokens_.begin(), tokens_.end(), is_character)) {
        kind_ = Kind::Literal;
        collect(0, n);
        return;
    }

    // "*text": every character is fixed once the leading '*' is consumed.
    if (is_sequence(tokens_.front()) &&
        std::all_of(tokens_.begin() + 1, tokens_.end(), is_character)) {
        kind_ = Kind::Suffix;
        collect(1, n);
        return;
    }

    // "text*", including the "text**" that a "dir/**" pattern parses to.
    if (is_sequence(tokens_.back()) &&
        std::all_of(tokens_.begin(), tokens_.end() - 1, is_character)) {
        kind_ = Kind::Prefix;
        collect(0, n - 1);
        return;
    }

    kind_ = Kind::General;
}

bool Glob::matches(std::string_view value) const {
    switch (kind_) {
    case Kind::Literal:
        return value == text_;
    case Kind::Prefix:
        return value.size() >= text_.size() &&
               value.compare(0, text_.size(), text_) == 0;
    case Kind::Suffix:
        return value.size() >= text_.size() &&
               value.compare(value.size() - text_.size(),
                             text_.size(), text_) == 0;
    case Kind::General:
        break;
    }

    return match_tokens(value);
}

namespace {

void set_state(std::vector<std::uint64_t>& states, std::size_t index) {
    states[index / 64] |= std::uint64_t{1} << (index % 64);
}

bool test_state(const std::vector<std::uint64_t>& states, std::size_t index) {
    return ((states[index / 64] >> (index % 64)) & 1) != 0;
}

}  // namespace

// Adds a token position and everything reachable from it without consuming
// input: '*' and "**/" may both stand for the empty string.
void Glob::add_closure(
    std::vector<std::uint64_t>& states,
    std::size_t index) const
{
    while (true) {
        set_state(states, index);

        if (index >= tokens_.size()) {
            return;
        }

        const Token::Type type = tokens_[index].type;

        if (type != Token::Type::AnySequence &&
            type != Token::Type::AnyDirs) {
            return;
        }

        ++index;
    }
}

// Advances every live token position one character at a time, which bounds the
// work at tokens * length. Backtracking instead is what makes a pattern like
// "*a*a*a*a*b" take exponential time on a long name.
bool Glob::match_tokens(std::string_view value) const {
    const std::size_t count = tokens_.size();
    const std::size_t words = count / 64 + 1;

    std::vector<std::uint64_t> current(words, 0);
    std::vector<std::uint64_t> next(words, 0);

    add_closure(current, 0);

    for (const char c : value) {
        std::fill(next.begin(), next.end(), 0);
        bool live = false;

        for (std::size_t i = 0; i < count; ++i) {
            if (!test_state(current, i)) {
                continue;
            }

            switch (tokens_[i].type) {
            case Token::Type::Character:
                if (tokens_[i].character == c) {
                    add_closure(next, i + 1);
                    live = true;
                }
                break;

            case Token::Type::AnyOne:
                add_closure(next, i + 1);
                live = true;
                break;

            case Token::Type::AnySequence:
                // '*' spans any character, '/' included.
                add_closure(next, i);
                live = true;
                break;

            case Token::Type::AnyDirs:
                // Only a '/' can close the directory run, so advancing past it
                // is not an option on any other character.
                set_state(next, i);
                live = true;

                if (c == '/') {
                    add_closure(next, i + 1);
                }
                break;
            }
        }

        if (!live) {
            return false;
        }

        current.swap(next);
    }

    return test_state(current, count);
}

const Glob* first_match(
    const fs::path& path,
    const fs::path& root,
    const std::string& filename,
    const std::vector<Glob>& patterns)
{
    std::string relative;
    bool relative_built = false;

    for (const Glob& pattern : patterns) {
        if (pattern.matches(filename)) {
            return &pattern;
        }

        if (pattern.scope() == Glob::Scope::NameOnly) {
            continue;
        }

        if (!relative_built) {
            relative = path.lexically_relative(root).generic_string();
            relative_built = true;
        }

        if (pattern.matches(relative)) {
            return &pattern;
        }
    }

    return nullptr;
}

bool matches_any(
    const fs::path& path,
    const fs::path& root,
    const std::string& filename,
    const std::vector<Glob>& patterns)
{
    return first_match(path, root, filename, patterns) != nullptr;
}

}  // namespace cclean
