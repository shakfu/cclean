# Cleaner tool specification: core

Status: draft for review, 2026-11-18.

This file specifies the behaviour of a tool that recursively removes development detritus. The spec has three parts:

- This file, the core. Every tool meets it.
- [spec-modern.md](spec-modern.md), the modern profile: reclean and cclean.
- [spec-classic.md](spec-classic.md), the classic profile: hclean and oclean, which keep the rclean 0.4 command line.

A tool conforms when it meets the core and one profile. Profiles set interface details: option spellings, the prompt, the text and JSON formats, and exit statuses beyond `EXIT-1`.

The core and the modern profile were extracted from reclean 0.6.0 plus Unreleased (`6281e10`, Rust) and cclean 0.3.0 (`0927f41`, C++17). The evidence, with file:line citations, is in `docs/dev/reclean-inventory.md` in the reclean repository and `docs/dev/cclean-inventory.md` in the cclean repository. The classic profile was taken from hclean (`8f97c00`) and oclean (`72bf1a9`), which behave identically on every classic requirement.

[conformance.md](conformance.md) records each tool's results. `scripts/conform.py` checks them.

## Conventions

- **Requirement** (`ROOT-1`, `GLOB-3`, ...): normative. Core IDs are unchanged from the single-file draft. Modern IDs are unchanged too. Classic IDs carry `-C` (`GLOB-C1`).
- **Divergence** (`D-1`, `D-2`, ...): reclean and cclean differ. Each states both behaviours. A `Proposal` line, where present, is a recommendation for review, not current behaviour. Divergences stay beside their topic, in whichever file holds it.
- **Defect** (`X-1`, ...): a bug in one implementation, listed in the profile of that implementation. Defects are not part of the specification.
- A project adopting these files keeps the IDs and records its own answer to each divergence.

Terms:

- **root**: the directory a run scans.
- **entry**: any file system object below the root.
- **target**: an entry selected for removal.
- **reason**: why an entry became a target.
- **project directory**: the parent of a candidate build-artifact or dependency directory.
- **real directory**: a directory reached without following a symlink.
- **usage error**: a bad option, option value, glob or config file, detected before the scan.
- **warning**: a problem that does not stop the run, such as an unreadable directory.

## 1. Invocation

- **CLI-3.** Every tool offers: root, patterns, excludes, dry run, skip prompt, build artifacts, age filter, JSON output, config file, protection override, verbose, help and version. The profile sets the spellings.

## 2. Root

- **ROOT-1.** The root is never a target. Only entries below it are tested.
- **ROOT-2.** Protected names and excludes do not apply to the root. A root named `.git` or `venv` is scanned.
- **ROOT-3.** A root that is a symlink to a directory is followed. Entries below it are not followed (`WALK-1`).
- **ROOT-4.** A missing root, or a broken symlink as root, exits 1 with an error.

**D-5. Root that is a file.** reclean exits 0 with no output. cclean exits 1 with `Root path is not a directory: <root>`.
Proposal: exit 1; a file root is almost always a typo.

**D-6. Root in paths.** reclean prints text paths relative to the root (`src/__pycache__/`) but JSON paths with the root prefix (`proj/src/__pycache__`). cclean prints the root-joined path in both (`./src/__pycache__/`).
Proposal: one form in both outputs, plus `root` in the JSON document (cclean has it), so a consumer can resolve either form.

## 3. Matching

### 3.1 Glob dialect

- **GLOB-1.** Matching is case-sensitive and byte-wise.
- **GLOB-3.** `**/x` matches `x` at the top level and at any depth.
- **GLOB-4.** `?` matches exactly one character.
- **GLOB-5.** A leading dot has no special status. `*.pyc` matches `.hidden.pyc`.
- **GLOB-6.** A match covers the whole subject string (anchored at both ends).

Whether `*` matches `/` is set by the profile (`GLOB-2`, `GLOB-C1`).

**D-7. Classes, braces and escapes.** reclean (globset) supports `[ab]`, `{a,b}` and `\` escapes on Unix, and rejects an unclosed `[` with exit 2. cclean treats `[`, `]`, `{`, `}`, `!` and `\` as literal characters.
Proposal: support `[...]` and `{...}`. Users copy patterns from `.gitignore`, where both appear.

### 3.2 Match subject

**D-8. What a pattern is matched against.**

- reclean: the entry's path relative to the root (`src/main.c`). A pattern without `/` therefore matches only at the top level unless it starts with `**/` or `*`. The built-in patterns all start with `**/`.
- cclean: built-in patterns match the final component only. Config and CLI patterns match the final component first, then the path relative to the root. So `f.o` matches `a/b/f.o`.

Proposal: cclean's rule. `-g foo.log` matching nothing below the top level surprises users; `.gitignore` has the same rule as cclean.

### 3.3 Built-in patterns

**D-9. Default pattern list.**

- reclean, 14 patterns: `**/.DS_Store`, `**/Thumbs.db`, `**/__pycache__`, `**/.coverage`, `**/.mypy_cache`, `**/.pylint_cache`, `**/.pytest_cache`, `**/.ruff_cache`, `**/.rumdl_cache`, `**/.pyscn`, `**/.ropeproject`, `**/pip-log.txt`, `**/*.pyc`, `**/*.pyo`.
- cclean, 5 patterns: `__pycache__`, `*.pyc`, `*.pyo`, `.*_cache`, `.DS_Store`.

cclean's `.*_cache` covers 5 of reclean's names and any future `.<tool>_cache`. It also matches any user directory with that shape. reclean has `Thumbs.db`, `.coverage`, `.pyscn`, `.ropeproject` and `pip-log.txt`, which cclean lacks.

- **DEF-1.** No default list includes shell or REPL history (`.bash_history`, `.python_history`). Nothing regenerates it.

**D-10. User patterns and defaults.** In reclean, `-g` or a config `patterns` key replaces the defaults. In cclean, user patterns add to the defaults, and `--no-defaults` or `defaults = false` removes them.
Proposal: cclean's rule, so that adding `*.log` does not silently drop `__pycache__`.

### 3.4 Excludes

- **EXCL-1.** An entry matching an exclude is neither a target nor entered. Its subtree is pruned.
- **EXCL-2.** Excludes use the same dialect and subject as patterns.
- **EXCL-3.** Excludes apply to every reason: patterns, build artifacts and dependencies. (reclean exception: see `D-13`.)

**D-11. Default excludes.** reclean excludes `**/.venv` and `**/venv` by default. cclean has none.

**D-12. Command-line excludes and config excludes.** In reclean, `-e` appends to the config or built-in list. In cclean, any `-e` replaces the config list.

**D-13. Excludes inside a matched directory.** Both tools apply excludes only during the walk. A matched directory is removed whole, so `-e '*.pyc'` does not protect `.pyc` files inside a matched `__pycache__/`. In reclean, `-D` is checked before the built-in excludes, so `.venv` beside `uv.lock` is a target despite `**/.venv`.
Proposal: document it as the rule. Honouring excludes inside a target means partial removal, which breaks `MATCH-1`.

### 3.5 Protected directories

- **PROT-1.** The protected names are `.git`, `.hg`, `.svn`, `.config`, `.ssh`, `.gnupg`.
- **PROT-2.** The test is exact, case-sensitive equality with the final component, for any entry type. A file named `.hg` is protected.
- **PROT-3.** A protected entry is neither a target nor entered, even when a pattern names it. The check runs before excludes and patterns.
- **PROT-4.** One flag turns protection off for the run.

**D-14. Protected list in config.** reclean's `protected_dirs` key replaces the list. cclean has a boolean `skip_protected` only.

**D-15. Protected names inside a matched directory.** Neither tool checks them during removal. A `.git` inside a matched directory is removed with it. This was observed in cclean, whose `REVIEW.md` rates it High. In reclean it follows from the source: removal empties every entry.
Proposal: refuse a target that contains a protected name, with a warning, unless protection is off.
Deferred. The cost and the options are in [d15-benchmark.md](d15-benchmark.md).

### 3.6 Reasons

- **MATCH-1.** A matched directory is one target. It is not entered, so nothing inside it is listed separately.

**D-16. Reason names.** reclean: `pattern`, `broken-symlink`, `build-artifact`, `dependency`. cclean: `default`, `config`, `command-line`, `build-artifact`, `dependency`. reclean records the matching pattern in a separate field; cclean records where the pattern came from.

**D-17. Precedence when several rules match.** cclean: build artifact, then dependency, then the first matching pattern. reclean: dependency, then pattern, then build artifact. A `build/` matched by `-g build` and `-B` is `pattern` in reclean and `build-artifact` in cclean.

## 4. Walk

- **WALK-1.** Symlinks are never followed, during the walk or during sizing.
- **WALK-2.** The walk crosses mount points. No tool has a one-file-system option.
- **WALK-3.** An entry or directory that cannot be read gives a warning on stderr. The walk continues. The profile sets the exit status.
- **WALK-4.** Targets are sorted by path, comparing component by component as bytes. So `a/` sorts before `a.b/`.

**D-18. Matched symlinks.** reclean ignores a symlink matched by a pattern unless `-i` is given. cclean always makes it a target. Both unlink the link itself.

**D-19. Broken symlinks.** reclean's `-r` makes every broken symlink a target, regardless of patterns. cclean has no equivalent.

**D-20. Special files.** A FIFO, socket or device that matches a pattern is listed by both tools, as type `file` with size 0. reclean then fails to remove it (`unsupported file type`, exit 1). cclean removes it.
Proposal: skip special files during the walk with a debug note.

**D-21. Containment check.** reclean canonicalizes each directory before listing it and skips one that resolves outside the root. cclean scans by path and relies on the removal checks (`RM-2`).

## 5. Build artifacts

Enabled by a flag or config `build_artifacts = true`.

- **ART-1.** The table of directory names and marker files:

| Directory | Marker files (any one) |
|-|-|
| `build` | `CMakeLists.txt`, `meson.build`, `package.json`, `build.gradle`, `build.gradle.kts`, `pyproject.toml`, `setup.py`, `pubspec.yaml` |
| `target` | `Cargo.toml`, `pom.xml` |
| `dist` | `package.json`, `pyproject.toml`, `setup.py` |
| `.next`, `.nuxt`, `.svelte-kit`, `.turbo`, `.parcel-cache` | `package.json` |
| `.gradle` | `build.gradle`, `build.gradle.kts` |
| `zig-out`, `zig-cache`, `.zig-cache` | `build.zig` |
| `.build` | `Package.swift` |
| `_build` | `mix.exs` |

- **ART-2.** A candidate is a real directory whose name is in the table.
- **ART-3.** The project directory contains a marker paired with that name. Pairing is strict: `package.json` does not qualify `target`.
- **ART-4.** The project directory contains `.git`, as a directory or a file (submodules and worktrees use a file). The profile may also accept a `.git` in an ancestor (`ART-C1`).
- **ART-6.** A matched artifact directory is not entered.

Which repository owns a candidate is set by the profile (`ART-5`, `ART-C1`).

**D-22. Git-tracked guard.** reclean runs `git ls-files` in the project directory and keeps a candidate that holds tracked files, such as a committed `dist/` or `vendor/`. If git cannot answer, the candidate is kept with a warning (exit 3). cclean has no such check.
Proposal: adopt it. A committed `build/` is the main false positive of `ART-1`.

**D-23. Kept candidate.** When reclean keeps a candidate (tracked, or git failed), it then walks into it as an ordinary directory, so patterns can match inside it.

**D-24. Monorepo projects.** cclean's config key `project_roots` names directories, relative to the config file, that count as projects without `ART-4` and `ART-5`. reclean has none.

**D-25. Symlinked `.git` and markers.** reclean follows symlinks when testing `.git` beside the project directory and when testing markers. cclean does not: a symlinked `.git` or marker does not count. reclean's outermost test (`ART-5`) uses lstat.

## 6. Filters

- **FILT-1.** Filters apply after sizing. Totals and listings count only targets that pass.
- **FILT-3.** `--older-than` uses modification time. A file or symlink target uses its own mtime. A directory target counts as recent if its own mtime or that of any entry below it is recent.
- **FILT-4.** A target older than the limit, with mtime at or before `now - limit`, passes. A future mtime is recent.
- **FILT-5.** Duration units are `s`, `m`, `h`, `d`, `w` (1, 60, 3600, 86400, 604800 seconds), lowercase only. Any other unit is a usage error.
- **FILT-8.** A target whose mtime cannot be read does not pass the age filter.

**D-28. Duration grammar.** reclean accepts digits only, trimming surrounding whitespace (`7d`). cclean accepts a decimal fraction (`1.5h`, `.5d`) and no whitespace.

**D-29. Unreadable mtime.** reclean drops the target silently. cclean drops it with the warning `Cannot apply age filter to <path>`, which makes the exit status 3.

## 7. Sizing

- **SIZE-1.** Sizes are apparent bytes (`st_size`), not allocated blocks.
- **SIZE-2.** A directory's size is the sum over the regular files below it. Directory entries add 0. Symlinks: see `D-31`.
- **SIZE-3.** Hard links count once per link.
- **SIZE-4.** Formatting: below 1024, `N B` as an integer. Otherwise divide by 1024 until the value is below 1024, and print two decimals with a binary unit: `1.95 KiB`. Rounding is the platform's float formatting. 1048575 bytes prints as `1024.00 KiB`.

**D-31. Symlink size.** reclean counts a symlink, as a target or inside one, at the length of its target text. cclean counts 0.

**D-32. Largest unit.** reclean stops at `TiB`. cclean goes to `PiB`.

## 8. Confirmation

- **ASK-1.** The tool asks before removing, unless the run is a dry run, the skip flag was given, or nothing matched.
- **ASK-4.** A declined prompt removes nothing, and exits as a completed run (`EXIT-1`).
- **ASK-5.** JSON output asks too. The prompt does not go to stdout under JSON.

How the answer is read is set by the profile (`ASK-2`, `ASK-3`, `ASK-C1`).

## 9. Removal

- **RM-1.** Removal never follows a symlink, including one that replaced a path component after the scan. On Unix this means a walk through directory descriptors: open the root, then each component below it with `O_NOFOLLOW`. (See `D-36` for Windows.)
- **RM-2.** Before removing, the tool compares the target's type, device and inode with the values recorded during the scan. A replaced target is refused with an error.
- **RM-3.** A directory target is emptied through descriptors, and its entries are unlinked with `unlinkat`. Symlinks inside are unlinked, never followed.
- **RM-5.** A failed target is reported. The remaining targets are still removed, and the run exits 1.
- **RM-6.** A failed directory target may be left partly emptied.
- **RM-8.** Entries inside a target are not identity-checked individually.

Read-only directories inside a target are handled by the profile (`RM-4`, `RM-C1`).

**D-36. Windows.** reclean supports Windows. It removes by path after comparing type, volume serial number and file ID. The other tools are POSIX-only.

**D-37. Bytes reported as freed.** reclean and cclean report the sum of scanned sizes of fully removed targets, not a measurement. reclean prints it whenever at least one target was removed. cclean prints it only when no removal failed. Neither counts the removed part of a failed target.

## 10. Output

### 10.1 Streams

- **OUT-1.** The report goes to stdout: the matched list, totals and the removal summary.
- **OUT-2.** Warnings, errors and progress go to stderr.
- **OUT-3.** Under JSON, the text report is replaced by one JSON document on stdout. Warnings and errors still go to stderr.

### 10.2 Colour

These apply to a tool that colours its output. The palette is set by the profile.

- **COL-1.** `--color auto` colours a stream only when it is a terminal and `NO_COLOR` is unset or empty.
- **COL-2.** `--color always` colours through pipes and overrides `NO_COLOR`. `--color never` disables colour.
- **COL-3.** stdout and stderr are decided separately.
- **COL-4.** `TERM` is not consulted.
- **COL-6.** JSON is never coloured.

### 10.3 Progress

- **PROG-1.** Progress is drawn on stderr only when stderr is a terminal, and is erased when done.

### 10.4 Names

- **NAME-1.** Text output escapes control characters and backslashes in file names, so a name cannot forge a line or emit a terminal escape sequence.
- **NAME-2.** JSON output is valid UTF-8. Each invalid byte becomes U+FFFD.

**D-44. Escape format.** cclean writes `\xNN` for bytes 0x00-0x1f and 0x7f and passes bytes 0x80 and above through raw. reclean replaces invalid UTF-8 with U+FFFD and writes Rust escapes (`\n`, `\u{1}`). reclean does not escape backslashes on Windows. reclean's warning and error lines use Rust `{:?}` quoting rather than the report escaping.

## 11. JSON

- **JSON-1.** `--format json` writes one document to stdout after the run, including for dry runs, declined prompts and empty results.

The document shape is set by the profile.

**D-46. Errors.** No tool emits JSON for an error that ends the run early, such as a bad root or a bad glob. Consumers must check the exit status before parsing.

## 12. Exit status

- **EXIT-1.** Status 0 means the run completed and nothing failed. Status 1 means a runtime failure: a bad root, or at least one failed removal. A usage error exits non-zero before any scan. The profile sets the usage-error status, and whether warnings have their own status.
- **EXIT-2.** Status 1 outranks any warning status.

## 13. Configuration

- **CFG-1.** The file is TOML, named `.<tool>.toml` (`.reclean.toml`, `.hclean.toml`), so tools do not read each other's files.
- **CFG-2.** Upward discovery stops at the first file found. Files are not merged.
- **CFG-4.** A command-line option that was given overrides the config value. No tool has a flag that restores a built-in default changed by config: `build_artifacts = true` cannot be turned off for one run.

**D-30. Filters in config.** reclean's config stores filters as integers (`older_than_secs`, `larger_than_bytes`). cclean's stores the CLI strings (`older_than = "30d"`).
Proposal: strings, so the config reads like the command line.

**D-47. When config is read.** reclean reads a file only with `-c`. cclean always reads one, unless `--no-config` is given.
Trade-off: cclean's rule makes a stray file in a parent directory apply to every run. reclean's rule makes a project's config easy to forget.

**D-48. Discovery start and global file.** reclean searches upward from the working directory, then reads a global file (`$XDG_CONFIG_HOME/reclean/config.toml`, falling back to `~/.config`; `%APPDATA%` on Windows). cclean searches upward from the root and has no global file. Neither stops below `~`.

**D-49. Keys.**

| Concept | reclean | cclean |
|-|-|-|
| Root | `path` | none |
| Patterns | `patterns` (replaces defaults) | `patterns` (adds to defaults) |
| Defaults on/off | none | `defaults` |
| Excludes | `exclude_patterns` | `excludes` |
| Protected | `protected_dirs` (list) | `skip_protected` (bool) |
| Artifacts, dependencies | `build_artifacts`, `dependencies` | `build_artifacts`, `dependencies` |
| Custom dependencies | none | `dependency_markers` |
| Monorepo projects | none | `project_roots` |
| Filters | `older_than_secs`, `larger_than_bytes` (integers) | `older_than`, `larger_than` (strings) |
| Run flags | `dry_run`, `skip_confirmation`, `include_symlinks`, `remove_broken_symlinks`, `stats_mode`, `show_progress` (ignored) | none |
| Split | none | `split` |

**D-50. TOML dialect.** reclean parses full TOML. cclean parses a line-oriented subset: one `key = value` per line, basic strings without escapes, single-line arrays and no tables. A file valid for cclean is valid TOML. The reverse is not true.

**D-51. Starter file.** reclean's `-w` writes `.reclean.toml` into the working directory and refuses to overwrite one. cclean has no writer.

## 14. Index

Requirements by file:

| Area | Core | Modern | Classic |
|-|-|-|-|
| Invocation | CLI-3 | CLI-1, CLI-2 | CLI-C1, CLI-C2 |
| Root | ROOT-1 to ROOT-4 | | |
| Glob | GLOB-1, GLOB-3 to GLOB-6 | GLOB-2 | GLOB-C1 |
| Defaults, excludes, protection | DEF-1, EXCL-1 to EXCL-3, PROT-1 to PROT-4, MATCH-1 | | |
| Walk | WALK-1 to WALK-4 | WALK-5 | |
| Build artifacts | ART-1 to ART-4, ART-6 | ART-5 | ART-C1 |
| Dependencies | | DEP-1 to DEP-5 | |
| Filters | FILT-1, FILT-3 to FILT-5, FILT-8 | FILT-2, FILT-6, FILT-7 | |
| Sizing | SIZE-1 to SIZE-4 | | |
| Confirmation | ASK-1, ASK-4, ASK-5 | ASK-2, ASK-3 | ASK-C1 |
| Removal | RM-1 to RM-3, RM-5, RM-6, RM-8 | RM-4, RM-7 | RM-C1 |
| Output | OUT-1 to OUT-3, COL-1 to COL-4, COL-6, PROG-1, NAME-1, NAME-2 | TXT-1 to TXT-6, COL-5, PROG-2 to PROG-4 | TXT-C1 to TXT-C3 |
| JSON | JSON-1 | JSON-2, JSON-3 | JSON-C1 |
| Exit status | EXIT-1, EXIT-2 | EXIT-3 | EXIT-C1 |
| Configuration | CFG-1, CFG-2, CFG-4 | CFG-3 | CFG-C1 |
| Library | | LIB-1 | |

Divergences:

| ID | Topic | File | Proposal |
|-|-|-|-|
| D-1 | `-d` meaning | modern | drop `-d`; `-n` dry run, `-D` dependencies |
| D-2 | operands vs options | modern | open |
| D-3 | short-option syntax | modern | open |
| D-4 | `text` vs `human` | modern | open |
| D-5 | file as root | core | exit 1 |
| D-6 | root prefix in paths | core | one form; `root` in JSON |
| D-7 | classes and braces | core | support them |
| D-8 | match subject | core | name, then relative path |
| D-9 | default list | core | open |
| D-10 | user patterns vs defaults | core | add, with `--no-defaults` |
| D-11 | default excludes | core | open |
| D-12 | CLI vs config excludes | core | open |
| D-13 | excludes inside targets | core | document as the rule |
| D-14 | protected list in config | core | open |
| D-15 | protected names inside targets | core | deferred |
| D-16 | reason names | core | open |
| D-17 | reason precedence | core | open |
| D-18 | matched symlinks | core | open |
| D-19 | broken symlinks | core | open |
| D-20 | special files | core | skip during the walk |
| D-21 | containment check | core | open |
| D-22 | git-tracked guard | core | adopt |
| D-23 | descent into kept candidates | core | open |
| D-24 | `project_roots` | core | open |
| D-25 | symlinked `.git` and markers | core | open |
| D-26 | `dependency_markers` | modern | open |
| D-27 | git-tracked guard for `-D` | modern | adopt with D-22 |
| D-28 | duration grammar | core | open |
| D-29 | unreadable mtime warning | core | open |
| D-30 | filters in config | core | strings |
| D-31 | symlink size | core | open |
| D-32 | `PiB` | core | open |
| D-33 | prompt keys | modern | any other key declines |
| D-34 | prompt stream | modern | stderr |
| D-35 | split removal | modern | open |
| D-36 | Windows | core | open |
| D-37 | bytes freed when a removal fails | core | open |
| D-38 | empty-result line | modern | open |
| D-39 | dry-run line | modern | open |
| D-40 | removal summary | modern | open |
| D-41 | warnings layout | modern | open |
| D-42 | quiet and stats | modern | open |
| D-43 | progress switch | modern | open |
| D-44 | escape format | core | open |
| D-45 | JSON shape | modern | agree before stable; schema 2 |
| D-46 | JSON on early errors | core | open |
| D-47 | when config is read | core | open |
| D-48 | discovery start, global file | core | open |
| D-49 | config keys | core | follows other decisions |
| D-50 | TOML dialect | core | open |
| D-51 | starter file | core | open |
| D-52 | library packaging | modern | open |
