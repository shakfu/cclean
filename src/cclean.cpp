#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <termios.h>
#include <unistd.h>

#ifndef CCLEAN_VERSION
#define CCLEAN_VERSION "unknown"
#endif

namespace fs = std::filesystem;

namespace defaults {

// Built-in patterns, enabled unless --no-defaults is given. ".*_cache" covers
// the per-tool caches by shape, so .pytest_cache, .mypy_cache and .ruff_cache
// need no entries of their own; narrowing it would drop them.
constexpr std::array<std::string_view, 5> patterns = {
    "__pycache__",
    "*.pyc",
    "*.pyo",
    ".*_cache",
    ".DS_Store"
};

// Build output that --build-artifacts may remove. A directory qualifies when
// its name appears here and the paired marker file sits beside it, inside a
// project marked by .git. Several ecosystems build into the same directory
// name, so one name carries several markers.
struct Artifact {
    std::string_view directory;
    std::string_view marker;
};

constexpr std::array<Artifact, 25> artifacts = {{
    // C and C++
    {"build", "CMakeLists.txt"},
    {"build", "meson.build"},

    // Rust
    {"target", "Cargo.toml"},

    // JavaScript and TypeScript
    {"build", "package.json"},
    {"dist", "package.json"},
    {".next", "package.json"},
    {".nuxt", "package.json"},
    {".svelte-kit", "package.json"},
    {".turbo", "package.json"},
    {".parcel-cache", "package.json"},

    // JVM
    {"target", "pom.xml"},
    {"build", "build.gradle"},
    {"build", "build.gradle.kts"},
    {".gradle", "build.gradle"},
    {".gradle", "build.gradle.kts"},

    // Python
    {"build", "pyproject.toml"},
    {"dist", "pyproject.toml"},
    {"build", "setup.py"},
    {"dist", "setup.py"},

    // Zig
    {"zig-out", "build.zig"},
    {"zig-cache", "build.zig"},
    {".zig-cache", "build.zig"},

    // Swift
    {".build", "Package.swift"},

    // Elixir
    {"_build", "mix.exs"},

    // Dart and Flutter
    {"build", "pubspec.yaml"}
}};

// Three ecosystems, not every ecosystem. A wrong entry here deletes a tree that
// cannot be rebuilt, so the list stays where the restore command is known and
// the layout is conventional. Anything else is a pattern in .cclean.toml.
constexpr std::array<Artifact, 8> dependencies = {{
    // uv.lock rather than pyproject.toml: the lock file is what makes the
    // environment reproducible, and `uv sync` rebuilds it exactly. A bare
    // pyproject.toml is also satisfied by poetry, pdm, hatch and by a .venv
    // populated with pip, none of which restore to a known state.
    {".venv", "uv.lock"},
    // A lock file rather than package.json, which carries ranges: only the lock
    // pins a tree. One row per package manager, because they share the install
    // directory but not the lock name, and a name match on node_modules alone
    // would take trees no lock can rebuild.
    {"node_modules", "package-lock.json"},
    {"node_modules", "npm-shrinkwrap.json"},
    {"node_modules", "yarn.lock"},
    {"node_modules", "pnpm-lock.yaml"},
    {"node_modules", "bun.lock"},
    {"node_modules", "bun.lockb"},
    // go.mod pins versions on its own: minimal version selection is
    // deterministic, so it is the lock file as well as the manifest.
    {"vendor", "go.mod"},
}};

// Never matched and never descended into. Their contents are state managed
// by another tool, where a name match is far likelier to be a false positive
// than a cache, and a wrong deletion costs history, credentials, or a working
// environment. --no-skip walks them anyway.
constexpr std::array<std::string_view, 6> skipped = {
    ".git",
    ".hg",
    ".svn",
    ".config",
    ".ssh",
    ".gnupg"
};

}  // namespace defaults

struct Target {
    fs::path path;
    std::uintmax_t size = 0;
    bool is_directory = false;
    bool is_symlink = false;
    std::string reason;
    fs::file_time_type newest_time{};
    bool has_time = false;
};

struct Config {
    std::vector<std::string> patterns;
    std::vector<std::string> excludes;
    bool defaults = true;
    bool build_artifacts = false;
    bool dependencies = false;
    bool skip_protected = true;
    std::vector<std::pair<std::string, std::string>> dependency_markers;
    std::vector<std::string> project_roots;
    std::string older_than;
    std::string larger_than;
};

static std::string trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool parse_bool(const std::string& value, bool& result) {
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

static bool parse_string_array(
    const std::string& value,
    std::vector<std::string>& result)
{
    const std::string text = trim(value);

    if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
        return false;
    }

    const std::string body = text.substr(1, text.size() - 2);
    std::size_t position = 0;

    while (position < body.size()) {
        while (position < body.size() &&
               (body[position] == ' ' || body[position] == '\t' ||
                body[position] == ',')) {
            ++position;
        }

        if (position == body.size()) {
            break;
        }

        if (body[position] != '"') {
            return false;
        }
        ++position;

        std::string item;
        bool closed = false;
        while (position < body.size()) {
            const char c = body[position++];
            if (c == '"') {
                closed = true;
                break;
            }
            if (c == '\\' || c == '\n' || c == '\r') {
                return false;
            }
            item += c;
        }

        if (!closed) {
            return false;
        }

        result.push_back(std::move(item));

        while (position < body.size() &&
               (body[position] == ' ' || body[position] == '\t')) {
            ++position;
        }
        if (position < body.size() && body[position] != ',') {
            return false;
        }
    }

    return true;
}

// [["deps", "mix.exs"], ["vendor", "composer.lock"]]. The inner arrays go
// through parse_string_array, so quoting and escaping stay defined in one
// place; only the outer nesting is walked here.
static bool parse_pair_array(
    const std::string& value,
    std::vector<std::pair<std::string, std::string>>& result)
{
    const std::string text = trim(value);

    if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
        return false;
    }

    const std::string body = text.substr(1, text.size() - 2);
    std::size_t position = 0;

    while (position < body.size()) {
        while (position < body.size() &&
               (body[position] == ' ' || body[position] == '\t' ||
                body[position] == ',')) {
            ++position;
        }

        if (position == body.size()) {
            break;
        }

        if (body[position] != '[') {
            return false;
        }

        const std::size_t close = body.find(']', position);
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

        while (position < body.size() &&
               (body[position] == ' ' || body[position] == '\t')) {
            ++position;
        }
        if (position < body.size() && body[position] != ',') {
            return false;
        }
    }

    return true;
}

static bool parse_string(const std::string& value, std::string& result) {
    const std::string text = trim(value);
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return false;
    }
    result = text.substr(1, text.size() - 2);
    return result.find('\\') == std::string::npos &&
           result.find('\n') == std::string::npos &&
           result.find('\r') == std::string::npos;
}

static bool load_config(
    const fs::path& path,
    Config& config,
    std::string& error)
{
    std::ifstream input(path);
    if (!input) {
        error = "Cannot read config file: " + path.string();
        return false;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        // Only a # outside quotes opens a comment. Truncating at the first one
        // anywhere rejected patterns like "#*#", which name real files: emacs
        // lock and autosave entries are the common case. Quotes toggle rather
        // than nest, which is exact here because the string parsers below
        // reject backslashes.
        bool quoted = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') {
                quoted = !quoted;
            } else if (line[i] == '#' && !quoted) {
                line.erase(i);
                break;
            }
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            error = path.string() + ":" + std::to_string(line_number) +
                    ": expected key = value";
            return false;
        }

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));

        if (key == "patterns") {
            if (!parse_string_array(value, config.patterns)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": patterns must be an array of strings";
                return false;
            }
        } else if (key == "excludes") {
            if (!parse_string_array(value, config.excludes)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": excludes must be an array of strings";
                return false;
            }
        } else if (key == "defaults") {
            if (!parse_bool(value, config.defaults)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": defaults must be true or false";
                return false;
            }
        } else if (key == "build_artifacts") {
            if (!parse_bool(value, config.build_artifacts)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": build_artifacts must be true or false";
                return false;
            }
        } else if (key == "dependencies") {
            if (!parse_bool(value, config.dependencies)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": dependencies must be true or false";
                return false;
            }
        } else if (key == "skip_protected") {
            if (!parse_bool(value, config.skip_protected)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": skip_protected must be true or false";
                return false;
            }
        } else if (key == "dependency_markers") {
            if (!parse_pair_array(value, config.dependency_markers)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": dependency_markers must be an array of "
                        "[directory, marker] string pairs, each a plain name";
                return false;
            }
        } else if (key == "project_roots") {
            if (!parse_string_array(value, config.project_roots)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": project_roots must be an array of strings";
                return false;
            }
        } else if (key == "older_than") {
            if (!parse_string(value, config.older_than)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": older_than must be a quoted duration";
                return false;
            }
        } else if (key == "larger_than") {
            if (!parse_string(value, config.larger_than)) {
                error = path.string() + ":" + std::to_string(line_number) +
                        ": larger_than must be a quoted size";
                return false;
            }
        } else {
            error = path.string() + ":" + std::to_string(line_number) +
                    ": unknown key " + key;
            return false;
        }
    }

    return true;
}

static fs::path find_config(const fs::path& root) {
    fs::path current = fs::absolute(root).lexically_normal();
    if (!fs::is_directory(current)) {
        current = current.parent_path();
    }

    while (!current.empty()) {
        const fs::path candidate = current / ".cclean.toml";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
        const fs::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

static std::optional<std::chrono::seconds> parse_duration(
    const std::string& value)
{
    if (value.size() < 2) {
        return std::nullopt;
    }
    const char suffix = value.back();
    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    // The suffix has to be the only thing left, or "1dd" and "1d5d" both read
    // as one day: comparing *end to the last character alone cannot tell them
    // from "1d".
    const bool consumed = end == value.c_str() + value.size() - 1;
    if (end != value.c_str() && consumed && number >= 0) {
        double multiplier = 0;
        switch (suffix) {
        case 's': multiplier = 1; break;
        case 'm': multiplier = 60; break;
        case 'h': multiplier = 60 * 60; break;
        case 'd': multiplier = 24 * 60 * 60; break;
        case 'w': multiplier = 7 * 24 * 60 * 60; break;
        default: return std::nullopt;
        }
        const double seconds = number * multiplier;
        if (seconds <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return std::chrono::seconds(static_cast<std::int64_t>(seconds));
        }
    }
    return std::nullopt;
}

static std::optional<std::uintmax_t> parse_size(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::size_t split = 0;
    while (split < value.size() &&
           ((value[split] >= '0' && value[split] <= '9') ||
            value[split] == '.')) {
        ++split;
    }
    if (split == 0) {
        return std::nullopt;
    }
    const std::string number_text = value.substr(0, split);
    char* end = nullptr;
    const double number = std::strtod(number_text.c_str(), &end);
    if (end == number_text.c_str() || *end != '\0') {
        return std::nullopt;
    }
    const std::string suffix = value.substr(split);
    double multiplier = 1;
    if (suffix == "B" || suffix.empty()) multiplier = 1;
    else if (suffix == "K" || suffix == "KiB") multiplier = 1024;
    else if (suffix == "M" || suffix == "MiB") multiplier = 1024 * 1024;
    else if (suffix == "G" || suffix == "GiB") multiplier = 1024 * 1024 * 1024;
    else if (suffix == "T" || suffix == "TiB") multiplier = 1024.0 * 1024 * 1024 * 1024;
    else return std::nullopt;
    const double bytes = number * multiplier;
    if (number < 0 || bytes > static_cast<double>(std::numeric_limits<std::uintmax_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uintmax_t>(bytes);
}

// A glob compiled to tokens. std::regex dominated the scan: on a 64k-entry
// tree it cost 257 ms of a 414 ms run, because every entry is tested against
// every pattern. Matching is open-coded instead, with exact-, prefix-, and
// suffix-shaped patterns taking a string compare rather than the token walk.
class Glob {
public:
    enum class Scope {
        NameOnly,   // Tested against the final path component only.
        NameOrPath  // Also tested against the path relative to ROOT.
    };

    Glob(const std::string& pattern, Scope scope)
        : scope_(scope)
    {
        parse(pattern);
        classify();
    }

    Scope scope() const { return scope_; }

    bool matches(std::string_view value) const {
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

private:
    struct Token {
        enum class Type {
            Character,  // A literal character.
            AnyOne,     // '?'
            AnySequence,// '*', which also spans '/'.
            AnyDirs     // "**/", i.e. nothing, or anything ending in '/'.
        };

        Type type;
        char character;
    };

    enum class Kind { Literal, Prefix, Suffix, General };

    std::vector<Token> tokens_;
    std::string text_;
    Kind kind_ = Kind::General;
    Scope scope_ = Scope::NameOrPath;

    void parse(const std::string& glob) {
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
                // Collapse runs of '*'; "a**b" and "a*b" accept the same
                // strings, and one token spares the matcher a split point.
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
    void classify() {
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

    static void set(std::vector<std::uint64_t>& states, std::size_t index) {
        states[index / 64] |= std::uint64_t{1} << (index % 64);
    }

    static bool test(
        const std::vector<std::uint64_t>& states,
        std::size_t index)
    {
        return ((states[index / 64] >> (index % 64)) & 1) != 0;
    }

    // Adds a token position and everything reachable from it without
    // consuming input: '*' and "**/" may both stand for the empty string.
    void add_closure(
        std::vector<std::uint64_t>& states,
        std::size_t index) const
    {
        while (true) {
            set(states, index);

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

    // Advances every live token position one character at a time, which
    // bounds the work at tokens * length. Backtracking instead is what makes
    // a pattern like "*a*a*a*a*b" take exponential time on a long name.
    bool match_tokens(std::string_view value) const {
        const std::size_t count = tokens_.size();
        const std::size_t words = count / 64 + 1;

        std::vector<std::uint64_t> current(words, 0);
        std::vector<std::uint64_t> next(words, 0);

        add_closure(current, 0);

        for (const char c : value) {
            std::fill(next.begin(), next.end(), 0);
            bool live = false;

            for (std::size_t i = 0; i < count; ++i) {
                if (!test(current, i)) {
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
                    // Only a '/' can close the directory run, so advancing
                    // past it is not an option on any other character.
                    set(next, i);
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

        return test(current, count);
    }
};

static bool is_terminal(int descriptor) {
    return isatty(descriptor) == 1;
}

// Escape sequences resolve to empty strings off a terminal, so the same
// output code serves a pipe. NO_COLOR is honoured (https://no-color.org).
struct Style {
    const char* reset = "";
    const char* dim = "";
    const char* bold = "";
    const char* directory = "";
    const char* warning = "";
    const char* failure = "";
    const char* success = "";

    static Style detect(int descriptor) {
        const char* const no_color = std::getenv("NO_COLOR");

        if (!is_terminal(descriptor) || (no_color && no_color[0] != '\0')) {
            return Style{};
        }

        Style style;
        style.reset = "\033[0m";
        style.dim = "\033[2m";
        style.bold = "\033[1m";
        style.directory = "\033[36m";
        style.warning = "\033[33m";
        style.failure = "\033[31m";
        style.success = "\033[32m";
        return style;
    }
};

// A single rewritten line on stderr, so a redirected target list stays clean.
// Without it a scan over a large tree looks like a hang.
class Progress {
public:
    explicit Progress(bool enabled)
        : enabled_(enabled),
          drawn_(std::chrono::steady_clock::now()) {}

    // Cheap enough to call per directory entry: the clock is only read once
    // every few hundred calls, and the line is redrawn at most every 80 ms.
    void update(const char* phase, std::uintmax_t done) {
        if (!enabled_ || ++calls_ % 256 != 0) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        if (now - drawn_ < std::chrono::milliseconds(80)) {
            return;
        }

        drawn_ = now;
        draw(phase, done);
    }

    void finish() {
        if (!enabled_ || width_ == 0) {
            return;
        }

        std::fprintf(stderr, "\r%*s\r", static_cast<int>(width_), "");
        std::fflush(stderr);
        width_ = 0;
    }

private:
    bool enabled_;
    std::chrono::steady_clock::time_point drawn_;
    std::uintmax_t calls_ = 0;
    std::size_t width_ = 0;

    void draw(const char* phase, std::uintmax_t done) {
        char line[128];
        const int written = std::snprintf(line, sizeof(line), "  %s %llu",
                                          phase,
                                          static_cast<unsigned long long>(done));

        if (written <= 0) {
            return;
        }

        const std::size_t length = static_cast<std::size_t>(written);
        const std::size_t padding = length < width_ ? width_ - length : 0;

        std::fprintf(stderr, "\r%s%*s", line, static_cast<int>(padding), "");
        std::fflush(stderr);
        width_ = length;
    }
};

// Takes one keypress without waiting for Enter. Falls back to reading a
// character from the stream when stdin is a pipe or a file, so scripts still
// work. Anything but y or Y cancels, end-of-input included.
static bool confirmed() {
    if (!is_terminal(STDIN_FILENO)) {
        const int key = std::getchar();
        return key == 'y' || key == 'Y';
    }

    termios original;

    if (tcgetattr(STDIN_FILENO, &original) != 0) {
        const int key = std::getchar();
        return key == 'y' || key == 'Y';
    }

    termios raw = original;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    // TCSAFLUSH discards anything typed ahead, so a stray keystroke from
    // before the prompt cannot stand in for the answer.
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    const int key = std::getchar();

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);

    return key == 'y' || key == 'Y';
}

static std::uintmax_t saturating_add(
    std::uintmax_t a,
    std::uintmax_t b)
{
    if (b > std::numeric_limits<std::uintmax_t>::max() - a) {
        return std::numeric_limits<std::uintmax_t>::max();
    }

    return a + b;
}

static std::string format_size(std::uintmax_t bytes) {
    static constexpr const char* units[] = {
        "B", "KiB", "MiB", "GiB", "TiB", "PiB"
    };

    double value = static_cast<double>(bytes);
    std::size_t unit = 0;

    while (value >= 1024.0 &&
           unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }

    char buffer[64];

    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu %s",
                      static_cast<unsigned long long>(bytes),
                      units[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s",
                      value,
                      units[unit]);
    }

    return buffer;
}

static std::uintmax_t file_size_or_zero(
    const fs::directory_entry& entry,
    std::vector<std::string>& errors)
{
    std::error_code ec;
    const auto size = entry.file_size(ec);

    if (ec) {
        errors.push_back("Cannot determine size of " +
                         entry.path().string() + ": " + ec.message());
        return 0;
    }

    return size;
}

// Calculates the logical size of a directory without following symlinks.
// Sizes every matched directory. Sizing is stat-bound and was 43% of a run
// over many small targets, 94% of a run over one large target, so it is spread
// across threads. The unit of work is a single directory level rather than a
// whole target: one target holding the entire tree then parallelizes just as
// well as many small ones, which a task-per-target split cannot do.
// Spreads a queue of directories across all cores. A worker pops one, runs
// `scan` on it holding no lock, and merges whatever subdirectories that turned
// up back into the queue. `scan` is responsible for its own results and their
// synchronisation; this only distributes the work.
//
//   scan(const Job&, std::vector<Job>& children)
template <typename Job, typename Scan>
static void parallel_directories(std::vector<Job> queue, Scan scan) {
    if (queue.empty()) {
        return;
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::size_t active = 0;

    const auto worker = [&] {
        std::unique_lock<std::mutex> lock(mutex);

        while (true) {
            ready.wait(lock, [&] { return !queue.empty() || active == 0; });

            if (queue.empty()) {
                // Nothing queued and nobody left who could queue more.
                ready.notify_all();
                return;
            }

            const Job job = std::move(queue.back());
            queue.pop_back();
            ++active;

            lock.unlock();

            std::vector<Job> children;
            scan(job, children);

            lock.lock();

            queue.insert(queue.end(),
                         std::make_move_iterator(children.begin()),
                         std::make_move_iterator(children.end()));
            --active;

            if (!queue.empty() || active == 0) {
                ready.notify_all();
            }
        }
    };

    unsigned int count = std::thread::hardware_concurrency();

    if (count == 0) {
        count = 1;
    }

    std::vector<std::thread> pool;
    pool.reserve(count - 1);

    for (unsigned int i = 0; i + 1 < count; ++i) {
        pool.emplace_back(worker);
    }

    worker();

    for (std::thread& thread : pool) {
        thread.join();
    }
}

// Which thread reported which error is not reproducible, so the warnings are
// ordered before they are shown rather than left to vary between runs.
static void merge_errors(
    std::vector<std::string>& found,
    std::vector<std::string>& errors)
{
    std::sort(found.begin(), found.end());
    errors.insert(errors.end(),
                  std::make_move_iterator(found.begin()),
                  std::make_move_iterator(found.end()));
}

// Sizes every matched directory. Sizing is stat-bound and was 43% of a run
// over many small targets, 94% of a run over one large target. The unit of
// work is a single directory level rather than a whole target, so one target
// holding the entire tree parallelizes as well as many small ones.
static void size_directories(
    std::vector<Target>& targets,
    std::vector<std::string>& errors,
    Progress& progress)
{
    struct Job {
        fs::path directory;
        std::size_t target;
    };

    std::vector<Job> queue;

    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].is_directory) {
            queue.push_back({targets[i].path, i});
        }
    }

    std::mutex mutex;
    std::vector<std::string> found;
    std::uintmax_t sized = 0;

    parallel_directories(
        std::move(queue),
        [&](const Job& job, std::vector<Job>& children) {
            std::uintmax_t total = 0;
            std::vector<std::string> local;
            std::optional<fs::file_time_type> newest;

            std::error_code ec;
            fs::directory_iterator it(
                job.directory,
                fs::directory_options::none,
                ec);

            const fs::directory_iterator end;

            for (; !ec && it != end; it.increment(ec)) {
                if (ec) {
                    local.push_back("Error scanning " +
                                    job.directory.string() + ": " +
                                    ec.message());
                    ec.clear();
                    continue;
                }

                const fs::directory_entry& entry = *it;

                std::error_code status_ec;
                const bool entry_is_symlink = entry.is_symlink(status_ec);

                if (status_ec) {
                    local.push_back("Cannot inspect " +
                                    entry.path().string() + ": " +
                                    status_ec.message());
                    continue;
                }

                if (entry_is_symlink) {
                    // Symlinks contribute no file contents of their own, and
                    // are not descended into.
                    continue;
                }

                std::error_code time_ec;
                const fs::file_time_type entry_time =
                    entry.last_write_time(time_ec);
                if (time_ec) {
                    local.push_back("Cannot determine modification time of " +
                                    entry.path().string() + ": " +
                                    time_ec.message());
                } else if (!newest || entry_time > *newest) {
                    newest = entry_time;
                }

                // Not a symlink, so status() and symlink_status() agree here.
                if (entry.is_directory(status_ec)) {
                    children.push_back({entry.path(), job.target});
                } else if (entry.is_regular_file(status_ec)) {
                    total = saturating_add(total,
                                           file_size_or_zero(entry, local));
                }
            }

            if (ec) {
                local.push_back("Error scanning " + job.directory.string() +
                                ": " + ec.message());
            }

            const std::lock_guard<std::mutex> guard(mutex);

            Target& target = targets[job.target];
            target.size = saturating_add(target.size, total);
            if (newest && (!target.has_time || *newest > target.newest_time)) {
                target.newest_time = *newest;
                target.has_time = true;
            }

            found.insert(found.end(),
                         std::make_move_iterator(local.begin()),
                         std::make_move_iterator(local.end()));

            progress.update("sizing", ++sized);
        });

    merge_errors(found, errors);
}

static bool matches_any(
    const fs::path& path,
    const fs::path& root,
    const std::string& filename,
    const std::vector<Glob>& patterns)
{
    std::string relative;
    bool relative_built = false;

    for (const auto& pattern : patterns) {
        if (pattern.matches(filename)) {
            return true;
        }

        if (pattern.scope() == Glob::Scope::NameOnly) {
            continue;
        }

        if (!relative_built) {
            relative = path.lexically_relative(root).generic_string();
            relative_built = true;
        }

        if (pattern.matches(relative)) {
            return true;
        }
    }

    return false;
}

static std::size_t matching_pattern(
    const fs::path& path,
    const fs::path& root,
    const std::string& filename,
    const std::vector<Glob>& patterns)
{
    std::string relative;
    bool relative_built = false;

    for (std::size_t i = 0; i < patterns.size(); ++i) {
        const Glob& pattern = patterns[i];
        if (pattern.matches(filename)) {
            return i;
        }

        if (pattern.scope() == Glob::Scope::NameOnly) {
            continue;
        }

        if (!relative_built) {
            relative = path.lexically_relative(root).generic_string();
            relative_built = true;
        }

        if (pattern.matches(relative)) {
            return i;
        }
    }

    return patterns.size();
}

static bool has_entry(const fs::path& directory, std::string_view name) {
    std::error_code ec;
    return fs::exists(directory / std::string(name), ec);
}

// "build" and "target" are ordinary names, so they only count as artifacts
// beside the marker files of a project that generates one. The .git test is
// what keeps a stray directory called build out of the list.
// True when a directory between `project` and ROOT is itself a repository.
// A submodule or a vendored checkout carries its own .git and marker file, so
// without this its build output matches even though it sits well below the top
// level of the project being cleaned.
static bool has_enclosing_project(fs::path project, const fs::path& root) {
    const fs::path stop = root.lexically_normal();
    project = project.lexically_normal();

    while (project != stop) {
        const fs::path parent = project.parent_path();

        if (parent.empty() || parent == project) {
            // Ran out of path before reaching ROOT.
            return false;
        }

        project = parent;

        if (has_entry(project, ".git")) {
            return true;
        }
    }

    return false;
}

// lexically_normal() keeps a trailing separator when the path ends in a dot
// component, so "/a/b/." becomes "/a/b/", which compares unequal to "/a/b".
// A ROOT of "." reaches here as exactly that, so directory comparison needs
// the separator gone.
static fs::path normalize_directory(const fs::path& directory) {
    const fs::path normalized = directory.lexically_normal();

    if (normalized.filename().empty() && normalized.has_parent_path()) {
        return normalized.parent_path();
    }

    return normalized;
}

static bool is_artifact_directory(
    const fs::path& directory,
    const std::string& name,
    const fs::path& root,
    const std::vector<fs::path>& project_roots = {})
{
    // Name first: it costs a few string compares, where the marker tests below
    // each cost a stat.
    bool named = false;

    for (const defaults::Artifact& artifact : defaults::artifacts) {
        if (artifact.directory == name) {
            named = true;
            break;
        }
    }

    if (!named) {
        return false;
    }

    const fs::path project =
        normalize_directory(fs::absolute(directory.parent_path()));
    const fs::path normalized_root =
        normalize_directory(fs::absolute(root));

    bool configured_project = false;
    for (const fs::path& project_root : project_roots) {
        if (project == project_root) {
            configured_project = true;
            break;
        }
    }

    if (!configured_project && !has_entry(project, ".git")) {
        return false;
    }

    // The artifact must sit at the top level of the outermost project.
    if (!configured_project && has_enclosing_project(project, normalized_root)) {
        return false;
    }

    for (const defaults::Artifact& artifact : defaults::artifacts) {
        if (artifact.directory == name &&
            has_entry(project, artifact.marker)) {
            return true;
        }
    }

    return false;
}

static bool is_dependency_directory(
    const fs::path& directory,
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& configured = {})
{
    for (const defaults::Artifact& dependency : defaults::dependencies) {
        if (dependency.directory == name &&
            has_entry(directory.parent_path(), dependency.marker)) {
            return true;
        }
    }
    // Configured pairs take the same marker guard and the same --dependencies
    // gate as the built-ins, which is what separates them from a pattern.
    for (const auto& dependency : configured) {
        if (dependency.first == name &&
            has_entry(directory.parent_path(), dependency.second)) {
            return true;
        }
    }
    return false;
}

static bool is_skipped(const std::string& name) {
    return std::find(defaults::skipped.begin(),
                     defaults::skipped.end(),
                     name) != defaults::skipped.end();
}

// Walks the tree and collects everything that matches. Listing a directory is
// a syscall the thread spends its time waiting on, so the levels are spread
// across cores the same way sizing is. A matched directory is a single
// deletion target and is not descended into, which is what keeps the contents
// of a matched __pycache__ out of both the walk and the listing.
static void scan_tree(
    const fs::path& root,
    const std::vector<Glob>& patterns,
    const std::vector<Glob>& excludes,
    bool use_skips,
    bool build_artifacts,
    bool dependencies,
    const std::vector<std::pair<std::string, std::string>>& dependency_markers,
    const std::vector<fs::path>& project_roots,
    std::size_t builtin_count,
    std::size_t config_count,
    std::vector<Target>& targets,
    std::vector<std::string>& errors,
    Progress& progress)
{
    std::mutex mutex;
    std::vector<std::string> found;
    std::uintmax_t seen = 0;

    std::vector<fs::path> queue;
    queue.push_back(root);

    parallel_directories(
        std::move(queue),
        [&](const fs::path& directory, std::vector<fs::path>& children) {
            std::vector<Target> local_targets;
            std::vector<std::string> local;
            std::uintmax_t local_seen = 0;

            std::error_code ec;
            fs::directory_iterator it(
                directory,
                fs::directory_options::none,
                ec);

            const fs::directory_iterator end;

            for (; !ec && it != end; it.increment(ec)) {
                if (ec) {
                    local.push_back("Error scanning " + directory.string() +
                                    ": " + ec.message());
                    ec.clear();
                    continue;
                }

                ++local_seen;

                const fs::directory_entry& entry = *it;
                const fs::path& path = entry.path();

                std::error_code status_ec;
                const bool is_symlink = entry.is_symlink(status_ec);

                if (status_ec) {
                    local.push_back("Cannot inspect " + path.string() + ": " +
                                    status_ec.message());
                    continue;
                }

                const std::string filename = path.filename().generic_string();

                // Neither matched nor descended into: see defaults::skipped.
                if (use_skips && is_skipped(filename)) {
                    continue;
                }

                // --exclude prunes rather than only suppressing the match, so
                // that naming a directory keeps everything under it. That is
                // what lets --exclude .venv restore protection the built-in
                // list no longer gives.
                if (!excludes.empty() &&
                    matches_any(path, root, filename, excludes)) {
                    continue;
                }

                const bool is_directory =
                    !is_symlink && entry.is_directory(status_ec);

                const std::size_t pattern_index =
                    matching_pattern(path, root, filename, patterns);
                const bool artifact = build_artifacts && is_directory &&
                    is_artifact_directory(path, filename, root, project_roots);
                const bool dependency = dependencies && is_directory &&
                    is_dependency_directory(path, filename, dependency_markers);

                if (pattern_index == patterns.size() && !artifact && !dependency) {
                    // Symlinks are never followed, so only a real directory
                    // is worth queueing.
                    if (is_directory) {
                        children.push_back(path);
                    }

                    continue;
                }

                Target target;
                target.path = path;
                target.is_directory = is_directory;
                target.is_symlink = is_symlink;
                std::error_code time_ec;
                target.newest_time = entry.last_write_time(time_ec);
                target.has_time = !time_ec;
                if (time_ec) {
                    local.push_back("Cannot determine modification time of " +
                                    path.string() + ": " + time_ec.message());
                }
                if (artifact) {
                    target.reason = "build-artifact";
                } else if (dependency) {
                    target.reason = "dependency";
                } else if (pattern_index < builtin_count) {
                    target.reason = "default";
                } else if (pattern_index < builtin_count + config_count) {
                    target.reason = "config";
                } else {
                    target.reason = "command-line";
                }

                if (!is_directory && !is_symlink &&
                    entry.is_regular_file(status_ec)) {
                    target.size = file_size_or_zero(entry, local);
                } else {
                    // A matched directory is sized later, in one pass over
                    // all of them. Symlinks and other special files have no
                    // ordinary size.
                    target.size = 0;
                }

                local_targets.push_back(std::move(target));
            }

            if (ec) {
                local.push_back("Error scanning " + directory.string() +
                                ": " + ec.message());
            }

            const std::lock_guard<std::mutex> guard(mutex);

            targets.insert(targets.end(),
                           std::make_move_iterator(local_targets.begin()),
                           std::make_move_iterator(local_targets.end()));
            found.insert(found.end(),
                         std::make_move_iterator(local.begin()),
                         std::make_move_iterator(local.end()));

            seen += local_seen;
            progress.update("scanning", seen);
        });

    merge_errors(found, errors);
}

static std::string builtin_patterns() {
    std::string list;

    for (const std::string_view pattern : defaults::patterns) {
        if (!list.empty()) {
            list += "  ";
        }
        list += pattern;
    }

    return list;
}

static std::string artifact_directories() {
    std::vector<std::string_view> names;

    for (const defaults::Artifact& artifact : defaults::artifacts) {
        if (std::find(names.begin(), names.end(), artifact.directory) ==
            names.end()) {
            names.push_back(artifact.directory);
        }
    }

    std::sort(names.begin(), names.end());

    std::string list;
    std::size_t column = 0;

    for (const std::string_view name : names) {
        if (column != 0 && column + 2 + name.size() > 68) {
            list += "\n  ";
            column = 0;
        } else if (column != 0) {
            list += "  ";
            column += 2;
        }

        list += name;
        column += name.size();
    }

    return list;
}

static std::string skipped_directories() {
    std::string list;

    for (const std::string_view name : defaults::skipped) {
        if (!list.empty()) {
            list += "  ";
        }
        list += name;
    }

    return list;
}

static std::string json_string(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                result += escaped;
            } else {
                result += static_cast<char>(c);
            }
        }
    }
    result += '"';
    return result;
}

static void print_json(
    const fs::path& root,
    const std::vector<Target>& targets,
    const std::vector<std::string>& errors,
    bool dry_run,
    bool cancelled,
    std::size_t removed,
    std::size_t failed)
{
    std::uintmax_t total = 0;
    std::map<std::string, std::pair<std::size_t, std::uintmax_t>> stats;

    for (const Target& target : targets) {
        total = saturating_add(total, target.size);
        auto& stat = stats[target.reason];
        ++stat.first;
        stat.second = saturating_add(stat.second, target.size);
    }

    // Status reports the action, so a scan warning does not disguise a dry run
    // as a failed removal. Warnings are their own array, and still set exit 1.
    std::string status;
    if (failed != 0) {
        status = "failed";
    } else if (targets.empty()) {
        status = "empty";
    } else if (cancelled) {
        status = "cancelled";
    } else if (dry_run) {
        status = "dry-run";
    } else {
        status = "removed";
    }

    std::cout << "{\n"
              << "  \"root\": " << json_string(root.string()) << ",\n"
              << "  \"status\": " << json_string(status) << ",\n"
              << "  \"targets\": [";

    for (std::size_t i = 0; i < targets.size(); ++i) {
        const Target& target = targets[i];
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << "\n    {\"path\": "
                  << json_string(target.path.string())
                  << ", \"type\": "
                  << json_string(target.is_directory ? "directory" : "file")
                  << ", \"bytes\": " << target.size
                  << ", \"reason\": " << json_string(target.reason)
                  << "}";
    }

    std::cout << "\n  ],\n"
              << "  \"total\": {\"targets\": " << targets.size()
              << ", \"bytes\": " << total << "},\n"
              << "  \"stats\": {";

    std::size_t stat_index = 0;
    for (const auto& entry : stats) {
        if (stat_index++ != 0) {
            std::cout << ',';
        }
        std::cout << "\n    " << json_string(entry.first)
                  << ": {\"targets\": " << entry.second.first
                  << ", \"bytes\": " << entry.second.second << "}";
    }

    std::cout << "\n  },\n"
              << "  \"removed\": " << removed << ",\n"
              << "  \"failed\": " << failed << ",\n"
              << "  \"warnings\": [";
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << "\n    " << json_string(errors[i]);
    }
    std::cout << "\n  ]\n}\n";
}

static bool validate_target(const Target& target, std::string& error) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(target.path, ec);
    if (ec || status.type() == fs::file_type::not_found) {
        error = "Target changed or disappeared: " + target.path.string();
        return false;
    }

    const bool is_symlink = status.type() == fs::file_type::symlink;
    const bool is_directory = status.type() == fs::file_type::directory;
    if (is_symlink != target.is_symlink ||
        (!target.is_symlink && is_directory != target.is_directory)) {
        error = "Target changed type: " + target.path.string();
        return false;
    }
    return true;
}

static void print_usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program << " [OPTION ...] ROOT [PATTERN ...]\n\n"
        << "Built-in patterns, added to any given on the command line:\n"
        << "  " << builtin_patterns() << "\n\n"
        << "Options:\n"
        << "  -n, --dry-run  List matching targets and exit without removing\n"
        << "  -b, --build-artifacts\n"
        << "                 Also remove a project's build output, listed\n"
        << "                 below\n"
        << "      --dependencies\n"
        << "                 Also remove marker-guarded dependency trees\n"
        << "  -e, --exclude PATTERN\n"
        << "                 Leave anything matching PATTERN alone, contents\n"
        << "                 included. Repeatable\n"
        << "  -v, --verbose  Name every item as it is removed\n"
        << "  -y, --yes      Remove without prompting\n"
        << "  --format FORMAT  Output human (default) or json\n"
        << "  --older-than DURATION\n"
        << "                 Match only targets older than DURATION\n"
        << "                 (s, m, h, d, w)\n"
        << "  --larger-than SIZE\n"
        << "                 Match only targets of at least SIZE\n"
        << "                 (B, K, M, G, T)\n"
        << "  -h, --help     Show this text\n"
        << "  -V, --version  Show the version and exit\n"
        << "  --no-defaults  Match only the patterns given on the command line\n"
        << "  --no-skip      Also descend into the skipped directories below\n"
        << "  --             Treat every later argument as ROOT or a pattern\n\n"
        << "Never matched or descended into, unless --no-skip is given:\n"
        << "  " << skipped_directories() << "\n\n"
        << "Removed by --build-artifacts, when .git and the marker file for\n"
        << "the ecosystem sit beside them:\n"
        << "  " << artifact_directories() << "\n\n"
        << "Examples:\n"
        << "  " << program << "                       (scans the working directory)\n"
        << "  " << program << " ./project\n"
        << "  " << program << " --dry-run ./project \"*.log\" \"*.tmp\"\n"
        << "  " << program << " --no-defaults . \"build/**\" \"**/*.o\"\n"
        << "  " << program << " --exclude .venv --exclude \"**/fixtures/**\"\n\n"
        << "Command-line patterns match either the path relative to ROOT or\n"
        << "the final filename; built-in patterns match the filename only.\n\n"
        << "Supported wildcards:\n"
        << "  *      Any sequence of characters\n"
        << "  ?      Any single character\n"
        << "  **/    Zero or more directory levels\n";
}

int main(int argc, char* argv[]) {
    bool dry_run = false;
    bool use_defaults = true;
    bool use_skips = true;
    bool verbose = false;
    bool build_artifacts = false;
    bool dependencies = false;
    bool assume_yes = false;
    bool format_json = false;
    bool cli_defaults = false;
    bool cli_build_artifacts = false;
    bool cli_dependencies = false;
    bool cli_skip = false;
    bool cli_excludes = false;
    bool cli_older_than = false;
    bool cli_larger_than = false;
    std::string older_than;
    std::string larger_than;
    std::vector<std::string> exclude_patterns;
    bool options_ended = false;
    std::vector<std::string> operands;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (!options_ended && argument == "--") {
            options_ended = true;
            continue;
        }

        // An unrecognised option is rejected rather than taken as a pattern,
        // so a mistyped --dry-run cannot fall through to a live removal.
        if (!options_ended && argument.size() > 1 && argument[0] == '-') {
            if (argument == "-n" || argument == "--dry-run") {
                dry_run = true;
                continue;
            }

            if (argument == "--no-defaults") {
                use_defaults = false;
                cli_defaults = true;
                continue;
            }

            if (argument == "--no-skip") {
                use_skips = false;
                cli_skip = true;
                continue;
            }

            if (argument == "-b" || argument == "--build-artifacts") {
                build_artifacts = true;
                cli_build_artifacts = true;
                continue;
            }

            if (argument == "--dependencies") {
                dependencies = true;
                cli_dependencies = true;
                continue;
            }

            if (argument == "-v" || argument == "--verbose") {
                verbose = true;
                continue;
            }

            if (argument == "-y" || argument == "--yes") {
                assume_yes = true;
                continue;
            }

            if (argument == "--older-than" || argument == "--larger-than") {
                if (i + 1 >= argc) {
                    std::cerr << argument << " needs a value.\n";
                    return 2;
                }
                std::string& destination = argument == "--older-than"
                    ? older_than : larger_than;
                destination = argv[++i];
                if (argument == "--older-than") cli_older_than = true;
                else cli_larger_than = true;
                continue;
            }

            if (argument.rfind("--older-than=", 0) == 0) {
                older_than = argument.substr(13);
                cli_older_than = true;
                continue;
            }

            if (argument.rfind("--larger-than=", 0) == 0) {
                larger_than = argument.substr(14);
                cli_larger_than = true;
                continue;
            }

            if (argument == "--format") {
                if (i + 1 >= argc) {
                    std::cerr << argument << " needs a format.\n";
                    return 2;
                }
                const std::string value = argv[++i];
                if (value == "json") {
                    format_json = true;
                } else if (value == "human") {
                    format_json = false;
                } else {
                    std::cerr << "Unknown format: " << value << '\n';
                    return 2;
                }
                continue;
            }

            if (argument.rfind("--format=", 0) == 0) {
                const std::string value = argument.substr(9);
                if (value == "json") {
                    format_json = true;
                } else if (value == "human") {
                    format_json = false;
                } else {
                    std::cerr << "Unknown format: " << value << '\n';
                    return 2;
                }
                continue;
            }

            if (argument == "-h" || argument == "--help") {
                print_usage(argv[0]);
                return 0;
            }

            if (argument == "-e" || argument == "--exclude") {
                if (i + 1 >= argc) {
                    std::cerr << argument << " needs a pattern.\n";
                    return 2;
                }

                // Taken verbatim, so a pattern may itself begin with a dash.
                exclude_patterns.push_back(argv[++i]);
                cli_excludes = true;
                continue;
            }

            if (argument.rfind("--exclude=", 0) == 0) {
                exclude_patterns.push_back(argument.substr(10));
                cli_excludes = true;
                continue;
            }

            if (argument == "-V" || argument == "--version") {
                std::cout << "cclean " << CCLEAN_VERSION << '\n';
                return 0;
            }

            std::cerr << "Unknown option: " << argument << "\n\n";
            print_usage(argv[0]);
            return 2;
        }

        operands.push_back(argument);
    }

    // No ROOT given means the working directory; --help is how usage is read.
    if (operands.empty()) {
        operands.push_back(".");
    }

    const fs::path root = fs::path(operands.front());

    Config config;
    const fs::path config_path = find_config(root);
    if (!config_path.empty()) {
        std::string config_error;
        if (!load_config(config_path, config, config_error)) {
            std::cerr << config_error << '\n';
            return 2;
        }
    }

    if (!cli_defaults) {
        use_defaults = config.defaults;
    }
    if (!cli_build_artifacts) {
        build_artifacts = config.build_artifacts;
    }
    if (!cli_dependencies) {
        dependencies = config.dependencies;
    }
    if (!cli_skip) {
        use_skips = config.skip_protected;
    }
    if (!cli_excludes) {
        exclude_patterns = config.excludes;
    }
    if (!cli_older_than) older_than = config.older_than;
    if (!cli_larger_than) larger_than = config.larger_than;

    const fs::path absolute_root = normalize_directory(fs::absolute(root));

    // Anchored to the config file, not to ROOT. The config is found by
    // searching upward, so a repository-level .cclean.toml is read for every
    // ROOT beneath it; resolving its entries against ROOT instead made
    // project_roots = ["packages/api"] fail with "not a directory" on any run
    // from a subdirectory, including runs that never look at build artifacts.
    // An entry naming a sibling of ROOT is not an error, it simply never
    // matches, because the walk stays under ROOT.
    const fs::path config_dir = config_path.empty()
        ? absolute_root
        : normalize_directory(fs::absolute(config_path).parent_path());

    std::vector<fs::path> project_roots;
    for (const std::string& configured : config.project_roots) {
        const fs::path relative = fs::path(configured);
        if (relative.is_absolute()) {
            std::cerr << "project_roots entries must be relative to "
                         ".cclean.toml: " << configured << '\n';
            return 2;
        }
        const fs::path candidate =
            normalize_directory(config_dir / relative);
        const std::string candidate_relative =
            candidate.lexically_relative(config_dir).generic_string();
        if (candidate_relative == ".." ||
            candidate_relative.rfind("../", 0) == 0) {
            std::cerr << "project_roots entry is outside .cclean.toml's "
                         "directory: " << configured << '\n';
            return 2;
        }
        std::error_code project_ec;
        if (!fs::is_directory(candidate, project_ec) || project_ec) {
            std::cerr << "project_roots entry is not a directory: "
                      << configured << '\n';
            return 2;
        }
        project_roots.push_back(candidate);
    }

    std::optional<std::chrono::seconds> older_limit;
    if (!older_than.empty()) {
        older_limit = parse_duration(older_than);
        if (!older_limit) {
            std::cerr << "Invalid --older-than duration: " << older_than
                      << " (use a number followed by s, m, h, d, or w)\n";
            return 2;
        }
    }

    std::optional<std::uintmax_t> larger_limit;
    if (!larger_than.empty()) {
        larger_limit = parse_size(larger_than);
        if (!larger_limit) {
            std::cerr << "Invalid --larger-than size: " << larger_than
                      << " (use B, K, M, G, or T)\n";
            return 2;
        }
    }

    // --build-artifacts and --dependencies match on project layout rather than
    // on a pattern, so either is on its own enough to give the run something
    // to do.
    if (!use_defaults && operands.size() == 1 && config.patterns.empty() &&
        !build_artifacts && !dependencies) {
        // The defaults can be switched off by the config rather than by the
        // flag, so naming --no-defaults would point at an argument the user
        // never typed.
        std::cerr << (cli_defaults ? "--no-defaults" : "defaults = false")
                  << " leaves no patterns to match; supply at least one, or "
                     "--build-artifacts, or --dependencies.\n";
        return 2;
    }

    std::vector<Glob> patterns;
    patterns.reserve(
        (use_defaults ? defaults::patterns.size() : 0) + config.patterns.size() +
        operands.size() - 1);

    const std::size_t builtin_count =
        use_defaults ? defaults::patterns.size() : 0;

    // The built-ins all name a file or directory, so matching them against the
    // relative path only ever adds false positives: ".*_cache" would otherwise
    // read as "anything under a dot-directory ending in _cache", and take
    // ".venv/lib/foo_cache" with it.
    if (use_defaults) {
        for (const std::string_view pattern : defaults::patterns) {
            patterns.emplace_back(std::string(pattern),
                                  Glob::Scope::NameOnly);
        }
    }

    for (const std::string& pattern : config.patterns) {
        patterns.emplace_back(pattern, Glob::Scope::NameOrPath);
    }

    const std::size_t config_count = config.patterns.size();

    // Additional patterns supplied by the user.
    for (std::size_t i = 1; i < operands.size(); ++i) {
        patterns.emplace_back(operands[i], Glob::Scope::NameOrPath);
    }

    std::vector<Glob> excludes;
    excludes.reserve(exclude_patterns.size());

    for (const std::string& pattern : exclude_patterns) {
        excludes.emplace_back(pattern, Glob::Scope::NameOrPath);
    }

    std::error_code ec;
    const fs::file_status root_status = fs::symlink_status(root, ec);

    if (ec) {
        std::cerr << "Cannot inspect root path: "
                  << root.string() << ": " << ec.message() << '\n';
        return 1;
    }

    if (!fs::is_directory(root_status)) {
        std::cerr << "Root path is not a directory: "
                  << root.string() << '\n';
        return 1;
    }

    std::vector<Target> targets;
    std::vector<std::string> errors;

    // Opened once here so that an unreadable ROOT is a hard failure, rather
    // than a warning raised by whichever worker happened to draw it.
    {
        fs::directory_iterator probe(
            root, fs::directory_options::none, ec);

        if (ec) {
            std::cerr << "Cannot scan root path: "
                      << ec.message() << '\n';
            return 1;
        }
    }

    Progress progress(is_terminal(STDERR_FILENO));

    scan_tree(root, patterns, excludes, use_skips, build_artifacts,
              dependencies, config.dependency_markers, project_roots,
              builtin_count, config_count,
              targets, errors, progress);

    size_directories(targets, errors, progress);
    progress.finish();

    if (older_limit || larger_limit) {
        const fs::file_time_type now = fs::file_time_type::clock::now();
        std::vector<Target> filtered;
        filtered.reserve(targets.size());

        for (Target& target : targets) {
            bool keep = true;
            if (older_limit) {
                if (!target.has_time) {
                    errors.push_back("Cannot apply age filter to " +
                                     target.path.string());
                    keep = false;
                } else if (target.newest_time >
                           now - *older_limit) {
                    keep = false;
                }
            }
            if (keep && larger_limit && target.size < *larger_limit) {
                keep = false;
            }
            if (keep) {
                filtered.push_back(std::move(target));
            }
        }
        targets.swap(filtered);
    }

    const Style out = Style::detect(STDOUT_FILENO);
    const Style err = Style::detect(STDERR_FILENO);

    if (!errors.empty()) {
        std::cerr << '\n' << err.warning << "Warnings:" << err.reset << '\n';
        for (const auto& error : errors) {
            std::cerr << "  " << error << '\n';
        }
    }

    if (targets.empty()) {
        if (format_json) {
            print_json(root, targets, errors, dry_run, false, 0, 0);
        } else {
            std::cout << "No matching targets found.\n";
        }
        return errors.empty() ? 0 : 1;
    }

    // Directory iteration order is unspecified; the user reviews this list
    // before confirming a permanent deletion.
    std::sort(targets.begin(), targets.end(),
              [](const Target& a, const Target& b) {
                  return a.path < b.path;
              });

    std::uintmax_t total_size = 0;

    for (const auto& target : targets) {
        total_size = saturating_add(total_size, target.size);
    }

    if (!format_json) {
        std::cout << "\nMatched targets:\n";

        for (const auto& target : targets) {
            if (target.is_directory) {
                std::cout << "  " << out.directory << target.path.string() << '/'
                          << out.reset;
            } else {
                std::cout << "  " << target.path.string();
            }

            std::cout << out.dim << "  " << format_size(target.size) << out.reset
                      << '\n';
        }

        std::cout << '\n'
                  << out.bold << targets.size() << out.reset
                  << (targets.size() == 1 ? " target, " : " targets, ")
                  << out.bold << format_size(total_size) << out.reset
                  << " to reclaim\n";
    }

    if (dry_run) {
        if (format_json) {
            print_json(root, targets, errors, true, false, 0, 0);
        } else {
            std::cout << out.dim << "Dry run: nothing was removed."
                      << out.reset << '\n';
        }
        return errors.empty() ? 0 : 1;
    }

    bool go_ahead = assume_yes;
    if (!assume_yes) {
        if (format_json) {
            std::cerr << "Permanently remove " << targets.size()
                      << " targets? [y/N] ";
            std::cerr.flush();
        } else {
            std::cout << "\nPermanently remove? "
                      << out.bold << "[y/N]" << out.reset << ' ';
            std::cout.flush();
        }
        go_ahead = confirmed();
        if (format_json) {
            std::cerr << (go_ahead ? "y" : "n") << '\n';
        } else {
            std::cout << (go_ahead ? "y" : "n") << '\n';
        }
    }

    if (!go_ahead) {
        if (format_json) {
            print_json(root, targets, errors, false, true, 0, 0);
        } else {
            std::cout << "Cancelled.\n";
        }
        return errors.empty() ? 0 : 1;
    }

    std::size_t removed = 0;
    std::size_t failed = 0;

    // The matched list was already shown, so only failures are named again.
    // --verbose restores the per-item log.
    for (const auto& target : targets) {
        std::string validation_error;
        if (!validate_target(target, validation_error)) {
            ++failed;
            errors.push_back(validation_error);
            if (!format_json) {
                std::cerr << "  " << err.failure << "failed" << err.reset << "  "
                          << validation_error << '\n';
            }
            continue;
        }

        std::error_code remove_ec;

        if (target.is_directory) {
            fs::remove_all(target.path, remove_ec);
        } else {
            fs::remove(target.path, remove_ec);
        }

        if (remove_ec) {
            ++failed;
            std::cerr << "  " << err.failure << "failed" << err.reset << "  "
                      << target.path.string() << ": "
                      << remove_ec.message() << '\n';
        } else {
            ++removed;

            if (verbose && !format_json) {
                std::cout << "  " << out.dim << "removed" << out.reset << "  "
                          << target.path.string() << '\n';
            }
        }
    }

    if (failed == 0) {
        if (format_json) {
            print_json(root, targets, errors, false, false, removed, failed);
        } else {
            std::cout << out.success << "Removed " << removed << out.reset
                      << ", " << format_size(total_size) << " reclaimed\n";
        }
        return errors.empty() ? 0 : 1;
    }

    if (format_json) {
        print_json(root, targets, errors, false, false, removed, failed);
    } else {
        std::cout << "Removed " << removed << ", "
                  << out.failure << failed << " failed" << out.reset << '\n';
    }

    return 1;
}
