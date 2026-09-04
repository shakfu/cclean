#include "cclean/scan.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <optional>
#include <system_error>

#include <sys/stat.h>

#include "cclean/defaults.hpp"
#include "cclean/filters.hpp"
#include "cclean/project.hpp"
#include "cclean/text.hpp"
#include "parallel.hpp"

namespace cclean {

namespace {

// directory_entry::last_write_time() resolves the link before reading the
// timestamp, so --older-than judged a symlink by its target -- a link whose own
// mtime is years old was kept out of the list because the file it points at was
// touched today, and that file can sit outside the scanned tree entirely. The
// rest of the scanner treats the link itself as the object and reports it as
// zero bytes, so the age has to come from the link too.
//
// C++17 has no no-follow timestamp query and no conversion from time_t into the
// filesystem clock, so lstat supplies the former and the offset between the two
// clocks, sampled once, supplies the latter. The two samples are taken
// microseconds apart and --older-than resolves to whole seconds.
fs::file_time_type from_unix_seconds(std::int64_t seconds) {
    using Duration = fs::file_time_type::duration;

    static const Duration offset = [] {
        const auto file_now = fs::file_time_type::clock::now();
        const auto system_now = std::chrono::system_clock::now();
        return file_now.time_since_epoch() -
               std::chrono::duration_cast<Duration>(
                   system_now.time_since_epoch());
    }();

    return fs::file_time_type(
        offset + std::chrono::duration_cast<Duration>(
                     std::chrono::seconds(seconds)));
}

std::uintmax_t file_size_or_zero(
    const fs::directory_entry& entry,
    std::vector<std::string>& errors)
{
    std::error_code ec;
    const auto size = entry.file_size(ec);

    if (ec) {
        errors.push_back("Cannot determine size of " +
                         entry.path().string() + ": " + ec.message());
        return 0;
    }

    return size;
}

// Which thread reported which error is not reproducible, so the warnings are
// ordered before they are shown rather than left to vary between runs.
void merge_errors(
    std::vector<std::string>& found,
    std::vector<std::string>& errors)
{
    std::sort(found.begin(), found.end());
    errors.insert(errors.end(),
                  std::make_move_iterator(found.begin()),
                  std::make_move_iterator(found.end()));
}

void report(const ProgressFn& progress, const char* phase,
            std::uintmax_t done) {
    if (progress) {
        progress(phase, done);
    }
}

// Walks the tree and collects everything that matches. Listing a directory is a
// syscall the thread spends its time waiting on, so the levels are spread
// across cores the same way sizing is. A matched directory is a single deletion
// target and is not descended into.
void scan_tree(
    const fs::path& root,
    const ScanOptions& options,
    ScanResult& result,
    const ProgressFn& progress)
{
    std::mutex mutex;
    std::vector<std::string> found;
    std::uintmax_t seen = 0;

    std::vector<fs::path> queue;
    queue.push_back(root);

    parallel_directories(
        std::move(queue),
        [&](const fs::path& directory, std::vector<fs::path>& children) {
            std::vector<Target> local_targets;
            std::vector<std::string> local;
            std::uintmax_t local_seen = 0;

            std::error_code ec;
            fs::directory_iterator it(
                directory, fs::directory_options::none, ec);

            const fs::directory_iterator end;

            for (; !ec && it != end; it.increment(ec)) {
                if (ec) {
                    local.push_back("Error scanning " + directory.string() +
                                    ": " + ec.message());
                    ec.clear();
                    continue;
                }

                ++local_seen;

                const fs::directory_entry& entry = *it;
                const fs::path& path = entry.path();

                std::error_code status_ec;
                const bool is_symlink = entry.is_symlink(status_ec);

                if (status_ec) {
                    local.push_back("Cannot inspect " + path.string() + ": " +
                                    status_ec.message());
                    continue;
                }

                const std::string filename = path.filename().generic_string();

                // Neither matched nor descended into: see defaults::skipped.
                if (options.skip_protected && is_skipped(filename)) {
                    continue;
                }

                // An exclude prunes rather than only suppressing the match, so
                // that naming a directory keeps everything under it. That is
                // what lets --exclude .venv restore protection the built-in
                // list no longer gives.
                if (!options.excludes.empty() &&
                    matches_any(path, root, filename, options.excludes)) {
                    continue;
                }

                const bool is_directory =
                    !is_symlink && entry.is_directory(status_ec);

                if (status_ec) {
                    local.push_back("Cannot inspect " + path.string() + ": " +
                                    status_ec.message());
                    continue;
                }

                const Glob* matched =
                    first_match(path, root, filename, options.patterns);
                const bool artifact = options.build_artifacts && is_directory &&
                    is_artifact_directory(path, filename, root,
                                          options.project_roots);
                std::string restore;
                const bool dependency = options.dependencies && is_directory &&
                    is_dependency_directory(path, filename,
                                            options.dependency_markers,
                                            &restore);

                if (matched == nullptr && !artifact && !dependency) {
                    // Symlinks are never followed, so only a real directory is
                    // worth queueing.
                    if (is_directory) {
                        children.push_back(path);
                    }

                    continue;
                }

                Target target;
                target.path = path;
                target.is_directory = is_directory;
                target.is_symlink = is_symlink;

                // The one no-follow stat this target needs, taken once. It
                // carries the identity removal re-checks after the user has
                // reviewed the list, and for a symlink it is also where the
                // timestamp comes from.
                struct stat info;
                const bool stated = ::lstat(path.c_str(), &info) == 0;

                if (stated) {
                    target.device = static_cast<std::uint64_t>(info.st_dev);
                    target.inode = static_cast<std::uint64_t>(info.st_ino);
                    target.has_identity = true;
                }

                // An unreadable mtime is recorded, not reported. It is only
                // ever consulted by the age filter, which raises its own
                // "Cannot apply age filter" error for the same target; warning
                // here as well made every run without the filter exit 1 over a
                // value it never read.
                if (is_symlink) {
                    target.has_time = stated;

                    if (stated) {
                        target.newest_time = from_unix_seconds(
                            static_cast<std::int64_t>(info.st_mtime));
                    }
                } else {
                    std::error_code time_ec;
                    target.newest_time = entry.last_write_time(time_ec);
                    target.has_time = !time_ec;
                }

                if (artifact) {
                    target.reason = Reason::BuildArtifact;
                } else if (dependency) {
                    target.reason = Reason::Dependency;
                    target.restore = std::move(restore);
                } else {
                    target.reason = matched->reason();
                }

                // A matched directory is sized later, in one pass over all of
                // them. Symlinks and other special files have no ordinary size.
                target.size = 0;

                if (!is_directory && !is_symlink) {
                    const bool is_file = entry.is_regular_file(status_ec);

                    if (status_ec) {
                        // The target is still listed, at an unknown size,
                        // rather than dropped: it matched a pattern, and hiding
                        // it would be the more surprising outcome.
                        local.push_back("Cannot inspect " + path.string() +
                                        ": " + status_ec.message());
                    } else if (is_file) {
                        target.size = file_size_or_zero(entry, local);
                    }
                }

                local_targets.push_back(std::move(target));
            }

            if (ec) {
                local.push_back("Error scanning " + directory.string() + ": " +
                                ec.message());
            }

            const std::lock_guard<std::mutex> guard(mutex);

            result.targets.insert(
                result.targets.end(),
                std::make_move_iterator(local_targets.begin()),
                std::make_move_iterator(local_targets.end()));
            found.insert(found.end(),
                         std::make_move_iterator(local.begin()),
                         std::make_move_iterator(local.end()));

            seen += local_seen;
            report(progress, "scanning", seen);
        });

    merge_errors(found, result.warnings);
}

// Calculates the logical size of every matched directory without following
// symlinks. Sizing is stat-bound and was 43% of a run over many small targets,
// 94% of a run over one large target. The unit of work is a single directory
// level rather than a whole target, so one target holding the entire tree
// parallelizes as well as many small ones.
void size_directories(
    ScanResult& result,
    const ProgressFn& progress)
{
    struct Job {
        fs::path directory;
        std::size_t target;
    };

    std::vector<Job> queue;

    for (std::size_t i = 0; i < result.targets.size(); ++i) {
        if (result.targets[i].is_directory) {
            queue.push_back({result.targets[i].path, i});
        }
    }

    std::mutex mutex;
    std::vector<std::string> found;
    std::uintmax_t sized = 0;

    parallel_directories(
        std::move(queue),
        [&](const Job& job, std::vector<Job>& children) {
            std::uintmax_t total = 0;
            std::vector<std::string> local;
            std::optional<fs::file_time_type> newest;

            std::error_code ec;
            fs::directory_iterator it(
                job.directory, fs::directory_options::none, ec);

            const fs::directory_iterator end;

            for (; !ec && it != end; it.increment(ec)) {
                if (ec) {
                    local.push_back("Error scanning " +
                                    job.directory.string() + ": " +
                                    ec.message());
                    ec.clear();
                    continue;
                }

                const fs::directory_entry& entry = *it;

                std::error_code status_ec;
                const bool entry_is_symlink = entry.is_symlink(status_ec);

                if (status_ec) {
                    local.push_back("Cannot inspect " +
                                    entry.path().string() + ": " +
                                    status_ec.message());
                    continue;
                }

                if (entry_is_symlink) {
                    // Symlinks contribute no file contents of their own, and
                    // are not descended into.
                    continue;
                }

                std::error_code time_ec;
                const fs::file_time_type entry_time =
                    entry.last_write_time(time_ec);
                if (time_ec) {
                    local.push_back("Cannot determine modification time of " +
                                    entry.path().string() + ": " +
                                    time_ec.message());
                } else if (!newest || entry_time > *newest) {
                    newest = entry_time;
                }

                // Not a symlink, so status() and symlink_status() agree here. A
                // failed query is reported rather than read as "not a
                // directory": a permission error or an entry disappearing
                // mid-walk would otherwise drop a whole subtree from the size
                // silently, and the documented contract is that an unreadable
                // path is named and the run exits 1.
                const bool child_is_directory = entry.is_directory(status_ec);

                if (status_ec) {
                    local.push_back("Cannot inspect " +
                                    entry.path().string() + ": " +
                                    status_ec.message());
                    continue;
                }

                if (child_is_directory) {
                    children.push_back({entry.path(), job.target});
                    continue;
                }

                const bool child_is_file = entry.is_regular_file(status_ec);

                if (status_ec) {
                    local.push_back("Cannot inspect " +
                                    entry.path().string() + ": " +
                                    status_ec.message());
                    continue;
                }

                if (child_is_file) {
                    total = saturating_add(total,
                                           file_size_or_zero(entry, local));
                }
            }

            if (ec) {
                local.push_back("Error scanning " + job.directory.string() +
                                ": " + ec.message());
            }

            const std::lock_guard<std::mutex> guard(mutex);

            Target& target = result.targets[job.target];
            target.size = saturating_add(target.size, total);
            if (newest && (!target.has_time || *newest > target.newest_time)) {
                target.newest_time = *newest;
                target.has_time = true;
            }

            found.insert(found.end(),
                         std::make_move_iterator(local.begin()),
                         std::make_move_iterator(local.end()));

            report(progress, "sizing", ++sized);
        });

    merge_errors(found, result.warnings);
}

// Both filters read a value the walk does not produce on its own, so they run
// once sizing is done. A target whose age cannot be read is dropped, with a
// warning: the filter was asked for and cannot be answered.
void apply_filters(ScanResult& result, const ScanOptions& options) {
    if (!options.older_than && !options.larger_than) {
        return;
    }

    const fs::file_time_type now = fs::file_time_type::clock::now();
    std::vector<Target> filtered;
    filtered.reserve(result.targets.size());

    for (Target& target : result.targets) {
        bool keep = true;

        if (options.older_than) {
            if (!target.has_time) {
                result.warnings.push_back("Cannot apply age filter to " +
                                          target.path.string());
                keep = false;
            } else if (!is_older_than(target.newest_time, now,
                                      *options.older_than)) {
                keep = false;
            }
        }

        if (keep && options.larger_than && target.size < *options.larger_than) {
            keep = false;
        }

        if (keep) {
            filtered.push_back(std::move(target));
        }
    }

    result.targets.swap(filtered);
}

}  // namespace

std::vector<Glob> compile_patterns(
    bool use_defaults,
    const std::vector<std::string>& config,
    const std::vector<std::string>& command_line)
{
    std::vector<Glob> patterns;
    patterns.reserve((use_defaults ? defaults::patterns.size() : 0) +
                     config.size() + command_line.size());

    if (use_defaults) {
        for (const std::string_view pattern : defaults::patterns) {
            patterns.emplace_back(std::string(pattern),
                                  Glob::Scope::NameOnly,
                                  Reason::Default);
        }
    }

    for (const std::string& pattern : config) {
        patterns.emplace_back(pattern, Glob::Scope::NameOrPath,
                              Reason::Config);
    }

    for (const std::string& pattern : command_line) {
        patterns.emplace_back(pattern, Glob::Scope::NameOrPath,
                              Reason::CommandLine);
    }

    return patterns;
}

std::vector<Glob> compile_excludes(const std::vector<std::string>& patterns) {
    std::vector<Glob> excludes;
    excludes.reserve(patterns.size());

    for (const std::string& pattern : patterns) {
        excludes.emplace_back(pattern, Glob::Scope::NameOrPath);
    }

    return excludes;
}

ScanResult scan(
    const fs::path& root,
    const ScanOptions& options,
    const ProgressFn& progress)
{
    ScanResult result;
    result.root = root;

    scan_tree(root, options, result, progress);
    size_directories(result, progress);
    apply_filters(result, options);

    // Directory iteration order is unspecified; this list is what the user
    // reviews before confirming a permanent deletion.
    std::sort(result.targets.begin(), result.targets.end(),
              [](const Target& a, const Target& b) {
                  return a.path < b.path;
              });

    return result;
}

}  // namespace cclean
