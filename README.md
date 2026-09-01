# cclean

Finds and removes build caches and editor debris from a directory tree. It lists everything it matched, with sizes, and waits for confirmation before deleting anything.

```text
$ cclean --dry-run api

Matched targets:
  api/.DS_Store  6.00 KiB
  api/.pytest_cache/  148.00 KiB
  api/pkg/__pycache__/  1.20 MiB
  api/tests/__pycache__/  86.33 KiB

4 targets, 1.43 MiB to reclaim
Dry run: nothing was removed.
```

Without `--dry-run` it prints the same list, then asks:

```text
Permanently remove? [y/N] y
Removed 4, 1.43 MiB reclaimed
```

## Build

The build is defined by `CMakeLists.txt`. The `Makefile` is a frontend over it, so either entry point works.

```text
make                    # configure and build into build/
make install            # install to /usr/local, stripped
make clean              # remove build/
```

`make install` places three things under the prefix: `bin/cclean`, `lib/libcclean.a`, and the headers under `include/cclean/`.

```text
cmake -S . -B build
cmake --build build --parallel
cmake --install build --strip
```

Both honour the usual overrides:

```text
make BUILD=out BUILD_TYPE=Debug
make install PREFIX=$HOME/.local
```

The version reported by `cclean --version` comes from `project(... VERSION ...)` in `CMakeLists.txt`, passed to the compiler as a definition. A build made outside CMake reports `unknown`.

Requires CMake 3.16 and a C++17 compiler. No dependencies beyond the standard library and pthreads. POSIX only: the confirmation prompt uses `<termios.h>`.

## Library

The program is a thin frontend over `libcclean`, which does the finding, sizing and removing. The library touches no terminal, prints nothing of its own, and reads no environment variable; argument parsing, the confirmation prompt, colour, progress and the human-readable report all live in `cli/`.

```cpp
#include <cclean/cclean.hpp>

cclean::ScanOptions options;
options.patterns = cclean::compile_patterns(true, {}, {"*.log"});
options.excludes = cclean::compile_excludes({"vendor"});
options.larger_than = 1024u * 1024;

const cclean::ScanResult result = cclean::scan("./project", options);

for (const cclean::Target& target : result.targets) {
    std::string error;
    cclean::remove_target(target, error);
}
```

`scan()` walks the tree, sizes every matched directory, applies the age and size filters, and returns the targets sorted by path together with any paths it could not read. It takes an optional `ProgressFn`, called once per unit of work from the worker threads but serialised, so throttling is the caller's to decide.

The headers, each usable on its own:

| Header | Contents |
|-|-|
| `cclean/scan.hpp` | `ScanOptions`, `ScanResult`, `scan()`, `compile_patterns()`, `compile_excludes()` |
| `cclean/target.hpp` | `Target` and the `Reason` it reports |
| `cclean/remove.hpp` | `remove_target()`, the descriptor-relative deletion boundary |
| `cclean/config.hpp` | `Config`, `find_config()`, `load_config()` |
| `cclean/glob.hpp` | `Glob` and the two matching helpers |
| `cclean/filters.hpp` | `parse_duration()`, `parse_size()`, `is_older_than()` |
| `cclean/project.hpp` | Marker and repository detection for build artifacts and dependency trees |
| `cclean/defaults.hpp` | The built-in pattern, artifact and skip tables |
| `cclean/json.hpp` | `write_json()`, the machine-readable report |
| `cclean/text.hpp` | Size formatting, terminal-safe display escaping, JSON string escaping |
| `cclean/cclean.hpp` | All of the above, plus `version()` |

The static library is `libcclean.a`. Everything is in namespace `cclean`. Link it with a threading library: a project that adds this one with `add_subdirectory` and links `cclean_core` gets `Threads::Threads` from the target, and one linking the installed archive needs `-pthread` itself. There is no installed CMake package config yet.

## Tests

```text
make test               # or: ctest --test-dir build --output-on-failure
```

Two suites registered with CTest, neither with a dependency of its own:

- `tests/unit.cpp` links `libcclean` and covers the glob matcher, the reasons a scan reports, size formatting, saturating addition, the skip list, pattern scope, build-artifact detection, the numeric filters, output escaping, and the worker pool. It also includes two headers under `src/`, which are part of the library but not of its installed API.

- `tests/cli.sh` drives the built binary: exit codes, what is actually deleted, confirmation handling, the skip list end to end, build artifacts, symlinks, and the shape of the output.

Both exit non-zero on failure, so `make test` fails the build.

The terminal branch of the confirmation prompt takes a single keypress in raw mode, so it is driven through a pseudo-terminal from `python3`, and is skipped where `python3` is absent. The driver reads the pty continuously rather than sleeping before it answers: `tcsetattr(TCSAFLUSH)` drains output first, which on a pty blocks until the master reads, and a keypress written during that window is queued and then discarded by the same call.

## Usage

```text
cclean [OPTION ...] ROOT [PATTERN ...]
```

`ROOT` defaults to the working directory. Patterns given on the command line are added to the built-in set, never substituted for it.

```text
cclean                                    # scan the working directory
cclean ./project                          # scan one project
cclean --dry-run ./project "*.log"        # add a pattern, remove nothing
cclean --no-defaults . "build/**"         # only what is named here
```

### Options

| Option | Effect |
|-|-|
| `-n`, `--dry-run` | List matches and exit without removing |
| `-b`, `--build-artifacts` | Also remove project build output (see below) |
| `--dependencies` | Also remove marker-guarded dependency trees |
| `-e`, `--exclude PATTERN` | Leave anything matching alone, contents included. Repeatable |
| `-v`, `--verbose` | Name every item as it is removed |
| `-y`, `--yes` | Remove without prompting |
| `--format FORMAT` | Select `human` (default) or `json` output |
| `--older-than DURATION` | Match only targets older than a duration (`s`, `m`, `h`, `d`, `w`) |
| `--larger-than SIZE` | Match only targets of at least a size (`B`, `K`, `M`, `G`, `T`) |
| `-h`, `--help` | Show usage |
| `-V`, `--version` | Show the version and exit |
| `--no-defaults` | Match only the patterns given on the command line |
| `--no-skip` | Descend into the protected directories listed below |
| `--` | Treat every later argument as `ROOT` or a pattern |

### Configuration

cclean searches from `ROOT` upward for the first `.cclean.toml`. It does not read configuration from outside that ancestor chain. CLI options override configuration values. CLI exclude patterns replace configured excludes when at least one `--exclude` is supplied; command-line patterns are added after configured patterns.

The supported schema is deliberately small and rejects unknown keys, duplicate keys, and invalid values:

```toml
patterns = ["*.tmp", "**/generated/**"]
excludes = ["fixtures", ".venv"]
defaults = true
build_artifacts = false
dependencies = false
skip_protected = true
dependency_markers = [["deps", "mix.exs"], ["vendor", "composer.lock"]]
project_roots = ["packages/api", "packages/worker"]
older_than = "30d"
larger_than = "100M"
```

`patterns`, `excludes`, and `project_roots` are arrays of strings; `dependency_markers` is an array of two-string arrays. The other values are booleans or quoted filter values. Configuration errors exit with status 2 before scanning starts.

The file is read by a small hand-written parser, not a TOML library, and it accepts a subset of TOML rather than all of it: one `key = value` per line, no tables, no multi-line arrays, and basic strings with no backslash escapes. Within that subset it is strict — a key may appear only once, an array element may not be empty (`[, "a"]` and `["a",,"b"]` are errors), a single trailing comma is allowed, and anything it does not understand is an error rather than a value quietly ignored. A `#` outside quotes starts a comment.

`dependency_markers` adds ecosystems the built-in list leaves out. Each entry is a directory name and the marker file that must sit beside it, both plain names rather than paths, since the marker is looked up in the directory's parent. A configured pair takes the same marker guard and the same `--dependencies` gate as a built-in, which is what distinguishes it from a pattern: `patterns = ["deps"]` matches any directory of that name, on every run.

Project roots let marker files identify package projects inside a monorepo. They are relative to the directory holding `.cclean.toml`, not to `ROOT`, so one repository-level file serves every `ROOT` beneath it; an entry outside the `ROOT` of a given run simply never matches.

## What it removes by default

```text
__pycache__   *.pyc   *.pyo   .*_cache   .DS_Store
```

`.*_cache` covers per-tool caches by shape, so `.pytest_cache`, `.mypy_cache`, `.ruff_cache` and `.tox_cache` all match without needing entries of their own.

## What it never touches

These directories are neither matched nor descended into:

```text
.git   .hg   .svn   .config   .ssh   .gnupg
```

They hold state managed by another tool. A name match inside them is far more likely to be a false positive than a cache, and a wrong deletion costs history, credentials, or a working environment. A `.pyc` inside `.git` is left alone, and no pattern can reach it. `--no-skip` walks them anyway.

Symbolic links are never followed. A matched link is removed as a link, and counts as zero bytes, since deleting it frees no file contents.

## Build artifacts

`--build-artifacts` removes a project's output directory. It is off by default because these directories are expensive to regenerate.

`.git` must sit beside the directory, and so must the marker file for its ecosystem. Several ecosystems build into the same directory name, so a name on its own is never enough.

| Removed | Marker file beside it |
|-|-|
| `build/` | `CMakeLists.txt`, `meson.build`, `package.json`, `build.gradle`, `build.gradle.kts`, `pyproject.toml`, `setup.py`, `pubspec.yaml` |
| `dist/` | `package.json`, `pyproject.toml`, `setup.py` |
| `target/` | `Cargo.toml`, `pom.xml` |
| `.next/`, `.nuxt/`, `.svelte-kit/`, `.turbo/`, `.parcel-cache/` | `package.json` |
| `.gradle/` | `build.gradle`, `build.gradle.kts` |
| `zig-out/`, `zig-cache/`, `.zig-cache/` | `build.zig` |
| `.build/` | `Package.swift` |
| `_build/` | `mix.exs` |

A marker only licenses the directory it is paired with: `package.json` beside a `target/` does not qualify it, and `Cargo.toml` beside a `dist/` does not either. A `build/` directory without a `.git` beside it is an ordinary directory and is left alone.

The directory must also be top-level in the outermost project. A submodule or a vendored checkout carries its own `.git` and marker file, so its `build/` would otherwise qualify on its own; if anything between it and `ROOT` is a repository, it is skipped. Name the nested project as `ROOT` to clean it directly.

`node_modules/` is deliberately absent. It is dependencies rather than build output, and restoring it needs the network, where everything above rebuilds offline. Remove it with a pattern if you want to: `cclean . node_modules`.

`--dependencies` removes trees that a package manager can fetch again. They are reported separately from build artifacts because restoring them may need network access, credentials, or post-install hooks.

| Removed | Marker file beside it | Restored by |
|-|-|-|
| `.venv/` | `uv.lock` | `uv sync` |
| `node_modules/` | `package-lock.json`, `npm-shrinkwrap.json` | `npm ci` |
| `node_modules/` | `yarn.lock` | `yarn install --immutable` |
| `node_modules/` | `pnpm-lock.yaml` | `pnpm install --frozen-lockfile` |
| `node_modules/` | `bun.lock`, `bun.lockb` | `bun install --frozen-lockfile` |
| `vendor/` | `go.mod` | `go mod vendor` |

The list is three ecosystems, not every ecosystem. A wrong entry deletes a tree that cannot be rebuilt, so it stays where the restore command is known and the layout is conventional. Other ecosystems are a pattern in `.cclean.toml`, which matches by name without a marker and so is the user's own judgement to make.

Each marker is the file that pins versions, not the one that declares them. `uv.lock` rather than `pyproject.toml`, which poetry, pdm and hatch also write and which sits beside pip-populated virtualenvs. `package-lock.json` rather than `package.json`, which carries ranges: only the lock names a tree, and only `npm ci` reinstalls it exactly. `go.mod` needs no companion, because minimal version selection makes it deterministic on its own.

`node_modules/` has a row per package manager because they share the install directory but not the lock name. A `node_modules/` with no lock beside it is left alone, since nothing on disk says what tree to put back.

bun has two entries because it changed format: `bun.lockb` is the binary lock it wrote before 1.2, `bun.lock` the text one it writes now. Both still install.

Lock names outside this list are a `dependency_markers` entry. Deno writes `deno.lock` beside a `node_modules/` when it manages one:

```toml
dependencies = true
dependency_markers = [["node_modules", "deno.lock"]]
```

Two things `--dependencies` does not preserve. Edits made directly inside a dependency tree are lost, since the manager rewrites it from the registry; patched `vendor/` source is the usual case. And for Go, the presence of `vendor/` is itself build configuration: removing it makes the build fall back to the module cache or the network.

The `.git` requirement is literal, so a monorepo is not detected unless its package roots are listed in `project_roots` in `.cclean.toml`.

This project now matches its own rule. Running `cclean -b` at the top of this repository will offer to delete `build/`, because `.git` and `CMakeLists.txt` sit beside it. That is correct, and `make` regenerates it.

## Excluding

`--exclude` keeps anything matching a pattern, and is repeatable.

```text
cclean . --exclude .venv --exclude "**/fixtures/**"
```

An exclude prunes: naming a directory keeps everything inside it, not just the directory itself. That is what makes `--exclude .venv` protect a virtual environment, which the built-in skip list does not. A `.venv` is only ever removed whole under `--dependencies`, and then only beside a `uv.lock`.

Excludes are tested the way command-line patterns are, against the path relative to `ROOT` and against the final path component. They apply to built-in patterns, command-line patterns, build artifacts, and dependency candidates alike.

## Patterns

Command-line patterns are tested against the path relative to `ROOT` and against the final path component. Built-in patterns are tested against the final component only, which is why `.*_cache` does not match `.venv/lib/foo_cache`.

| Wildcard | Matches |
|-|-|
| `*` | Any sequence of characters, `/` included |
| `?` | Any single character |
| `**/` | Zero or more directory levels |

A matched directory is removed whole. Its contents are not searched again, and never appear as separate entries in the list.

## Confirmation

The prompt takes a single keypress, with no Enter. `y` or `Y` proceeds; anything else cancels, end of input included. Typed-ahead keystrokes are discarded, so a stray keypress from before the prompt cannot answer it.

When standard input is a pipe, a character is read from it instead, so
`echo y | cclean .` works in scripts.

`--yes` skips confirmation and is intended for explicitly unattended runs. It never changes the default behavior.

## Output

Progress appears on standard error during long scans and is erased when they finish. Nothing is drawn for the first 80 milliseconds, so quick runs stay silent. The target list goes to standard output, so redirecting it leaves a clean file.

Colour is used for directories, totals, and failures. It is disabled automatically when output is not a terminal, and when `NO_COLOR` is set.

The matched list is printed once. After confirmation you get a one-line summary; only failures are named again. `--verbose` restores the per-item log.

`--format json` writes one JSON document to standard output. It includes the root, status, matched targets, logical byte totals, per-reason statistics, and warnings. Progress, prompts, and other diagnostics go to standard error. Each target includes a `reason`: `default`, `config`, `command-line`, `build-artifact`, or `dependency`.

### Filenames in output

A POSIX filename is a byte string: it can contain control characters, and it need not be valid UTF-8. Both forms of output account for that, differently.

In human output, every C0 control character and DEL is shown as `\xNN` and a literal backslash is doubled. This is what stops a name containing a newline from forging an entry in the matched list, or one containing an escape sequence from erasing an entry already printed — the list is what you read before confirming a permanent deletion. Bytes above `0x7f` are passed through, so ordinary non-ASCII names display normally.

In JSON output, the document is always valid UTF-8 and always parseable. Bytes that are not valid UTF-8 are replaced with U+FFFD, one per byte, so a `path` may be a lossy rendering of the real name; the human output is the faithful form. Control characters are `\u`-escaped as JSON requires.

### Exit codes

| Code | Meaning |
|-|-|
| 0 | Scan completed; removal succeeded, nothing matched, cancellation, or dry run |
| 1 | `ROOT` cannot be inspected, a scan warning occurred, or a removal failed |
| 2 | Bad option, invalid configuration, or no patterns left to match |

## Sizes

Reported sizes are logical file sizes, not disk usage. They ignore block rounding, sparse files, and filesystem compression, so the space actually reclaimed may differ.

`--older-than` drops targets younger than the duration. Durations use `s`, `m`, `h`, `d`, or `w`. For directories, the newest entry in the directory determines its age. For a symlink, the link's own timestamp is used, never its target's — the same no-follow rule the rest of the scan applies. `--larger-than` keeps only targets at or above the given logical size. Sizes use bytes by default, or binary `K`, `M`, `G`, or `T` suffixes.

Both filters accept a decimal value with an optional fractional part, converted exactly and truncated toward zero. A value that does not fit, or that carries an exponent, a sign, or any trailing character, is an error rather than a silently different limit.

Directories are sized in parallel across all cores. The unit of work is a single directory level rather than a whole target, so one large `node_modules` parallelizes as well as a thousand small `__pycache__` directories.

The tree walk uses the same work queue, so listing directories is spread across cores too. On a 65,101-entry tree that took the walk from 102 ms to 49 ms, and a full dry run from 88 ms to 61 ms, on 8 cores.

## How deletion works

Removal is descriptor-relative. The parent directory of a reviewed target is opened once, the entry is confirmed to still have the type it had when the list was shown, and every removal names an entry within a directory descriptor rather than re-resolving a path. Directories are emptied by opening each level with `O_NOFOLLOW` and unlinking within it, so a symlink substituted for a directory mid-run is an error rather than a way out of the tree, and a component renamed after the check cannot be reached by name at all.

Symlinks are unlinked, never followed. A symlink matched as a target removes the link itself and leaves whatever it pointed at alone.

## Limitations

- Unreadable directories are reported as warnings and make the command exit with status 1. The root itself must be readable.

- A removal that is already in progress cannot be undone by a change made underneath it. The identity check happens once, before the walk down a matched directory begins; entries created inside that directory while it is being emptied are removed along with the rest.

- Pointing `ROOT` directly at a protected directory scans it. The skip list applies to entries found during the walk, not to `ROOT` itself.
