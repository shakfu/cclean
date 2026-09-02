#include "options.hpp"

#include <iostream>
#include <string>

#include "cclean/cclean.hpp"

namespace cclean::cli {

void print_usage(std::ostream& out, const char* program) {
    out
        << "Usage:\n"
        << "  " << program << " [OPTION ...] ROOT [PATTERN ...]\n\n"
        << "Built-in patterns, added to any given on the command line:\n"
        << "  " << builtin_patterns() << "\n\n"
        << "Options:\n"
        << "  -n, --dry-run  List matching targets and exit without removing\n"
        << "  -b, --build-artifacts\n"
        << "                 Also remove a project's build output, listed\n"
        << "                 below\n"
        << "  -d, --dependencies\n"
        << "                 Also remove marker-guarded dependency trees\n"
        << "  -e, --exclude PATTERN\n"
        << "                 Leave anything matching PATTERN alone, contents\n"
        << "                 included. Repeatable\n"
        << "  -v, --verbose  Name every item as it is removed\n"
        << "  -y, --yes      Remove without prompting\n"
        << "  --color WHEN   Colour output: auto (default), always, never\n"
        << "  --format FORMAT  Output human (default) or json\n"
        << "  --config FILE  Read this configuration file instead of\n"
        << "                 searching upward from ROOT\n"
        << "  --no-config    Read no configuration file at all\n"
        << "  --older-than DURATION\n"
        << "                 Match only targets older than DURATION\n"
        << "                 (s, m, h, d, w)\n"
        << "  --larger-than SIZE\n"
        << "                 Match only targets of at least SIZE\n"
        << "                 (B, K, M, G, T)\n"
        << "  -h, --help     Show this text\n"
        << "  -V, --version  Show the version and exit\n"
        << "  --no-defaults  Match only the patterns given on the command line\n"
        << "  --no-skip      Also descend into the skipped directories below\n"
        << "  --             Treat every later argument as ROOT or a pattern\n\n"
        << "Never matched or descended into, unless --no-skip is given:\n"
        << "  " << skipped_directories() << "\n\n"
        << "Removed by --build-artifacts, when .git and the marker file for\n"
        << "the ecosystem sit beside them:\n"
        << "  " << artifact_directories() << "\n\n"
        << "Examples:\n"
        << "  " << program << "                       (scans the working directory)\n"
        << "  " << program << " ./project\n"
        << "  " << program << " --dry-run ./project \"*.log\" \"*.tmp\"\n"
        << "  " << program << " --no-defaults . \"build/**\" \"**/*.o\"\n"
        << "  " << program << " --exclude .venv --exclude \"**/fixtures/**\"\n\n"
        << "Command-line patterns match either the path relative to ROOT or\n"
        << "the final filename; built-in patterns match the filename only.\n\n"
        << "Supported wildcards:\n"
        << "  *      Any sequence of characters\n"
        << "  ?      Any single character\n"
        << "  **/    Zero or more directory levels\n";
}

namespace {

bool parse_format(const std::string& value, bool& format_json) {
    if (value == "json") {
        format_json = true;
        return true;
    }

    if (value == "human") {
        format_json = false;
        return true;
    }

    std::cerr << "Unknown format: " << value << '\n';
    return false;
}

bool parse_color(const std::string& value, ColorWhen& color) {
    if (value == "auto") {
        color = ColorWhen::Auto;
        return true;
    }

    if (value == "always") {
        color = ColorWhen::Always;
        return true;
    }

    if (value == "never") {
        color = ColorWhen::Never;
        return true;
    }

    std::cerr << "Unknown colour setting: " << value
              << " (use auto, always, or never)\n";
    return false;
}

}  // namespace

bool parse_command_line(
    int argc,
    char* argv[],
    CommandLine& command_line,
    int& status)
{
    bool options_ended = false;
    status = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (!options_ended && argument == "--") {
            options_ended = true;
            continue;
        }

        // An unrecognised option is rejected rather than taken as a pattern,
        // so a mistyped --dry-run cannot fall through to a live removal.
        if (!options_ended && argument.size() > 1 && argument[0] == '-') {
            if (argument == "-n" || argument == "--dry-run") {
                command_line.dry_run = true;
                continue;
            }

            if (argument == "--no-defaults") {
                command_line.use_defaults = false;
                command_line.set_defaults = true;
                continue;
            }

            if (argument == "--no-skip") {
                command_line.use_skips = false;
                command_line.set_skips = true;
                continue;
            }

            if (argument == "-b" || argument == "--build-artifacts") {
                command_line.build_artifacts = true;
                command_line.set_build_artifacts = true;
                continue;
            }

            if (argument == "-d" || argument == "--dependencies") {
                command_line.dependencies = true;
                command_line.set_dependencies = true;
                continue;
            }

            if (argument == "-v" || argument == "--verbose") {
                command_line.verbose = true;
                continue;
            }

            if (argument == "-y" || argument == "--yes") {
                command_line.assume_yes = true;
                continue;
            }

            if (argument == "--older-than" || argument == "--larger-than") {
                if (i + 1 >= argc) {
                    std::cerr << argument << " needs a value.\n";
                    status = 2;
                    return false;
                }

                const bool age = argument == "--older-than";
                (age ? command_line.older_than
                     : command_line.larger_than) = argv[++i];
                (age ? command_line.set_older_than
                     : command_line.set_larger_than) = true;
                continue;
            }

            if (argument.rfind("--older-than=", 0) == 0) {
                command_line.older_than = argument.substr(13);
                command_line.set_older_than = true;
                continue;
            }

            if (argument.rfind("--larger-than=", 0) == 0) {
                command_line.larger_than = argument.substr(14);
                command_line.set_larger_than = true;
                continue;
            }

            if (argument == "--no-config") {
                command_line.no_config = true;
                continue;
            }

            if (argument == "--config") {
                if (i + 1 >= argc) {
                    std::cerr << argument << " needs a file.\n";
                    status = 2;
                    return false;
                }

                command_line.config_path = argv[++i];
                continue;
            }

            if (argument.rfind("--config=", 0) == 0) {
                command_line.config_path = argument.substr(9);
                continue;
            }

            if (argument == "--color") {
                if (i + 1 >= argc) {
                    std::cerr << argument << " needs a setting.\n";
                    status = 2;
                    return false;
                }

                if (!parse_color(argv[++i], command_line.color)) {
                    status = 2;
                    return false;
                }

                continue;
            }

            if (argument.rfind("--color=", 0) == 0) {
                if (!parse_color(argument.substr(8), command_line.color)) {
                    status = 2;
                    return false;
                }

                continue;
            }

            if (argument == "--format") {
                if (i + 1 >= argc) {
                    std::cerr << argument << " needs a format.\n";
                    status = 2;
                    return false;
                }

                if (!parse_format(argv[++i], command_line.format_json)) {
                    status = 2;
                    return false;
                }

                continue;
            }

            if (argument.rfind("--format=", 0) == 0) {
                if (!parse_format(argument.substr(9),
                                  command_line.format_json)) {
                    status = 2;
                    return false;
                }

                continue;
            }

            if (argument == "-h" || argument == "--help") {
                // Asked for, so it is output rather than a diagnostic:
                // `cclean --help | less` has to show something.
                print_usage(std::cout, argv[0]);
                return false;
            }

            if (argument == "-e" || argument == "--exclude") {
                if (i + 1 >= argc) {
                    std::cerr << argument << " needs a pattern.\n";
                    status = 2;
                    return false;
                }

                // Taken verbatim, so a pattern may itself begin with a dash.
                command_line.excludes.push_back(argv[++i]);
                command_line.set_excludes = true;
                continue;
            }

            if (argument.rfind("--exclude=", 0) == 0) {
                command_line.excludes.push_back(argument.substr(10));
                command_line.set_excludes = true;
                continue;
            }

            if (argument == "-V" || argument == "--version") {
                std::cout << "cclean " << version() << '\n';
                return false;
            }

            std::cerr << "Unknown option: " << argument << "\n\n";
            print_usage(std::cerr, argv[0]);
            status = 2;
            return false;
        }

        command_line.operands.push_back(argument);
    }

    if (command_line.no_config && !command_line.config_path.empty()) {
        std::cerr << "--config and --no-config cannot both be given.\n";
        status = 2;
        return false;
    }

    // No ROOT given means the working directory; --help is how usage is read.
    if (command_line.operands.empty()) {
        command_line.operands.push_back(".");
    }

    return true;
}

}  // namespace cclean::cli
