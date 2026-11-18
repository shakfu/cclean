# Cleaner tool specification: classic profile

Status: draft for review, 2026-11-18.

This profile extends [spec.md](spec.md), the core. hclean and oclean follow it. It keeps the rclean 0.4 command line. Each requirement was taken from hclean (`8f97c00`) and oclean (`72bf1a9`), which agree on all of them.

## 1. Invocation

| Capability | Spelling |
|-|-|
| Root | `-p, --path PATH` (default `.`) |
| Patterns | `-g, --glob GLOB`, repeatable; replaces the defaults |
| Preset pattern sets | `--preset NAME`, repeatable |
| Exclude | `-e, --exclude GLOB`, repeatable |
| Dry run | `-d, --dry-run` |
| Skip prompt | `-y, --skip-confirmation` |
| Per-pattern stats | `-s, --stats` |
| Age filter | `-o, --older-than D` |
| Build artifacts | `-B, --build-artifacts` |
| Matched symlinks | `-i, --include-symlinks` |
| Broken symlinks | `-r, --remove-broken-symlinks` |
| Config | `-c, --configfile [PATH]` |
| Format | `--format text\|json` |
| Protected override | `--no-protect` |
| Quiet | `-q, --quiet` |
| Verbose | `-v, --verbose` |
| Progress | `-P, --progress`, `--no-progress` |
| List patterns | `-l, --list` |
| Write starter config | `-w, --write-configfile` |
| Help, version | `-h, --help`, `--version` |

- **CLI-C1.** Every option in the table is accepted with the meaning given.
- **CLI-C2.** A usage error exits 1 before any scan, with `<tool>: <message>` on stderr.

**DC-1. Colour.** oclean has `--color auto|always|never` and colours directory paths bold blue. hclean has no colour. The core `COL` requirements apply to oclean only.

## 2. Matching

- **GLOB-C1.** A pattern without `/` matches the final component. A pattern with `/` matches the relative path segment by segment. `*` and `?` match within one segment and never match `/`. A whole segment `**` matches zero or more segments. `[...]` is a class, with `!` or `^` negation and `a-z` ranges. An unclosed `[` is a usage error.

## 3. Build artifacts

- **ART-C1.** The project directory, or one of its ancestors, contains `.git`. The search stops below `~`: the home directory and its ancestors are not examined. A start outside `~` climbs to `/`. A nested repository is still a project, so workspace members such as `crates/foo/target` qualify.

## 4. Confirmation

- **ASK-C1.** The prompt `Delete N item(s)? [y/N] ` goes to stderr. One line is read. After trimming whitespace, `y` or `yes` in any case means yes. Anything else, EOF or a read error means no. Typeahead is not discarded.

## 5. Removal

- **RM-C1.** A read-only directory inside a target, owned by the user, is made owner-writable through a descriptor, then emptied. A directory owned by someone else is not changed, and the target fails.

## 6. Text report

- **TXT-C1.** Each target is one line, `Matched: <path>`, with no header, size or total. The path is the root made absolute and normalised, joined with the path below it.
- **TXT-C2.** `-s` appends one line per pattern: `  <pattern>: N item(s), SIZE`.
- **TXT-C3.** After removal, `Removed N item(s), SIZE.` goes to stdout. N counts removed targets. SIZE is the bytes freed, summed as files are unlinked. Each failure goes to stderr as `<tool>: <message>`.

## 7. JSON

- **JSON-C1.** The document has four keys, in this order:

| Key | Content |
|-|-|
| `matches` | array of `{path, size, pattern}` |
| `summary` | `{total_count, total_size, total_size_human, freed_size, freed_size_human, dry_run}` |
| `stats` | array of `{pattern, count, size, size_human}` |
| `failures` | array of `{path, error}` |

`pattern` is the matching glob, `build-artifact` or `broken-symlink`.

## 8. Exit status

- **EXIT-C1.** Exit statuses:

| Status | Meaning |
|-|-|
| 0 | Completed and no removal failed, including when warnings were raised. Also `-h` and `--version`. |
| 1 | Usage error, bad root, or at least one removal failed. |

## 9. Configuration

- **CFG-C1.** A file is read only with `-c`. Bare `-c` searches upward from the working directory, stopping below `~`, then reads `<tool>/config.toml` under `$XDG_CONFIG_HOME` or `~/.config`. Unknown keys are ignored. A missing key takes its default.

**DC-2. Relative `path`.** hclean resolves a relative `path` key against the config file's directory, or the working directory for the global file. oclean resolves it against the working directory.
