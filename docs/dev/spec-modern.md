# Cleaner tool specification: modern profile

Status: draft for review, 2026-11-18.

This profile extends [spec.md](spec.md), the core. reclean and cclean follow it. IDs are unchanged from the single-file draft.

## 1. Invocation

The two command lines share capabilities but not spellings.

| Capability | reclean | cclean |
|-|-|-|
| Root | `-p, --path PATH` (default `.`) | first operand `ROOT` (default `.`) |
| Patterns | `-g, --glob GLOB`, repeatable | later operands `PATTERN ...` |
| Exclude | `-e, --exclude GLOB`, repeatable | `-e, --exclude PATTERN`, repeatable |
| Dry run | `-d, --dry-run` | `-n, --dry-run` |
| Skip prompt | `-y, --skip-confirmation` | `-y, --yes` |
| Build artifacts | `-B, --build-artifacts` | `-b, --build-artifacts` |
| Dependencies | `-D, --dependencies` | `-d, --dependencies` |
| Age filter | `-o, --older-than D` | `--older-than D` |
| Size filter | `--larger-than S` | `--larger-than S` |
| Colour | `--color auto\|always\|never` | `--color auto\|always\|never` |
| Format | `--format text\|json` | `--format human\|json` |
| Config | `-c, --configfile [PATH]` | `--config FILE`, `--no-config` |
| Protected override | `--no-protect` | `--no-skip` |
| Drop built-in patterns | implicit: `-g` replaces them | `--no-defaults` |
| Verbose | `-v` | `-v` |
| Help, version | `-h`, `-V` | `-h`, `-V` |
| Quiet | `-q` | none |
| Per-pattern stats | `-s, --stats` | none |
| List built-in tables | `-l, --list` | none (shown in `--help`) |
| Shell completions | `--completions SHELL` | none |
| Write starter config | `-w` | none |
| Matched symlinks | `-i` | always on |
| Broken symlinks | `-r` | none |
| Disable split removal | none | `--no-split` |

- **CLI-1.** `-y`, `-v`, `-e`, `--older-than`, `--larger-than`, `--color`, `-h` and `-V` have the same meaning in both.
- **CLI-2.** A usage error exits 2 before any scan (see `EXIT-3`).

**D-1. `-d` means opposite things.** In reclean `-d` is dry run. In cclean `-d` widens removal to dependency trees. A user moving between the tools can delete `node_modules` while expecting a preview. The classic profile also uses `-d` for dry run.
Proposal: neither tool uses `-d`; dry run is `-n` (the `rm`/`make`/`rsync` convention) and dependencies is `-D`.

**D-2. Root and patterns as operands.** cclean takes `ROOT [PATTERN ...]` as operands. reclean takes them as `-p` and `-g` options. cclean warns when a pattern operand names an existing directory, since the user may have meant a second root.

**D-3. Short-option syntax.** reclean (clap) accepts bundled flags (`-dsy`) and `-gX`/`-g=X`. cclean accepts neither. cclean lets a value start with `-` (`-e -foo`); reclean rejects it.

**D-4. Format names.** The text format is `text` in reclean and `human` in cclean.

## 2. Matching

- **GLOB-2.** `*` matches any sequence, including `/`.

## 3. Walk

- **WALK-5.** The walk runs in parallel across cores.

## 4. Build artifacts

- **ART-5.** Outermost project: no directory strictly above the project directory, up to and including the root, contains `.git`. A nested repository is not a project unless it is the root. Nothing above the root is examined.

## 5. Dependency trees

Enabled by `-D` (reclean) or `-d` (cclean), or config `dependencies = true`.

- **DEP-1.** The table is identical, and checked in this order. The first row whose lock file is present wins.

| Directory | Lock file | Restore command |
|-|-|-|
| `.venv` | `uv.lock` | `uv sync` |
| `node_modules` | `package-lock.json` | `npm ci` |
| `node_modules` | `npm-shrinkwrap.json` | `npm ci` |
| `node_modules` | `yarn.lock` | `yarn install --immutable` |
| `node_modules` | `pnpm-lock.yaml` | `pnpm install --frozen-lockfile` |
| `node_modules` | `bun.lock` | `bun install --frozen-lockfile` |
| `node_modules` | `bun.lockb` | `bun install --frozen-lockfile` |
| `vendor` | `go.mod` | `go mod vendor` |

- **DEP-2.** A candidate is a real directory whose name is in the table. The lock file is a regular file, tested without following symlinks.
- **DEP-3.** No `.git` or outermost-project rule applies.
- **DEP-4.** The restore command is shown with the target in text and JSON output.
- **DEP-5.** A matched dependency tree is not entered.

**D-26. Custom pairs.** cclean's config key `dependency_markers` adds `[directory, marker]` pairs, which carry no restore command. reclean's table is fixed.

**D-27. Git-tracked guard.** reclean applies `D-22` to dependency trees too. A committed `vendor/` is kept.

## 6. Filters

- **FILT-2.** Several filters combine with AND.
- **FILT-6.** `--larger-than` passes targets with `size >= limit`.
- **FILT-7.** Size suffixes are none, `B`, `K`, `KiB`, `M`, `MiB`, `G`, `GiB`, `T`, `TiB`. They are binary (K = 1024) and case-sensitive. `KB` and `k` are errors. A decimal fraction is allowed (`1.5K`) and the result is truncated to whole bytes.

## 7. Confirmation

- **ASK-2.** When stdin is a terminal, typeahead is discarded before the prompt is read. One keypress answers; Enter is not needed.
- **ASK-3.** When stdin is not a terminal, exactly one byte is read. `y` or `Y` means yes. Any other byte, EOF or a read error means no.

**D-33. Prompt text and keys.**

- reclean: `Do you want to delete the above? [y/n] ` on a terminal. Only `y`, `Y`, `n` and `N` answer; other keys and Enter are ignored. Not on a terminal: `[y/N] `.
- cclean: `Permanently remove? [y/N] `. Any key other than `y` or `Y` means no. Under JSON: `Permanently remove N targets? [y/N] `.

Proposal: cclean's rule (default no, any other key declines). reclean's README.md:302 already documents it.

**D-34. Prompt stream.** reclean writes the prompt to stderr. cclean writes it to stdout in text mode, and to stderr under JSON. Redirecting cclean's stdout to a file therefore hides the prompt.
Proposal: stderr in both modes.

## 8. Removal

- **RM-4.** Read-only directories are not made writable. Their contents fail to be removed, and the target counts as failed.
- **RM-7.** Targets are removed in parallel. Results are reported in list order after all removals finish.

**D-35. Split removal.** cclean divides a target of 4096 or more entries across the worker pool (`--no-split` turns this off). reclean removes each target on one thread.

## 9. Output

### 9.1 Text report

- **TXT-1.** The listing opens with an empty line, then `Matched targets:`.
- **TXT-2.** Each target is one line: two spaces, the path, two spaces, the size. A directory's path ends in `/`. A symlink to a directory does not.
- **TXT-3.** A dependency target appends two spaces and `restore: <command>`.
- **TXT-4.** The total reads `N targets, SIZE to reclaim`, with `1 target` for one.
- **TXT-5.** A breakdown by reason follows the total only when two or more reasons are present.
- **TXT-6.** `-v` prints `  removed  <path>` for each removed target, after all removals finish.

**D-38. Empty result.** cclean prints `No matching targets found.` reclean prints nothing.

**D-39. Dry run line.** cclean ends a dry run with `Dry run: nothing was removed.` reclean prints nothing extra.

**D-40. Removal summary.**

- reclean: `Deleted N item(s) totalling SIZE`. Each failure is `error: Failed to remove "<path>": <reason>`, followed by `error: N removal(s) failed`.
- cclean: `Removed N, SIZE reclaimed`, or `Removed N, M failed`. Each failure is `  failed  <path>: <reason>`.

**D-41. Warnings layout.** cclean prints a `Warnings:` block, sorted. reclean prints each warning as it occurs, prefixed `warning: `, in thread order, then `warning: N path(s) could not be read; matches beneath them were not considered`.

**D-42. Quiet and stats.** reclean has `-q` (errors only) and `-s` (count and size per pattern). cclean has neither. Its reason breakdown covers part of what `-s` shows.

### 9.2 Colour

- **COL-5.** Palette: directory paths cyan, sizes and restore hints dim, counts and totals bold, the removal summary green, warnings yellow, errors red.

### 9.3 Progress

- **PROG-2.** Scanning shows `  scanning N`, then `  sizing N`.
- **PROG-3.** Removal shows `  removing [<30 chars of # and ->] PCT%  DONE / TOTAL entries`. TOTAL is the sum over targets of (entries below the target + 1).
- **PROG-4.** Progress does not depend on `--color`, `NO_COLOR` or `--format`.

**D-43. Turning progress off.** reclean's `-q` turns it off. cclean has no switch.

## 10. JSON

- **JSON-2.** The document carries `"schema": 1` and `"config"` (the config path, or `null`).
- **JSON-3.** Each target records its path, size in bytes, type (`file`, `directory`, `symlink`; special files are `file`), reason, and `restore` for dependency targets only.

**D-45. Document shape.** Both documents declare schema 1, but they are incompatible.

| | reclean | cclean |
|-|-|-|
| Key order | alphabetical | fixed, `schema` first |
| Root | absent | `root` |
| Outcome | `summary.dry_run`, `summary.cancelled` | `status`: `failed`, `empty`, `cancelled`, `dry-run` or `removed` |
| Target list | `matches` | `targets` |
| Size field | `size` | `bytes` |
| Pattern | `pattern` per target | absent |
| Totals | `summary.total_count`, `total_size`, `total_size_human` | `total.targets`, `total.bytes` |
| Per-reason | `reasons`: array of `{reason, count, size}` | `stats`: object keyed by reason, `{targets, bytes}` |
| Per-pattern | `stats`: array, empty unless `-s` | absent |
| Removed | `summary.removed_count`, `summary.removed_size` | `removed` |
| Failures | `failures`: array of `{path, error}` | `failed` (count); messages in `warnings` |
| Warnings | array of `{path, error}` | array of strings |

Proposal: agree on one shape before either tool ships schema 1 as stable. Bump to schema 2 in whichever tool changes. Shared design points from both:

- `status` as one enum (cclean) is simpler to consume than several booleans.
- `failures` and `warnings` as `{path, error}` objects (reclean) are easier to consume than strings.
- `root` should be present, so relative target paths can be resolved (`D-6`).

## 11. Exit status

- **EXIT-3.** Exit statuses:

| Status | Meaning |
|-|-|
| 0 | Completed with no warnings: removed, nothing matched, dry run, or declined. Also `-h` and `-V`. |
| 1 | Runtime failure: bad root, or at least one removal failed. |
| 2 | Usage or configuration error, detected before the scan. |
| 3 | Completed, but warnings were raised (for example, unreadable paths) and no removal failed. |

## 12. Configuration

- **CFG-3.** An unknown key is an error (exit 2). A missing key takes its default.

## 13. Library

- **LIB-1.** Scanning, filtering and removal live in a library that prints nothing itself.

**D-52. Packaging.** cclean installs `libcclean.a` and public headers. reclean exposes `lib.rs` as a crate and reports through `log` records under the `reclean::report` target.

## 14. Defects found during extraction

Not part of the specification. Each is a bug in one implementation.

reclean:

- **X-1.** `-q` without `-y` hides the listing but still prompts, so the user confirms without seeing the targets.
- **X-2.** `-l`, `--completions` and JSON output panic on a closed stdout (`| head -1`), exit 101.
- **X-3.** JSON keys are alphabetical, although README.md:224 says the document opens with `"schema": 1`.
- **X-4.** README.md:302 says any key other than `y` cancels. On a terminal, other keys are ignored (`D-33`).
- **X-5.** README.md:381 says Windows compares only the file type. Unreleased code also compares volume serial and file ID.
- **X-6.** A special file passes the walk and always fails at removal (`D-20`).

cclean:

- **X-7.** `--no-defaults` is documented as "Match only the patterns given on the command line", but config `patterns` still match.
- **X-8.** `--older-than=` or `--larger-than=` with an empty value silently disables the filter, including one set in config.
- **X-9.** An invalid `older_than` in config is reported as `Invalid --older-than duration`, with no file and line.
- **X-10.** The JSON-mode prompt has no singular: `Permanently remove 1 targets?`.
- **X-11.** `-v` help says "Name every item as it is removed". It names targets, after all removals finish.
- **X-12.** Root error messages print the root without escaping (`NAME-1`).
