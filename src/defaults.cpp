#include "cclean/defaults.hpp"

#include <algorithm>
#include <vector>

namespace cclean {

std::string builtin_patterns() {
    std::string list;

    for (const std::string_view pattern : defaults::patterns) {
        if (!list.empty()) {
            list += "  ";
        }
        list += pattern;
    }

    return list;
}

std::string artifact_directories() {
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

std::string skipped_directories() {
    std::string list;

    for (const std::string_view name : defaults::skipped) {
        if (!list.empty()) {
            list += "  ";
        }
        list += name;
    }

    return list;
}

}  // namespace cclean
