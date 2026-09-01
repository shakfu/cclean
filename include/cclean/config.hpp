#ifndef CCLEAN_CONFIG_HPP
#define CCLEAN_CONFIG_HPP

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace cclean {

namespace fs = std::filesystem;

// The contents of a .cclean.toml, before any command-line override is applied.
// The defaults here are the defaults of a run with no config file at all.
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

// Reads `path` into `config`. On failure `error` carries a
// "file:line: message" diagnostic and nothing is guaranteed about `config`.
bool load_config(const fs::path& path, Config& config, std::string& error);

// The nearest .cclean.toml at or above `root`, or an empty path. Entries that
// name a directory are resolved against the file's own directory, not ROOT,
// so a repository-level config serves every subdirectory.
fs::path find_config(const fs::path& root);

}  // namespace cclean

#endif  // CCLEAN_CONFIG_HPP
