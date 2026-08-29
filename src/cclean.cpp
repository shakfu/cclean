#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
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
};

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

            std::error_code ec;
            fs::directory_iterator it(
                job.directory,
                fs::directory_options::skip_permission_denied,
                ec);

            if (ec) {
                local.push_back("Cannot scan " + job.directory.string() +
                                ": " + ec.message());
            }

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

                // Not a symlink, so status() and symlink_status() agree here.
                if (entry.is_directory(status_ec)) {
                    children.push_back({entry.path(), job.target});
                } else if (entry.is_regular_file(status_ec)) {
                    total = saturating_add(total,
                                           file_size_or_zero(entry, local));
                }
            }

            const std::lock_guard<std::mutex> guard(mutex);

            Target& target = targets[job.target];
            target.size = saturating_add(target.size, total);

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

static bool is_artifact_directory(
    const fs::path& directory,
    const std::string& name,
    const fs::path& root)
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

    const fs::path project = directory.parent_path();

    if (!has_entry(project, ".git")) {
        return false;
    }

    // The artifact must sit at the top level of the outermost project.
    if (has_enclosing_project(project, root)) {
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
                fs::directory_options::skip_permission_denied,
                ec);

            if (ec) {
                local.push_back("Cannot scan " + directory.string() + ": " +
                                ec.message());
            }

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

                if (!matches_any(path, root, filename, patterns) &&
                    !(build_artifacts && is_directory &&
                      is_artifact_directory(path, filename, root))) {
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
        << "  -e, --exclude PATTERN\n"
        << "                 Leave anything matching PATTERN alone, contents\n"
        << "                 included. Repeatable\n"
        << "  -v, --verbose  Name every item as it is removed\n"
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
                continue;
            }

            if (argument == "--no-skip") {
                use_skips = false;
                continue;
            }

            if (argument == "-b" || argument == "--build-artifacts") {
                build_artifacts = true;
                continue;
            }

            if (argument == "-v" || argument == "--verbose") {
                verbose = true;
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
                continue;
            }

            if (argument.rfind("--exclude=", 0) == 0) {
                exclude_patterns.push_back(argument.substr(10));
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

    // --build-artifacts matches on project layout rather than on a pattern,
    // so it is on its own enough to give the run something to do.
    if (!use_defaults && operands.size() == 1 && !build_artifacts) {
        std::cerr << "--no-defaults leaves no patterns to match; "
                     "supply at least one, or --build-artifacts.\n";
        return 2;
    }

    const fs::path root = fs::path(operands.front());

    std::vector<Glob> patterns;
    patterns.reserve(
        (use_defaults ? defaults::patterns.size() : 0) + operands.size() - 1);

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
            root, fs::directory_options::skip_permission_denied, ec);

        if (ec) {
            std::cerr << "Cannot scan root path: "
                      << ec.message() << '\n';
            return 1;
        }
    }

    Progress progress(is_terminal(STDERR_FILENO));

    scan_tree(root, patterns, excludes, use_skips, build_artifacts,
              targets, errors, progress);

    size_directories(targets, errors, progress);
    progress.finish();

    const Style out = Style::detect(STDOUT_FILENO);
    const Style err = Style::detect(STDERR_FILENO);

    if (!errors.empty()) {
        std::cerr << '\n' << err.warning << "Warnings:" << err.reset << '\n';
        for (const auto& error : errors) {
            std::cerr << "  " << error << '\n';
        }
    }

    if (targets.empty()) {
        std::cout << "No matching targets found.\n";
        return 0;
    }

    // Directory iteration order is unspecified; the user reviews this list
    // before confirming a permanent deletion.
    std::sort(targets.begin(), targets.end(),
              [](const Target& a, const Target& b) {
                  return a.path < b.path;
              });

    std::uintmax_t total_size = 0;

    std::cout << "\nMatched targets:\n";

    for (const auto& target : targets) {
        total_size = saturating_add(total_size, target.size);

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

    if (dry_run) {
        std::cout << out.dim << "Dry run: nothing was removed."
                  << out.reset << '\n';
        return 0;
    }

    std::cout << "\nPermanently remove? "
              << out.bold << "[y/N]" << out.reset << ' ';
    std::cout.flush();

    const bool go_ahead = confirmed();

    std::cout << (go_ahead ? "y" : "n") << '\n';

    if (!go_ahead) {
        std::cout << "Cancelled.\n";
        return 0;
    }

    std::size_t removed = 0;
    std::size_t failed = 0;

    // The matched list was already shown, so only failures are named again.
    // --verbose restores the per-item log.
    for (const auto& target : targets) {
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

            if (verbose) {
                std::cout << "  " << out.dim << "removed" << out.reset << "  "
                          << target.path.string() << '\n';
            }
        }
    }

    if (failed == 0) {
        std::cout << out.success << "Removed " << removed << out.reset
                  << ", " << format_size(total_size) << " reclaimed\n";
        return 0;
    }

    std::cout << "Removed " << removed << ", "
              << out.failure << failed << " failed" << out.reset << '\n';

    return 1;
}

