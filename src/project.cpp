#include "cclean/project.hpp"

#include <algorithm>
#include <system_error>

#include "cclean/defaults.hpp"

namespace cclean {

fs::path normalize_directory(const fs::path& directory) {
    const fs::path normalized = directory.lexically_normal();

    if (normalized.filename().empty() && normalized.has_parent_path()) {
        return normalized.parent_path();
    }

    return normalized;
}

bool has_marker_file(const fs::path& directory, std::string_view name) {
    std::error_code ec;
    const fs::file_status status =
        fs::symlink_status(directory / std::string(name), ec);
    return !ec && status.type() == fs::file_type::regular;
}

bool has_repository(const fs::path& directory) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(directory / ".git", ec);
    return !ec && (status.type() == fs::file_type::directory ||
                   status.type() == fs::file_type::regular);
}

bool has_enclosing_project(fs::path project, const fs::path& root) {
    const fs::path stop = root.lexically_normal();
    project = project.lexically_normal();

    while (project != stop) {
        const fs::path parent = project.parent_path();

        if (parent.empty() || parent == project) {
            // Ran out of path before reaching ROOT.
            return false;
        }

        project = parent;

        if (has_repository(project)) {
            return true;
        }
    }

    return false;
}

bool is_artifact_directory(
    const fs::path& directory,
    const std::string& name,
    const fs::path& root,
    const std::vector<fs::path>& project_roots)
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
    const fs::path normalized_root = normalize_directory(fs::absolute(root));

    bool configured_project = false;
    for (const fs::path& project_root : project_roots) {
        if (project == project_root) {
            configured_project = true;
            break;
        }
    }

    if (!configured_project && !has_repository(project)) {
        return false;
    }

    // The artifact must sit at the top level of the outermost project.
    if (!configured_project && has_enclosing_project(project, normalized_root)) {
        return false;
    }

    for (const defaults::Artifact& artifact : defaults::artifacts) {
        if (artifact.directory == name &&
            has_marker_file(project, artifact.marker)) {
            return true;
        }
    }

    return false;
}

bool is_dependency_directory(
    const fs::path& directory,
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& configured)
{
    for (const defaults::Artifact& dependency : defaults::dependencies) {
        if (dependency.directory == name &&
            has_marker_file(directory.parent_path(), dependency.marker)) {
            return true;
        }
    }

    for (const auto& dependency : configured) {
        if (dependency.first == name &&
            has_marker_file(directory.parent_path(), dependency.second)) {
            return true;
        }
    }

    return false;
}

bool is_skipped(const std::string& name) {
    return std::find(defaults::skipped.begin(),
                     defaults::skipped.end(),
                     name) != defaults::skipped.end();
}

}  // namespace cclean
