#ifndef CCLEAN_CLI_OPTIONS_HPP
#define CCLEAN_CLI_OPTIONS_HPP

#include <string>
#include <vector>

namespace cclean::cli {

// The command line as typed. A `set_` flag records that the option appeared,
// which is what lets a config file supply the value only when it did not: the
// value alone cannot say, since false and "" are also legitimate settings.
struct CommandLine {
    bool dry_run = false;
    bool use_defaults = true;
    bool use_skips = true;
    bool verbose = false;
    bool build_artifacts = false;
    bool dependencies = false;
    bool assume_yes = false;
    bool format_json = false;

    std::string older_than;
    std::string larger_than;
    // Empty unless --config was given; `no_config` is --no-config.
    std::string config_path;
    bool no_config = false;
    std::vector<std::string> excludes;

    // ROOT, then any pattern. Never empty after a successful parse.
    std::vector<std::string> operands;

    bool set_defaults = false;
    bool set_build_artifacts = false;
    bool set_dependencies = false;
    bool set_skips = false;
    bool set_excludes = false;
    bool set_older_than = false;
    bool set_larger_than = false;
};

void print_usage(const char* program);

// False means the program is done: `status` is what it should return. That
// covers --help and --version as well as a rejected argument.
bool parse_command_line(
    int argc,
    char* argv[],
    CommandLine& command_line,
    int& status);

}  // namespace cclean::cli

#endif  // CCLEAN_CLI_OPTIONS_HPP
