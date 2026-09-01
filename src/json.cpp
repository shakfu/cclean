#include "cclean/json.hpp"

#include <map>
#include <string>
#include <utility>

#include "cclean/text.hpp"

namespace cclean {

void write_json(
    std::ostream& out,
    const fs::path& root,
    const ScanResult& result,
    const Outcome& outcome)
{
    std::uintmax_t total = 0;
    std::map<std::string, std::pair<std::size_t, std::uintmax_t>> stats;

    for (const Target& target : result.targets) {
        total = saturating_add(total, target.size);
        auto& stat = stats[reason_name(target.reason)];
        ++stat.first;
        stat.second = saturating_add(stat.second, target.size);
    }

    // Status reports the action, so a scan warning does not disguise a dry run
    // as a failed removal. Warnings are their own array, and still set exit 1.
    std::string status;
    if (outcome.failed != 0) {
        status = "failed";
    } else if (result.targets.empty()) {
        status = "empty";
    } else if (outcome.cancelled) {
        status = "cancelled";
    } else if (outcome.dry_run) {
        status = "dry-run";
    } else {
        status = "removed";
    }

    out << "{\n"
        << "  \"root\": " << json_string(root.string()) << ",\n"
        << "  \"status\": " << json_string(status) << ",\n"
        << "  \"targets\": [";

    for (std::size_t i = 0; i < result.targets.size(); ++i) {
        const Target& target = result.targets[i];
        if (i != 0) {
            out << ',';
        }
        out << "\n    {\"path\": " << json_string(target.path.string())
            << ", \"type\": "
            << json_string(target.is_directory ? "directory" : "file")
            << ", \"bytes\": " << target.size
            << ", \"reason\": " << json_string(reason_name(target.reason))
            << "}";
    }

    out << "\n  ],\n"
        << "  \"total\": {\"targets\": " << result.targets.size()
        << ", \"bytes\": " << total << "},\n"
        << "  \"stats\": {";

    std::size_t stat_index = 0;
    for (const auto& entry : stats) {
        if (stat_index++ != 0) {
            out << ',';
        }
        out << "\n    " << json_string(entry.first)
            << ": {\"targets\": " << entry.second.first
            << ", \"bytes\": " << entry.second.second << "}";
    }

    out << "\n  },\n"
        << "  \"removed\": " << outcome.removed << ",\n"
        << "  \"failed\": " << outcome.failed << ",\n"
        << "  \"warnings\": [";

    for (std::size_t i = 0; i < result.warnings.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << "\n    " << json_string(result.warnings[i]);
    }

    out << "\n  ]\n}\n";
}

}  // namespace cclean
