// Unit tests for libcclean.
//
// These link the library and use its public headers. Two headers under src/
// are also reached directly: they are part of the library but not of its
// installed API, and the behaviour they carry -- the TOML grammar and the
// worker pool's exception handling -- is worth a test of its own.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "cclean/cclean.hpp"

#include "parallel.hpp"
#include "toml.hpp"

using namespace cclean;
using namespace cclean::toml;

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_group = "";

void group(const char* name) {
    g_group = name;
}

void report(bool ok, const char* expression, int line) {
    ++g_checks;

    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s (line %d): %s\n", g_group, line, expression);
    }
}

template <typename A, typename B>
void report_eq(
    const A& actual,
    const B& expected,
    const char* expression,
    int line)
{
    ++g_checks;

    if (!(actual == expected)) {
        ++g_failures;
        std::printf("  FAIL  %s (line %d): %s\n", g_group, line, expression);
    }
}

#define CHECK(expression) report((expression), #expression, __LINE__)
#define CHECK_EQ(actual, expected) \
    report_eq((actual), (expected), #actual " == " #expected, __LINE__)

Glob name_glob(const std::string& pattern) {
    return Glob(pattern, Glob::Scope::NameOnly);
}

// ---------------------------------------------------------------- Glob

void test_literal_patterns() {
    group("literal");

    const Glob g = name_glob("__pycache__");

    CHECK(g.matches("__pycache__"));
    CHECK(!g.matches("__pycache__x"));
    CHECK(!g.matches("x__pycache__"));
    CHECK(!g.matches("__pycache_"));
    CHECK(!g.matches(""));
}

void test_suffix_patterns() {
    group("suffix");

    const Glob g = name_glob("*.pyc");

    CHECK(g.matches("a.pyc"));
    CHECK(g.matches(".pyc"));
    CHECK(!g.matches("a.pyo"));
    CHECK(!g.matches("pyc"));
    CHECK(!g.matches(""));

    // '*' spans '/', so a suffix pattern also matches a whole relative path.
    CHECK(g.matches("pkg/sub/a.pyc"));
}

void test_prefix_patterns() {
    group("prefix");

    const Glob g = name_glob("build/**");

    CHECK(g.matches("build/obj"));
    CHECK(g.matches("build/a/b/c.o"));
    CHECK(g.matches("build/"));

    // "build/**" describes the contents of build, not build itself.
    CHECK(!g.matches("build"));
    CHECK(!g.matches("src/build/x"));
}

void test_single_character_wildcard() {
    group("question mark");

    const Glob one = name_glob("?");

    CHECK(one.matches("a"));
    CHECK(one.matches("/"));
    CHECK(!one.matches(""));
    CHECK(!one.matches("ab"));

    const Glob g = name_glob("?.pyc");

    CHECK(g.matches("a.pyc"));
    CHECK(!g.matches("ab.pyc"));
    CHECK(!g.matches(".pyc"));
}

void test_directory_wildcard() {
    group("**/");

    const Glob g = name_glob("**/x");

    CHECK(g.matches("x"));
    CHECK(g.matches("a/x"));
    CHECK(g.matches("a/b/c/x"));

    // The distinguishing case: "**/" needs a directory boundary, so it does
    // not behave as a bare "*". A regression here would silently widen every
    // "**/" pattern a user writes.
    CHECK(!g.matches("ax"));
    CHECK(!g.matches("a/bx"));

    const Glob o = name_glob("**/*.o");

    CHECK(o.matches("o1.o"));
    CHECK(o.matches("build/obj/o1.o"));
    CHECK(!o.matches("o1.obj"));

    const Glob bare = name_glob("**/");

    CHECK(bare.matches(""));
    CHECK(bare.matches("a/"));
    CHECK(bare.matches("a/b/"));
    CHECK(!bare.matches("a"));
}

void test_star_spans_slash() {
    group("star spans slash");

    // Documented dialect behaviour: '*' crosses directory separators. The
    // README states this, and the built-in patterns rely on the filename
    // scope rather than on '*' stopping at '/'.
    const Glob g = name_glob("a*b");

    CHECK(g.matches("ab"));
    CHECK(g.matches("axb"));
    CHECK(g.matches("a/x/b"));
}

void test_metacharacters_are_literal() {
    group("regex metacharacters");

    // These are ordinary characters in a glob. An earlier implementation
    // translated globs to std::regex, where mishandling any of them would
    // change the match or abort the program.
    CHECK(name_glob("a+b").matches("a+b"));
    CHECK(!name_glob("a+b").matches("aab"));

    CHECK(name_glob("a.b").matches("a.b"));
    CHECK(!name_glob("a.b").matches("axb"));

    CHECK(name_glob("[a]").matches("[a]"));
    CHECK(!name_glob("[a]").matches("a"));

    CHECK(name_glob("(a|b)").matches("(a|b)"));
    CHECK(!name_glob("(a|b)").matches("a"));

    CHECK(name_glob("^a$").matches("^a$"));
    CHECK(!name_glob("^a$").matches("a"));

    CHECK(name_glob("a\\b").matches("a\\b"));
    CHECK(name_glob("a{1}").matches("a{1}"));
}

void test_degenerate_patterns() {
    group("degenerate patterns");

    const Glob empty = name_glob("");

    CHECK(empty.matches(""));
    CHECK(!empty.matches("a"));

    const Glob star = name_glob("*");

    CHECK(star.matches(""));
    CHECK(star.matches("anything/at/all"));

    // Runs of '*' collapse; "a**b" and "a*b" accept the same strings.
    CHECK(name_glob("a**b").matches("axb"));
    CHECK(name_glob("a***b").matches("axb"));
    CHECK(!name_glob("a**b").matches("axc"));

    CHECK(name_glob("****a").matches("a"));
    CHECK(!name_glob("****a").matches("b"));
}

void test_matching_is_bounded() {
    group("bounded matching");

    // A backtracking matcher takes exponential time on this input. The
    // matcher simulates the pattern instead, which bounds the work at
    // tokens * length. Without that bound this test does not terminate.
    const Glob g = name_glob("*a*a*a*a*a*a*a*a*b");
    const std::string value(4096, 'a');

    const auto start = std::chrono::steady_clock::now();
    const bool matched = g.matches(value);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(!matched);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
              .count() < 1000);
}

// ------------------------------------------------------------- defaults

void test_default_patterns() {
    group("default patterns");

    const Glob cache = name_glob(".*_cache");

    // .*_cache is load-bearing: it is the only entry covering the per-tool
    // caches, which have no named patterns of their own.
    CHECK(cache.matches(".pytest_cache"));
    CHECK(cache.matches(".mypy_cache"));
    CHECK(cache.matches(".ruff_cache"));
    CHECK(cache.matches(".tox_cache"));
    CHECK(cache.matches("._cache"));

    CHECK(!cache.matches("pytest_cache"));
    CHECK(!cache.matches(".pytest_cachex"));
    CHECK(!cache.matches("_cache"));

    CHECK(name_glob(".DS_Store").matches(".DS_Store"));
    CHECK(!name_glob(".DS_Store").matches("DS_Store"));

    for (const std::string_view pattern : defaults::patterns) {
        CHECK(!pattern.empty());
    }
}

// Exercises the real defaults::patterns array rather than a copy of one of
// its entries, so that removing or mistyping an entry fails a test.
void test_defaults_are_wired_up() {
    group("defaults wired up");

    std::vector<Glob> patterns;

    for (const std::string_view pattern : defaults::patterns) {
        patterns.emplace_back(std::string(pattern), Glob::Scope::NameOnly);
    }

    const fs::path root = "/r";

    const auto cleaned = [&](const std::string& name) {
        return matches_any(root / name, root, name, patterns);
    };

    CHECK(cleaned("__pycache__"));
    CHECK(cleaned("mod.pyc"));
    CHECK(cleaned("mod.pyo"));
    CHECK(cleaned(".pytest_cache"));
    CHECK(cleaned(".mypy_cache"));
    CHECK(cleaned(".ruff_cache"));
    CHECK(cleaned(".tox_cache"));
    CHECK(cleaned(".DS_Store"));

    CHECK(!cleaned("main.py"));
    CHECK(!cleaned("README.md"));
    CHECK(!cleaned("pycache"));
    CHECK(!cleaned("pytest_cache"));
    CHECK(!cleaned("src"));
    CHECK(!cleaned("node_modules"));
    CHECK(!cleaned("build"));
    CHECK(!cleaned("target"));
}

void test_skip_list() {
    group("skip list");

    CHECK(is_skipped(".git"));
    CHECK(is_skipped(".hg"));
    CHECK(is_skipped(".svn"));
    CHECK(is_skipped(".config"));
    CHECK(is_skipped(".ssh"));
    CHECK(is_skipped(".gnupg"));

    // Virtual environments are walked: they hold the largest concentration
    // of __pycache__ in a typical Python project.
    CHECK(!is_skipped(".venv"));
    CHECK(!is_skipped("venv"));

    CHECK(!is_skipped("git"));
    CHECK(!is_skipped(".gitignore"));
    CHECK(!is_skipped(".git/objects"));
    CHECK(!is_skipped(""));
    CHECK(!is_skipped("src"));
}

// ------------------------------------------------------------ scoping

void test_pattern_scope() {
    group("pattern scope");

    const fs::path root = "/r";
    const fs::path path = "/r/.venv/lib/foo_cache";
    const std::string filename = "foo_cache";

    // A built-in is tested against the filename only. This is what stops
    // ".*_cache" from reading as "anything under a dot-directory ending in
    // _cache" and taking .venv/lib/foo_cache with it.
    std::vector<Glob> builtin;
    builtin.emplace_back(".*_cache", Glob::Scope::NameOnly);
    CHECK(!matches_any(path, root, filename, builtin));

    // The same pattern given on the command line is also tested against the
    // relative path, where it does match. The scope is the whole difference.
    std::vector<Glob> supplied;
    supplied.emplace_back(".*_cache", Glob::Scope::NameOrPath);
    CHECK(matches_any(path, root, filename, supplied));

    // A genuine cache directory matches under either scope.
    const fs::path real = "/r/src/.ruff_cache";
    CHECK(matches_any(real, root, ".ruff_cache", builtin));
    CHECK(matches_any(real, root, ".ruff_cache", supplied));

    // A path-scoped pattern that only the relative path can satisfy.
    std::vector<Glob> nested;
    nested.emplace_back("build/**", Glob::Scope::NameOrPath);
    CHECK(matches_any("/r/build/obj", root, "obj", nested));
    CHECK(!matches_any("/r/src/build/obj", root, "obj", nested));

    CHECK(matches_any(path, root, filename, {}) == false);
}

// Excludes are ordinary globs given the same scope as command-line patterns.
// What differs is where the walk applies them, which the command-line suite
// covers; here only the matching is checked.
void test_exclude_scope() {
    group("exclude scope");

    std::vector<Glob> excludes;
    excludes.emplace_back(".venv", Glob::Scope::NameOrPath);
    excludes.emplace_back("**/fixtures/**", Glob::Scope::NameOrPath);

    const fs::path root = "/r";

    CHECK(matches_any("/r/.venv", root, ".venv", excludes));
    CHECK(matches_any("/r/a/tests/fixtures/x", root, "x", excludes));
    CHECK(!matches_any("/r/src", root, "src", excludes));
    CHECK(!matches_any("/r/venv", root, "venv", excludes));
}

// ------------------------------------------------------------- sizing

void test_format_size() {
    group("format_size");

    CHECK_EQ(format_size(0), std::string("0 B"));
    CHECK_EQ(format_size(1), std::string("1 B"));
    CHECK_EQ(format_size(1023), std::string("1023 B"));
    CHECK_EQ(format_size(1024), std::string("1.00 KiB"));
    CHECK_EQ(format_size(1536), std::string("1.50 KiB"));
    CHECK_EQ(format_size(1024 * 1024), std::string("1.00 MiB"));
    CHECK_EQ(format_size(1024ull * 1024 * 1024), std::string("1.00 GiB"));
    CHECK_EQ(format_size(1024ull * 1024 * 1024 * 1024),
             std::string("1.00 TiB"));

    // The unit table ends at PiB; a larger value must not walk off it.
    const std::string huge =
        format_size(std::numeric_limits<std::uintmax_t>::max());
    CHECK(huge.find("PiB") != std::string::npos);
}

void test_saturating_add() {
    group("saturating_add");

    const std::uintmax_t max = std::numeric_limits<std::uintmax_t>::max();

    CHECK_EQ(saturating_add(0, 0), std::uintmax_t{0});
    CHECK_EQ(saturating_add(2, 3), std::uintmax_t{5});
    CHECK_EQ(saturating_add(max, 0), max);
    CHECK_EQ(saturating_add(max, 1), max);
    CHECK_EQ(saturating_add(max, max), max);
    CHECK_EQ(saturating_add(max - 1, 1), max);
}

// --------------------------------------------------- build artifacts

class TempTree {
public:
    TempTree() {
        root_ = fs::temp_directory_path() /
                ("cclean-unit-" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    const fs::path& root() const { return root_; }

    // Creates PROJECT/DIRECTORY plus each named marker file beside it.
    fs::path project(
        const std::string& name,
        const std::string& artifact,
        const std::vector<std::string>& markers)
    {
        const fs::path project_path = root_ / name;
        fs::create_directories(project_path / artifact);

        for (const std::string& marker : markers) {
            if (marker == ".git") {
                fs::create_directories(project_path / ".git");
            } else {
                std::ofstream(project_path / marker) << "x";
            }
        }

        return project_path / artifact;
    }

private:
    fs::path root_;
};

void test_artifact_detection() {
    group("build artifacts");

    TempTree tree;

    const fs::path cmake =
        tree.project("cmake", "build", {".git", "CMakeLists.txt"});
    CHECK(is_artifact_directory(cmake, "build", tree.root()));

    const fs::path cargo =
        tree.project("cargo", "target", {".git", "Cargo.toml"});
    CHECK(is_artifact_directory(cargo, "target", tree.root()));

    // Cargo.lock is not required.
    const fs::path locked =
        tree.project("locked", "target", {".git", "Cargo.toml", "Cargo.lock"});
    CHECK(is_artifact_directory(locked, "target", tree.root()));

    // Each guard on its own.
    const fs::path no_git = tree.project("no_git", "build", {"CMakeLists.txt"});
    CHECK(!is_artifact_directory(no_git, "build", tree.root()));

    const fs::path no_marker = tree.project("no_marker", "build", {".git"});
    CHECK(!is_artifact_directory(no_marker, "build", tree.root()));

    const fs::path lock_only =
        tree.project("lock_only", "target", {".git", "Cargo.lock"});
    CHECK(!is_artifact_directory(lock_only, "target", tree.root()));

    // The marker must match the artifact: CMakeLists.txt does not license a
    // target/, and Cargo.toml does not license a build/.
    const fs::path crossed =
        tree.project("crossed", "target", {".git", "CMakeLists.txt"});
    CHECK(!is_artifact_directory(crossed, "target", tree.root()));

    const fs::path crossed_two =
        tree.project("crossed_two", "build", {".git", "Cargo.toml"});
    CHECK(!is_artifact_directory(crossed_two, "build", tree.root()));

    // Only these two names are ever considered.
    const fs::path other =
        tree.project("other", "output", {".git", "CMakeLists.txt"});
    CHECK(!is_artifact_directory(other, "output", tree.root()));
    CHECK(!is_artifact_directory(cmake, "dist", tree.root()));

    // One directory name, several ecosystems. Each marker licenses only the
    // directory it is paired with.
    struct Case { const char* dir; const char* marker; };
    const Case supported[] = {
        {"build", "CMakeLists.txt"}, {"build", "meson.build"},
        {"build", "package.json"},   {"build", "build.gradle"},
        {"build", "build.gradle.kts"}, {"build", "pyproject.toml"},
        {"build", "setup.py"},       {"build", "pubspec.yaml"},
        {"target", "Cargo.toml"},    {"target", "pom.xml"},
        {"dist", "package.json"},    {"dist", "pyproject.toml"},
        {"dist", "setup.py"},        {".next", "package.json"},
        {".nuxt", "package.json"},   {".svelte-kit", "package.json"},
        {".turbo", "package.json"},  {".parcel-cache", "package.json"},
        {".gradle", "build.gradle"}, {".gradle", "build.gradle.kts"},
        {"zig-out", "build.zig"},    {"zig-cache", "build.zig"},
        {".zig-cache", "build.zig"}, {".build", "Package.swift"},
        {"_build", "mix.exs"},
    };

    int index = 0;

    for (const Case& c : supported) {
        const std::string name = "case" + std::to_string(index++);
        const fs::path made = tree.project(name, c.dir, {".git", c.marker});
        CHECK(is_artifact_directory(made, c.dir, tree.root()));
    }

    // Every pair in the table is covered above.
    CHECK_EQ(sizeof(supported) / sizeof(supported[0]),
             defaults::artifacts.size());

    // A marker does not license a directory it is not paired with.
    const fs::path wrong_pair =
        tree.project("wrong_pair", "target", {".git", "package.json"});
    CHECK(!is_artifact_directory(wrong_pair, "target", tree.root()));

    const fs::path wrong_pair_two =
        tree.project("wrong_pair_two", "zig-out", {".git", "Cargo.toml"});
    CHECK(!is_artifact_directory(wrong_pair_two, "zig-out", tree.root()));

    // A nested repository's build output belongs to a deeper level than the
    // one being cleaned. This is the shape of a git submodule: the outer
    // project has .git and a marker, and so does the inner one.
    const fs::path outer = tree.root() / "outer";
    fs::create_directories(outer / ".git");
    fs::create_directories(outer / "build");
    std::ofstream(outer / "CMakeLists.txt") << "x";

    const fs::path inner = outer / "lib" / "vendored";
    fs::create_directories(inner / "build");
    std::ofstream(inner / ".git") << "gitdir: ../../.git/modules/vendored";
    std::ofstream(inner / "CMakeLists.txt") << "x";

    CHECK(is_artifact_directory(outer / "build", "build", tree.root()));
    CHECK(!is_artifact_directory(inner / "build", "build", tree.root()));

    // Pointing ROOT at the nested project makes it the top level again.
    CHECK(is_artifact_directory(inner / "build", "build", inner));

    // Depth alone is not the test: a project several levels below ROOT still
    // qualifies, as long as nothing between it and ROOT is a repository.
    const fs::path deep = tree.root() / "a" / "b" / "c";
    fs::create_directories(deep / ".git");
    fs::create_directories(deep / "build");
    std::ofstream(deep / "CMakeLists.txt") << "x";
    CHECK(is_artifact_directory(deep / "build", "build", tree.root()));

    // A .git file, as submodules and worktrees use, counts like a directory.
    const fs::path submodule = tree.root() / "submodule";
    fs::create_directories(submodule / "build");
    std::ofstream(submodule / ".git") << "gitdir: ../.git/modules/x";
    std::ofstream(submodule / "CMakeLists.txt") << "x";
    CHECK(is_artifact_directory(submodule / "build", "build", tree.root()));
}

// ------------------------------------------------------------- scanning

// The reason a target reports used to be worked out from the pattern's index
// against two running counts, which every caller had to keep in step with the
// order the patterns were built in. It is carried on the pattern itself now,
// so the wiring is what this covers: the three sources, the scope each gets,
// and the reasons that come back out of a real walk.
void test_scan_reasons() {
    group("scan reasons");

    TempTree tree;
    const fs::path root = tree.root();

    fs::create_directories(root / "pkg" / "__pycache__");
    std::ofstream(root / "pkg" / "__pycache__" / "a.bin") << "0123456789";
    std::ofstream(root / "pkg" / "keep.py") << "x";
    std::ofstream(root / "notes.log") << "xx";
    std::ofstream(root / "scratch.tmp") << "xxx";

    fs::create_directories(root / "vendor");
    std::ofstream(root / "vendor" / "bundled.log") << "x";

    ScanOptions options;
    options.patterns = compile_patterns(true, {"*.tmp"}, {"*.log"});
    options.excludes = compile_excludes({"vendor"});

    const ScanResult result = scan(root, options);

    CHECK(result.warnings.empty());
    CHECK_EQ(result.targets.size(), 3u);

    // Sorted by path, so the order is fixed rather than the walk's.
    CHECK(std::is_sorted(result.targets.begin(), result.targets.end(),
                         [](const Target& a, const Target& b) {
                             return a.path < b.path;
                         }));

    const auto find = [&](const std::string& name) -> const Target* {
        for (const Target& target : result.targets) {
            if (target.path.filename() == name) {
                return &target;
            }
        }
        return nullptr;
    };

    const Target* cache = find("__pycache__");
    const Target* log = find("notes.log");
    const Target* tmp = find("scratch.tmp");

    CHECK(cache != nullptr);
    CHECK(log != nullptr);
    CHECK(tmp != nullptr);

    if (!cache || !log || !tmp) {
        return;
    }

    CHECK_EQ(std::string(reason_name(cache->reason)), std::string("default"));
    CHECK_EQ(std::string(reason_name(tmp->reason)), std::string("config"));
    CHECK_EQ(std::string(reason_name(log->reason)),
             std::string("command-line"));

    // A matched directory is one target, sized from its contents, and is not
    // descended into: keep.py sits beside it and is not a target of its own.
    CHECK(cache->is_directory);
    CHECK_EQ(cache->size, std::uintmax_t{10});
    CHECK(find("a.bin") == nullptr);
    CHECK(find("keep.py") == nullptr);

    // An exclude prunes, so nothing under vendor/ is reachable at all.
    CHECK(find("bundled.log") == nullptr);

    // The size filter runs after sizing, so it sees the directory total.
    ScanOptions large = options;
    large.larger_than = std::uintmax_t{4};
    CHECK_EQ(scan(root, large).targets.size(), 1u);

    // Nothing in a tree made moments ago is a day old.
    ScanOptions old = options;
    old.older_than = std::chrono::hours(24);
    CHECK_EQ(scan(root, old).targets.size(), 0u);
}

// ------------------------------------------------------ numeric parsing

// The previous parser went through double and range-checked against
// static_cast<double>(the integer maximum). Neither maximum is representable
// as a double, so the bound rounded up to the next power of two and a value
// written at the boundary passed the check and then reached an out-of-range
// floating-to-integer conversion, which is undefined.
void test_parse_size() {
    group("parse_size");

    CHECK_EQ(parse_size("0").value(), 0u);
    CHECK_EQ(parse_size("0B").value(), 0u);
    CHECK_EQ(parse_size("1B").value(), 1u);
    CHECK_EQ(parse_size("512").value(), 512u);
    CHECK_EQ(parse_size("1K").value(), 1024u);
    CHECK_EQ(parse_size("1KiB").value(), 1024u);
    CHECK_EQ(parse_size("1M").value(), 1024u * 1024);
    CHECK_EQ(parse_size("1G").value(), 1024u * 1024 * 1024);
    CHECK_EQ(parse_size("1T").value(), 1024ull * 1024 * 1024 * 1024);

    // Fixed point, truncated toward zero, exact at every step.
    CHECK_EQ(parse_size("1.5K").value(), 1536u);
    CHECK_EQ(parse_size("0.5B").value(), 0u);
    CHECK_EQ(parse_size("2.25M").value(), 2359296u);
    CHECK_EQ(parse_size(".5K").value(), 512u);
    CHECK_EQ(parse_size("1.001K").value(), 1025u);

    const auto maximum = std::numeric_limits<std::uintmax_t>::max();
    CHECK_EQ(parse_size("18446744073709551615").value(), maximum);
    CHECK_EQ(parse_size("18446744073709551615B").value(), maximum);
    CHECK(!parse_size("18446744073709551616B").has_value());
    CHECK(!parse_size("99999999999999999999999B").has_value());

    // 16777216T is exactly 2^64 bytes: one past the representable maximum.
    CHECK_EQ(parse_size("16777215T").value(), 16777215ull * 1024 * 1024 * 1024 * 1024);
    CHECK(!parse_size("16777216T").has_value());

    CHECK(!parse_size("").has_value());
    CHECK(!parse_size("B").has_value());
    CHECK(!parse_size(".").has_value());
    CHECK(!parse_size(".B").has_value());
    CHECK(!parse_size("-1B").has_value());
    CHECK(!parse_size("1.2.3B").has_value());
    CHECK(!parse_size("1e3B").has_value());
    CHECK(!parse_size("1X").has_value());
    CHECK(!parse_size("1 K").has_value());
    CHECK(!parse_size("nonsense").has_value());
}

void test_parse_duration() {
    group("parse_duration");

    CHECK_EQ(parse_duration("1s").value().count(), 1);
    CHECK_EQ(parse_duration("0s").value().count(), 0);
    CHECK_EQ(parse_duration("90m").value().count(), 5400);
    CHECK_EQ(parse_duration("2h").value().count(), 7200);
    CHECK_EQ(parse_duration("1d").value().count(), 86400);
    CHECK_EQ(parse_duration("1w").value().count(), 604800);
    CHECK_EQ(parse_duration("0.5d").value().count(), 43200);
    CHECK_EQ(parse_duration("1.5h").value().count(), 5400);

    const auto maximum = std::numeric_limits<std::int64_t>::max();
    CHECK_EQ(parse_duration("9223372036854775807s").value().count(), maximum);
    CHECK(!parse_duration("9223372036854775808s").has_value());
    CHECK(!parse_duration("9223372036854775807w").has_value());

    // The unit has to be the only thing left, or "1dd" reads as one day.
    CHECK(!parse_duration("1dd").has_value());
    CHECK(!parse_duration("1d5d").has_value());
    CHECK(!parse_duration("1").has_value());
    CHECK(!parse_duration("d").has_value());
    CHECK(!parse_duration("-1d").has_value());
    CHECK(!parse_duration("1e3d").has_value());
    CHECK(!parse_duration("1.2.3d").has_value());
    CHECK(!parse_duration("").has_value());
    CHECK(!parse_duration("1y").has_value());
}

// now - limit is not representable for a large --older-than: the filesystem
// clock counts nanoseconds in 64 bits.
void test_age_comparison() {
    group("is_older_than");

    const auto now = fs::file_time_type::clock::now();
    const auto hour = std::chrono::seconds(3600);

    CHECK(is_older_than(now - std::chrono::hours(2), now, hour));
    CHECK(!is_older_than(now - std::chrono::minutes(10), now, hour));
    CHECK(is_older_than(now, now, std::chrono::seconds(0)));

    // Nothing is old enough, and nothing overflows on the way to saying so.
    const auto forever =
        std::chrono::seconds(std::numeric_limits<std::int64_t>::max());
    CHECK(!is_older_than(now - std::chrono::hours(24 * 365), now, forever));
}

// ---------------------------------------------------------- output escaping

// A POSIX filename is a byte string. The matched list is what the user reads
// before confirming a permanent deletion, so a name must not be able to forge
// a line in it or erase one that was already printed.
void test_display_escaping() {
    group("display");

    CHECK_EQ(display(std::string("plain.pyc")), std::string("plain.pyc"));
    CHECK_EQ(display(std::string("two\nlines")), std::string("two\\x0alines"));
    CHECK_EQ(display(std::string("ret\rurn")), std::string("ret\\x0durn"));
    CHECK_EQ(display(std::string("\033[2K")), std::string("\\x1b[2K"));
    CHECK_EQ(display(std::string("\x7f")), std::string("\\x7f"));
    CHECK_EQ(display(std::string("back\\slash")),
             std::string("back\\\\slash"));

    // Ordinary non-ASCII names are left alone: a terminal does not act on
    // them, and mangling them would make the list harder to read, not safer.
    CHECK_EQ(display(std::string("h\xc3\xa9llo")),
             std::string("h\xc3\xa9llo"));
}

// JSON is defined over text, so a single undecodable byte in one name used to
// make the whole document unparseable, totals and warnings included.
void test_json_string() {
    group("json_string");

    CHECK_EQ(json_string("plain"), std::string("\"plain\""));
    CHECK_EQ(json_string("a\"b"), std::string("\"a\\\"b\""));
    CHECK_EQ(json_string("a\\b"), std::string("\"a\\\\b\""));
    CHECK_EQ(json_string("a\nb"), std::string("\"a\\nb\""));
    CHECK_EQ(json_string("a\tb"), std::string("\"a\\tb\""));
    CHECK_EQ(json_string(std::string("\x01")), std::string("\"\\u0001\""));
    CHECK_EQ(json_string(std::string("\x7f")), std::string("\"\\u007f\""));

    // Well-formed UTF-8 passes through untouched, at every sequence length.
    CHECK_EQ(json_string("h\xc3\xa9llo"), std::string("\"h\xc3\xa9llo\""));
    CHECK_EQ(json_string("\xe2\x82\xac"), std::string("\"\xe2\x82\xac\""));
    CHECK_EQ(json_string("\xf0\x9f\x92\xa9"),
             std::string("\"\xf0\x9f\x92\xa9\""));

    // One replacement character per undecodable byte.
    const std::string replacement = "\xef\xbf\xbd";
    CHECK_EQ(json_string("\xff"), "\"" + replacement + "\"");
    CHECK_EQ(json_string("a\xff" "b"), "\"a" + replacement + "b\"");
    CHECK_EQ(json_string("\xc3"), "\"" + replacement + "\"");
    CHECK_EQ(json_string("\xc3\x28"), "\"" + replacement + "(\"");
    // Overlong, surrogate, and out-of-range forms are not valid UTF-8.
    CHECK_EQ(json_string("\xc0\xaf"), "\"" + replacement + replacement + "\"");
    CHECK_EQ(json_string("\xed\xa0\x80"),
             "\"" + replacement + replacement + replacement + "\"");
    CHECK_EQ(json_string("\xf5\x80\x80\x80"),
             "\"" + replacement + replacement + replacement + replacement +
             "\"");
}

// ---------------------------------------------------------- config parsing

// Skipping every comma before the next value accepted [, "a"] and ["a",,"b"],
// so a typo in a committed configuration changed what a run deleted in
// silence. TOML has no empty array element.
void test_string_array_parsing() {
    group("parse_string_array");

    std::vector<std::string> values;

    CHECK(parse_string_array("[]", values));
    CHECK_EQ(values.size(), 0u);

    values.clear();
    CHECK(parse_string_array("[\"*.tmp\"]", values));
    CHECK_EQ(values.size(), 1u);
    CHECK_EQ(values[0], std::string("*.tmp"));

    values.clear();
    CHECK(parse_string_array("  [ \"a\" ,  \"b\" ]  ", values));
    CHECK_EQ(values.size(), 2u);
    CHECK_EQ(values[1], std::string("b"));

    // TOML allows one trailing comma, and nothing else.
    values.clear();
    CHECK(parse_string_array("[\"a\",]", values));
    CHECK_EQ(values.size(), 1u);

    values.clear();
    CHECK(!parse_string_array("[, \"a\"]", values));
    CHECK(!parse_string_array("[\"a\",,\"b\"]", values));
    CHECK(!parse_string_array("[\"a\" \"b\"]", values));
    CHECK(!parse_string_array("[\"a\",,]", values));
    CHECK(!parse_string_array("[\"unclosed]", values));
    CHECK(!parse_string_array("[a]", values));
    CHECK(!parse_string_array("[\"a\\\\b\"]", values));
    CHECK(!parse_string_array("\"a\"", values));
}

void test_pair_array_parsing() {
    group("parse_pair_array");

    std::vector<std::pair<std::string, std::string>> pairs;

    CHECK(parse_pair_array("[]", pairs));
    CHECK_EQ(pairs.size(), 0u);

    pairs.clear();
    CHECK(parse_pair_array("[[\"deps\", \"mix.exs\"]]", pairs));
    CHECK_EQ(pairs.size(), 1u);
    CHECK_EQ(pairs[0].first, std::string("deps"));
    CHECK_EQ(pairs[0].second, std::string("mix.exs"));

    pairs.clear();
    CHECK(parse_pair_array("[[\"a\",\"b\"], [\"c\",\"d\"],]", pairs));
    CHECK_EQ(pairs.size(), 2u);

    // The outer parser used to look for the next ] textually, which rejected
    // a perfectly ordinary POSIX name that contains one.
    pairs.clear();
    CHECK(parse_pair_array("[[\"vendor\", \"we]ird.lock\"]]", pairs));
    CHECK_EQ(pairs.size(), 1u);
    CHECK_EQ(pairs[0].second, std::string("we]ird.lock"));

    pairs.clear();
    CHECK(!parse_pair_array("[[\"a\",\"b\"] [\"c\",\"d\"]]", pairs));
    CHECK(!parse_pair_array("[, [\"a\",\"b\"]]", pairs));
    CHECK(!parse_pair_array("[[\"a\"]]", pairs));
    CHECK(!parse_pair_array("[[\"a\",\"b\",\"c\"]]", pairs));
    // A marker is looked up beside the directory, so it has to be a plain
    // name: a separator would reach outside it.
    CHECK(!parse_pair_array("[[\"a\",\"../b\"]]", pairs));
    CHECK(!parse_pair_array("[[\"a\",\"b/c\"]]", pairs));
    CHECK(!parse_pair_array("[[\"a\",\"\"]]", pairs));
    CHECK(!parse_pair_array("[[\"a\",\"b\"]", pairs));
}

// ------------------------------------------------- parallel_directories

// A scan that throws must not strand the worker count or reach a thread
// boundary. The exception is expected to come back out of the call, and the
// call is expected to return at all: before this was handled, a throw left
// `active` raised and the remaining workers waiting on it forever.
void test_parallel_scan_propagates_exceptions() {
    group("parallel_directories");

    // Enough work that the throw lands while other workers are still running.
    std::vector<int> queue;
    for (int i = 0; i < 64; ++i) {
        queue.push_back(i);
    }

    std::atomic<int> scanned{0};
    bool threw = false;

    try {
        parallel_directories(queue, [&](int job, std::vector<int>& children) {
            ++scanned;
            if (job == 7) {
                throw std::runtime_error("scan failed");
            }
            // Two levels of children, so the queue is not merely drained.
            if (job < 8) {
                children.push_back(100 + job);
            }
        });
    } catch (const std::runtime_error& error) {
        threw = true;
        CHECK(std::string(error.what()) == "scan failed");
    }

    CHECK(threw);
    // The walk is abandoned rather than run to completion.
    CHECK(scanned.load() <= 64 + 8);

    // A scan that does not throw still visits everything and still returns.
    std::atomic<int> total{0};
    parallel_directories(queue, [&](int, std::vector<int>&) { ++total; });
    CHECK_EQ(total.load(), 64);
}

}  // namespace

int main() {
    test_literal_patterns();
    test_suffix_patterns();
    test_prefix_patterns();
    test_single_character_wildcard();
    test_directory_wildcard();
    test_star_spans_slash();
    test_metacharacters_are_literal();
    test_degenerate_patterns();
    test_matching_is_bounded();
    test_default_patterns();
    test_defaults_are_wired_up();
    test_skip_list();
    test_pattern_scope();
    test_exclude_scope();
    test_format_size();
    test_saturating_add();
    test_artifact_detection();
    test_scan_reasons();
    test_parse_size();
    test_parse_duration();
    test_age_comparison();
    test_display_escaping();
    test_json_string();
    test_string_array_parsing();
    test_pair_array_parsing();
    test_parallel_scan_propagates_exceptions();

    if (g_failures == 0) {
        std::printf("unit: %d checks passed\n", g_checks);
        return 0;
    }

    std::printf("unit: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
