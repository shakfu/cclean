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

// Writes one JSON object: root, status, targets, total, per-reason stats,
// removed, failed, warnings. Status reports the action, so a scan warning does
// not disguise a dry run as a failed removal.
//
// Paths are escaped as JSON text, so a name that is not valid UTF-8 reaches
// the document lossily. The human output remains the faithful form.
void write_json(
    std::ostream& out,
    const fs::path& root,
    const ScanResult& result,
    const Outcome& outcome);

}  // namespace cclean

#endif  // CCLEAN_JSON_HPP
