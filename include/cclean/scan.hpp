#ifndef CCLEAN_SCAN_HPP
#define CCLEAN_SCAN_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cclean/glob.hpp"
#include "cclean/target.hpp"

namespace cclean {

namespace fs = std::filesystem;

// Everything a walk needs. Compile the two pattern vectors with
// compile_patterns() and compile_excludes() rather than filling them by hand:
// the scope and reason each source gets are part of the matching contract.
struct ScanOptions {
    std::vector<Glob> patterns;
    std::vector<Glob> excludes;

    bool skip_protected = true;
    bool build_artifacts = false;
    bool dependencies = false;

    std::vector<std::pair<std::string, std::string>> dependency_markers;
    std::vector<fs::path> project_roots;

    // Applied after sizing, since both filters read a value the walk itself
    // does not produce.
    std::optional<std::chrono::seconds> older_than;
    std::optional<std::uintmax_t> larger_than;
};

struct ScanResult {
    // The root the walk was given, verbatim. Every target's path is built from
    // it, and removal resolves a target by descending from it one component at
    // a time, so the two have to be the same form: keeping it here is what
    // stops a caller from pairing a result with the wrong root.
    fs::path root;

    // Sorted by path. Directory iteration order is unspecified, and this list
    // is what a user reviews before confirming a permanent deletion.
    std::vector<Target> targets;

    // Paths that could not be read or inspected. Non-empty means the run was
    // incomplete; it does not mean the targets found are wrong.
    std::vector<std::string> warnings;
};

// Reports progress during the walk. `phase` is "scanning" or "sizing" and
// `done` counts entries or directories respectively. Called from worker
// threads, but serialised, so an implementation needs no locking of its own.
// It is called on every unit of work: throttling is the caller's to decide.
using ProgressFn = std::function<void(const char* phase, std::uintmax_t done)>;

// Built-ins first, then the config file, then the command line. The order is
// what decides the reason reported for a name that several sources match.
//
// The built-ins all name a file or directory, so they are matched against the
// filename only; matching them against the relative path would only add false
// positives -- ".*_cache" would read as "anything under a dot-directory ending
// in _cache", and take ".venv/lib/foo_cache" with it.
std::vector<Glob> compile_patterns(
    bool use_defaults,
    const std::vector<std::string>& config,
    const std::vector<std::string>& command_line);

// Excludes are ordinary globs with the same scope as a command-line pattern.
std::vector<Glob> compile_excludes(const std::vector<std::string>& patterns);

// Walks `root`, sizes every matched directory, applies the filters, and sorts
// what is left. A matched directory is a single target and is not descended
// into, which keeps the contents of a matched __pycache__ out of the listing.
//
// Symlinks are never followed and are never sized: the link itself is the
// object, and its age comes from the link rather than from its target.
ScanResult scan(
    const fs::path& root,
    const ScanOptions& options,
    const ProgressFn& progress = {});

}  // namespace cclean

#endif  // CCLEAN_SCAN_HPP
