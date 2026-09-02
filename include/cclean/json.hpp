#ifndef CCLEAN_JSON_HPP
#define CCLEAN_JSON_HPP

#include <cstddef>
#include <filesystem>
#include <ostream>

#include "cclean/scan.hpp"

namespace cclean {

namespace fs = std::filesystem;

// What the run did with the scan result, which decides the reported status.
struct Outcome {
    bool dry_run = false;
    bool cancelled = false;
    std::size_t removed = 0;
    std::size_t failed = 0;
};

// The version of the document shape below, reported as "schema". A consumer
// can refuse a document it does not know rather than read a renamed field as a
// missing one. It changes when a field changes meaning or leaves, not when one
// is added.
inline constexpr int json_schema_version = 1;

// Writes one JSON object: schema, root, config, status, targets, total,
// per-reason stats, removed, failed, warnings. Status reports the action, so a
// scan warning does not disguise a dry run as a failed removal.
//
// `config` is the configuration file the run used, or null when it used none.
//
// A target's "type" is "directory", "file" or "symlink". The third is not a
// refinement of the second: a matched symlink is unlinked without following
// it, and counts as zero bytes because removing it frees no contents, so a
// consumer summing bytes or re-checking a path before acting on it needs to
// tell the two apart.
//
// Paths are escaped as JSON text, so a name that is not valid UTF-8 reaches
// the document lossily. The human output remains the faithful form.
void write_json(
    std::ostream& out,
    const ScanResult& result,
    const Outcome& outcome,
    const fs::path& config = {});

}  // namespace cclean

#endif  // CCLEAN_JSON_HPP
