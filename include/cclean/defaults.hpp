#ifndef CCLEAN_DEFAULTS_HPP
#define CCLEAN_DEFAULTS_HPP

#include <array>
#include <string>
#include <string_view>

namespace cclean {
namespace defaults {

// Built-in patterns, enabled unless --no-defaults is given. ".*_cache" covers
// the per-tool caches by shape, so .pytest_cache, .mypy_cache and .ruff_cache
// need no entries of their own; narrowing it would drop them.
inline constexpr std::array<std::string_view, 5> patterns = {
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

inline constexpr std::array<Artifact, 25> artifacts = {{
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

// A dependency tree, the marker file that must sit beside it, and the command
// that puts it back. The restore command is part of the entry rather than of
// the documentation: it is the answer to the only question a user has when
// asked to confirm the removal of something the network has to rebuild, and
// keeping it here means a new row cannot be added without answering it.
struct Dependency {
    std::string_view directory;
    std::string_view marker;
    std::string_view restore;
};

// Three ecosystems, not every ecosystem. A wrong entry here deletes a tree that
// cannot be rebuilt, so the list stays where the restore command is known and
// the layout is conventional. Anything else is a pattern in .cclean.toml.
inline constexpr std::array<Dependency, 8> dependencies = {{
    // uv.lock rather than pyproject.toml: the lock file is what makes the
    // environment reproducible, and `uv sync` rebuilds it exactly. A bare
    // pyproject.toml is also satisfied by poetry, pdm, hatch and by a .venv
    // populated with pip, none of which restore to a known state.
    {".venv", "uv.lock", "uv sync"},
    // A lock file rather than package.json, which carries ranges: only the lock
    // pins a tree. One row per package manager, because they share the install
    // directory but not the lock name, and a name match on node_modules alone
    // would take trees no lock can rebuild.
    {"node_modules", "package-lock.json", "npm ci"},
    {"node_modules", "npm-shrinkwrap.json", "npm ci"},
    {"node_modules", "yarn.lock", "yarn install --immutable"},
    {"node_modules", "pnpm-lock.yaml", "pnpm install --frozen-lockfile"},
    {"node_modules", "bun.lock", "bun install --frozen-lockfile"},
    {"node_modules", "bun.lockb", "bun install --frozen-lockfile"},
    // go.mod pins versions on its own: minimal version selection is
    // deterministic, so it is the lock file as well as the manifest.
    {"vendor", "go.mod", "go mod vendor"},
}};

// Never matched and never descended into. Their contents are state managed
// by another tool, where a name match is far likelier to be a false positive
// than a cache, and a wrong deletion costs history, credentials, or a working
// environment. --no-skip walks them anyway.
inline constexpr std::array<std::string_view, 6> skipped = {
    ".git",
    ".hg",
    ".svn",
    ".config",
    ".ssh",
    ".gnupg"
};

}  // namespace defaults

// Renderings of the tables above, for a frontend's help text. Each returns a
// ready-to-print list; artifact_directories() wraps at 68 columns.
std::string builtin_patterns();
std::string artifact_directories();
std::string skipped_directories();

}  // namespace cclean

#endif  // CCLEAN_DEFAULTS_HPP
