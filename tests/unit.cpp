// Unit tests for the internals of cclean.
//
// cclean is a single translation unit with no header, so the tests include the
// source directly and rename its entry point out of the way. That keeps the
// program a single file while still reaching the static functions.

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#define main cclean_main
#include "../src/cclean.cpp"
#undef main

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

    if (g_failures == 0) {
        std::printf("unit: %d checks passed\n", g_checks);
        return 0;
    }

    std::printf("unit: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
