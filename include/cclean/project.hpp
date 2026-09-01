#ifndef CCLEAN_PROJECT_HPP
#define CCLEAN_PROJECT_HPP

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cclean {

namespace fs = std::filesystem;

// lexically_normal() keeps a trailing separator when the path ends in a dot
// component, so "/a/b/." becomes "/a/b/", which compares unequal to "/a/b".
// A ROOT of "." reaches here as exactly that, so directory comparison needs
// the separator gone.
fs::path normalize_directory(const fs::path& directory);

// A marker has to be a regular file, reached without following a link.
// exists() follows symlinks and is satisfied by anything at all, so a
// directory named CMakeLists.txt, a FIFO, or a link pointing at some unrelated
// file elsewhere would otherwise license the removal of a build directory in a
// tree that holds none of the project metadata the guard is proving.
bool has_marker_file(const fs::path& directory, std::string_view name);

// .git is the one marker that is normally a directory: a working tree carries
// a .git directory and a submodule or worktree carries a .git file pointing
// at the real one. Both are accepted, a symlink is not.
bool has_repository(const fs::path& directory);

// True when a directory between `project` and `root` is itself a repository.
// A submodule or a vendored checkout carries its own .git and marker file, so
// without this its build output matches even though it sits well below the top
// level of the project being cleaned.
bool has_enclosing_project(fs::path project, const fs::path& root);

// "build" and "target" are ordinary names, so they only count as artifacts
// beside the marker files of a project that generates one. The .git test is
// what keeps a stray directory called build out of the list; an entry in
// `project_roots` stands in for it.
bool is_artifact_directory(
    const fs::path& directory,
    const std::string& name,
    const fs::path& root,
    const std::vector<fs::path>& project_roots = {});

// Configured pairs take the same marker guard and the same --dependencies
// gate as the built-ins, which is what separates them from a pattern.
bool is_dependency_directory(
    const fs::path& directory,
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& configured = {});

// A member of defaults::skipped: never matched, never descended into.
bool is_skipped(const std::string& name);

}  // namespace cclean

#endif  // CCLEAN_PROJECT_HPP
