// Command-line frontend. Everything that finds, sizes and removes lives in
// libcclean; what is left here is argument parsing, the config-over-flag
// precedence, the confirmation, and the human-readable report.

#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "cclean/cclean.hpp"

#include "options.hpp"
#include "terminal.hpp"

using namespace cclean;
using namespace cclean::cli;

namespace {

// A command-line value wins over the config file; the config file wins over
// the built-in default. Whether the option appeared is what decides, not its
// value, since false and "" are settings a user can mean.
void apply_config(const Config& config, CommandLine& args) {
    if (!args.set_defaults) {
        args.use_defaults = config.defaults;
    }
    if (!args.set_build_artifacts) {
        args.build_artifacts = config.build_artifacts;
    }
    if (!args.set_dependencies) {
        args.dependencies = config.dependencies;
    }
    if (!args.set_skips) {
        args.use_skips = config.skip_protected;
    }
    if (!args.set_excludes) {
        args.excludes = config.excludes;
    }
    if (!args.set_older_than) {
        args.older_than = config.older_than;
    }
    if (!args.set_larger_than) {
        args.larger_than = config.larger_than;
    }
}

// Anchored to the config file, not to ROOT. The config is found by searching
// upward, so a repository-level .cclean.toml is read for every ROOT beneath it;
// resolving its entries against ROOT instead made project_roots =
// ["packages/api"] fail with "not a directory" on any run from a subdirectory,
// including runs that never look at build artifacts. An entry naming a sibling
// of ROOT is not an error, it simply never matches, because the walk stays
// under ROOT.
bool resolve_project_roots(
    const Config& config,
    const fs::path& config_dir,
    std::vector<fs::path>& project_roots)
{
    for (const std::string& configured : config.project_roots) {
        const fs::path relative(configured);

        if (relative.is_absolute()) {
            std::cerr << "project_roots entries must be relative to "
                         ".cclean.toml: " << configured << '\n';
            return false;
        }

        const fs::path candidate = normalize_directory(config_dir / relative);
        const std::string candidate_relative =
            candidate.lexically_relative(config_dir).generic_string();

        if (candidate_relative == ".." ||
            candidate_relative.rfind("../", 0) == 0) {
            std::cerr << "project_roots entry is outside .cclean.toml's "
                         "directory: " << configured << '\n';
            return false;
        }

        std::error_code ec;
        if (!fs::is_directory(candidate, ec) || ec) {
            std::cerr << "project_roots entry is not a directory: "
                      << configured << '\n';
            return false;
        }

        project_roots.push_back(candidate);
    }

    return true;
}

void print_warnings(const std::vector<std::string>& warnings,
                    const Style& style) {
    if (warnings.empty()) {
        return;
    }

    std::cerr << '\n' << style.warning << "Warnings:" << style.reset << '\n';

    for (const std::string& warning : warnings) {
        std::cerr << "  " << display(warning) << '\n';
    }
}

void print_targets(const std::vector<Target>& targets,
                   std::uintmax_t total_size,
                   const Style& style) {
    std::cout << "\nMatched targets:\n";

    for (const Target& target : targets) {
        if (target.is_directory) {
            std::cout << "  " << style.directory << display(target.path) << '/'
                      << style.reset;
        } else {
            std::cout << "  " << display(target.path);
        }

        std::cout << style.dim << "  " << format_size(target.size)
                  << style.reset << '\n';
    }

    std::cout << '\n'
              << style.bold << targets.size() << style.reset
              << (targets.size() == 1 ? " target, " : " targets, ")
              << style.bold << format_size(total_size) << style.reset
              << " to reclaim\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    CommandLine args;
    int status = 0;

    if (!parse_command_line(argc, argv, args, status)) {
        return status;
    }

    const fs::path root(args.operands.front());

    Config config;
    const fs::path config_path = find_config(root);

    if (!config_path.empty()) {
        std::string config_error;

        if (!load_config(config_path, config, config_error)) {
            std::cerr << config_error << '\n';
            return 2;
        }
    }

    apply_config(config, args);

    const fs::path config_dir = config_path.empty()
        ? normalize_directory(fs::absolute(root))
        : normalize_directory(fs::absolute(config_path).parent_path());

    ScanOptions options;
    options.skip_protected = args.use_skips;
    options.build_artifacts = args.build_artifacts;
    options.dependencies = args.dependencies;
    options.dependency_markers = config.dependency_markers;

    if (!resolve_project_roots(config, config_dir, options.project_roots)) {
        return 2;
    }

    if (!args.older_than.empty()) {
        options.older_than = parse_duration(args.older_than);

        if (!options.older_than) {
            std::cerr << "Invalid --older-than duration: " << args.older_than
                      << " (use a number followed by s, m, h, d, or w)\n";
            return 2;
        }
    }

    if (!args.larger_than.empty()) {
        options.larger_than = parse_size(args.larger_than);

        if (!options.larger_than) {
            std::cerr << "Invalid --larger-than size: " << args.larger_than
                      << " (use B, K, M, G, or T)\n";
            return 2;
        }
    }

    const std::vector<std::string> cli_patterns(args.operands.begin() + 1,
                                                args.operands.end());

    // --build-artifacts and --dependencies match on project layout rather than
    // on a pattern, so either is on its own enough to give the run something
    // to do.
    if (!args.use_defaults && cli_patterns.empty() && config.patterns.empty() &&
        !args.build_artifacts && !args.dependencies) {
        // The defaults can be switched off by the config rather than by the
        // flag, so naming --no-defaults would point at an argument the user
        // never typed.
        std::cerr << (args.set_defaults ? "--no-defaults" : "defaults = false")
                  << " leaves no patterns to match; supply at least one, or "
                     "--build-artifacts, or --dependencies.\n";
        return 2;
    }

    options.patterns =
        compile_patterns(args.use_defaults, config.patterns, cli_patterns);
    options.excludes = compile_excludes(args.excludes);

    std::error_code ec;
    const fs::file_status root_status = fs::symlink_status(root, ec);

    if (ec) {
        std::cerr << "Cannot inspect root path: " << root.string() << ": "
                  << ec.message() << '\n';
        return 1;
    }

    if (!fs::is_directory(root_status)) {
        std::cerr << "Root path is not a directory: " << root.string() << '\n';
        return 1;
    }

    // Opened once here so that an unreadable ROOT is a hard failure, rather
    // than a warning raised by whichever worker happened to draw it.
    {
        fs::directory_iterator probe(root, fs::directory_options::none, ec);

        if (ec) {
            std::cerr << "Cannot scan root path: " << ec.message() << '\n';
            return 1;
        }
    }

    Progress progress(is_terminal(STDERR_FILENO));

    ScanResult result = scan(
        root, options,
        [&progress](const char* phase, std::uintmax_t done) {
            progress.update(phase, done);
        });

    progress.finish();

    const Style out = Style::detect(STDOUT_FILENO);
    const Style err = Style::detect(STDERR_FILENO);

    print_warnings(result.warnings, err);

    if (result.targets.empty()) {
        if (args.format_json) {
            write_json(std::cout, root, result, {args.dry_run, false, 0, 0});
        } else {
            std::cout << "No matching targets found.\n";
        }

        return result.warnings.empty() ? 0 : 1;
    }

    std::uintmax_t total_size = 0;

    for (const Target& target : result.targets) {
        total_size = saturating_add(total_size, target.size);
    }

    if (!args.format_json) {
        print_targets(result.targets, total_size, out);
    }

    if (args.dry_run) {
        if (args.format_json) {
            write_json(std::cout, root, result, {true, false, 0, 0});
        } else {
            std::cout << out.dim << "Dry run: nothing was removed."
                      << out.reset << '\n';
        }

        return result.warnings.empty() ? 0 : 1;
    }

    bool go_ahead = args.assume_yes;

    if (!args.assume_yes) {
        // The prompt goes wherever the report is not, so a JSON document on
        // stdout stays a JSON document.
        std::ostream& prompt = args.format_json ? std::cerr : std::cout;

        if (args.format_json) {
            prompt << "Permanently remove " << result.targets.size()
                   << " targets? [y/N] ";
        } else {
            prompt << "\nPermanently remove? " << out.bold << "[y/N]"
                   << out.reset << ' ';
        }

        prompt.flush();
        go_ahead = confirmed();
        prompt << (go_ahead ? "y" : "n") << '\n';
    }

    if (!go_ahead) {
        if (args.format_json) {
            write_json(std::cout, root, result, {false, true, 0, 0});
        } else {
            std::cout << "Cancelled.\n";
        }

        return result.warnings.empty() ? 0 : 1;
    }

    Outcome outcome;

    // The matched list was already shown, so only failures are named again.
    // --verbose restores the per-item log.
    for (const Target& target : result.targets) {
        std::string removal_error;

        if (!remove_target(target, removal_error)) {
            ++outcome.failed;
            result.warnings.push_back(removal_error);
            std::cerr << "  " << err.failure << "failed" << err.reset << "  "
                      << display(removal_error) << '\n';
        } else {
            ++outcome.removed;

            if (args.verbose && !args.format_json) {
                std::cout << "  " << out.dim << "removed" << out.reset << "  "
                          << display(target.path) << '\n';
            }
        }
    }

    if (args.format_json) {
        write_json(std::cout, root, result, outcome);
    } else if (outcome.failed == 0) {
        std::cout << out.success << "Removed " << outcome.removed << out.reset
                  << ", " << format_size(total_size) << " reclaimed\n";
    } else {
        std::cout << "Removed " << outcome.removed << ", "
                  << out.failure << outcome.failed << " failed" << out.reset
                  << '\n';
    }

    if (outcome.failed != 0) {
        return 1;
    }

    return result.warnings.empty() ? 0 : 1;
}
