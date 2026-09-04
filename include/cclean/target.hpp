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

    // The identity of the entry itself, from a no-follow stat taken during the
    // scan: st_dev and st_ino, widened so that no POSIX type appears in a
    // public header. A run shows this list and then waits for the user, and in
    // that window an entry can be replaced by another of the same type --
    // deliberately, or by a build tool rewriting a cache directory atomically.
    // The type flags above cannot tell the replacement from the original, so
    // removal re-checks these instead and refuses a mismatch.
    //
    // `has_identity` is false only for a Target a caller built by hand rather
    // than took from scan(); such a target was never reviewed against a
    // displayed list, so removal falls back to checking the type alone.
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    bool has_identity = false;

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
