#include "cclean/config.hpp"

#include <fstream>
#include <set>
#include <system_error>

#include "toml.hpp"

namespace cclean {

namespace {

std::string where(const fs::path& path, std::size_t line) {
    return path.string() + ":" + std::to_string(line) + ": ";
}

}  // namespace

bool load_config(const fs::path& path, Config& config, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "Cannot read config file: " + path.string();
        return false;
    }

    std::string line;
    std::size_t line_number = 0;
    std::set<std::string> seen_keys;

    while (std::getline(input, line)) {
        ++line_number;

        // Only a # outside quotes opens a comment. Truncating at the first one
        // anywhere rejected patterns like "#*#", which name real files: emacs
        // lock and autosave entries are the common case. Quotes toggle rather
        // than nest, which is exact here because the string parsers reject
        // backslashes.
        bool quoted = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') {
                quoted = !quoted;
            } else if (line[i] == '#' && !quoted) {
                line.erase(i);
                break;
            }
        }

        line = toml::trim(line);
        if (line.empty()) {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            error = where(path, line_number) + "expected key = value";
            return false;
        }

        const std::string key = toml::trim(line.substr(0, equals));
        const std::string value = toml::trim(line.substr(equals + 1));

        // Every key here is a scalar or a whole array, so a second one is
        // always a mistake. Without this the values were quietly appended,
        // and a key repeated by a bad merge widened what a run deleted with
        // nothing to show for it. TOML rejects a duplicate key outright.
        if (!seen_keys.insert(key).second) {
            error = where(path, line_number) + "duplicate key " + key;
            return false;
        }

        if (key == "patterns") {
            if (!toml::parse_string_array(value, config.patterns)) {
                error = where(path, line_number) +
                        "patterns must be an array of strings";
                return false;
            }
        } else if (key == "excludes") {
            if (!toml::parse_string_array(value, config.excludes)) {
                error = where(path, line_number) +
                        "excludes must be an array of strings";
                return false;
            }
        } else if (key == "defaults") {
            if (!toml::parse_bool(value, config.defaults)) {
                error = where(path, line_number) +
                        "defaults must be true or false";
                return false;
            }
        } else if (key == "build_artifacts") {
            if (!toml::parse_bool(value, config.build_artifacts)) {
                error = where(path, line_number) +
                        "build_artifacts must be true or false";
                return false;
            }
        } else if (key == "dependencies") {
            if (!toml::parse_bool(value, config.dependencies)) {
                error = where(path, line_number) +
                        "dependencies must be true or false";
                return false;
            }
        } else if (key == "skip_protected") {
            if (!toml::parse_bool(value, config.skip_protected)) {
                error = where(path, line_number) +
                        "skip_protected must be true or false";
                return false;
            }
        } else if (key == "dependency_markers") {
            if (!toml::parse_pair_array(value, config.dependency_markers)) {
                error = where(path, line_number) +
                        "dependency_markers must be an array of "
                        "[directory, marker] string pairs, each a plain name";
                return false;
            }
        } else if (key == "project_roots") {
            if (!toml::parse_string_array(value, config.project_roots)) {
                error = where(path, line_number) +
                        "project_roots must be an array of strings";
                return false;
            }
        } else if (key == "older_than") {
            if (!toml::parse_string(value, config.older_than)) {
                error = where(path, line_number) +
                        "older_than must be a quoted duration";
                return false;
            }
        } else if (key == "larger_than") {
            if (!toml::parse_string(value, config.larger_than)) {
                error = where(path, line_number) +
                        "larger_than must be a quoted size";
                return false;
            }
        } else {
            error = where(path, line_number) + "unknown key " + key;
            return false;
        }
    }

    return true;
}

fs::path find_config(const fs::path& root) {
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

}  // namespace cclean
