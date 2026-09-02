#ifndef CCLEAN_TARGET_HPP
#define CCLEAN_TARGET_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace cclean {

namespace fs = std::filesystem;

// Why a target was matched. Reported verbatim by the JSON writer, so the
// names below are part of the output contract.
enum class Reason {
    Default,
    Config,
    CommandLine,
    BuildArtifact,
    Dependency
};

inline const char* reason_name(Reason reason) {
    switch (reason) {
    case Reason::Default:       return "default";
    case Reason::Config:        return "config";
    case Reason::CommandLine:   return "command-line";
    case Reason::BuildArtifact: return "build-artifact";
    case Reason::Dependency:    return "dependency";
    }

    return "unknown";
}

// One entry the scan decided to offer for removal. `size` is the logical size
// of a directory's contents, filled in after the walk; a symlink is the object
// itself and counts as zero bytes.
struct Target {
    fs::path path;
    std::uintmax_t size = 0;
    bool is_directory = false;
    bool is_symlink = false;

    // For a dependency target, the command that puts the tree back -- the one
    // thing a user needs before confirming the removal of something only the
    // network can rebuild. Empty for every other reason, and for a dependency
    // configured through dependency_markers.
    std::string restore;
    Reason reason = Reason::Default;
    fs::file_time_type newest_time{};
    bool has_time = false;
};

}  // namespace cclean

#endif  // CCLEAN_TARGET_HPP
