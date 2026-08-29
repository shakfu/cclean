# cclean

Finds and removes build caches and editor debris from a directory tree. It lists everything it matched, with sizes, and waits for confirmation before deleting anything.

```
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

```
Permanently remove? [y/N] y
Removed 4, 1.43 MiB reclaimed
```

## Build

The build is defined by `CMakeLists.txt`. The `Makefile` is a frontend over it,
so either entry point works.

```
make                    # configure and build into build/
make install            # install to /usr/local, stripped
make clean              # remove build/
```

```
cmake -S . -B build
cmake --build build --parallel
cmake --install build --strip
```

Both honour the usual overrides:

```
make BUILD=out BUILD_TYPE=Debug
make install PREFIX=$HOME/.local
```

The version reported by `cclean --version` comes from `project(... VERSION ...)`
in `CMakeLists.txt`, passed to the compiler as a definition. A build made
outside CMake reports `unknown`.

Requires CMake 3.16 and a C++17 compiler. No dependencies beyond the standard
library and pthreads. POSIX only: the confirmation prompt uses `<termios.h>`.

## Tests

```
make test               # or: ctest --test-dir build --output-on-failure
```

Two suites registered with CTest, neither with a dependency of its own:

- `tests/unit.cpp` covers the glob matcher, size formatting, saturating addition, the skip list, pattern scope, and build-artifact detection. It includes `src/cclean.cpp` directly and renames its entry point, which is how it reaches static functions without splitting the program into a library.

- `tests/cli.sh` drives the built binary: exit codes, what is actually deleted, confirmation handling, the skip list end to end, build artifacts, symlinks, and the shape of the output.

Both exit non-zero on failure, so `make test` fails the build. The terminal branch of the confirmation prompt, which takes a single keypress in raw mode, needs a pseudo-terminal and is not covered.

## Usage

```
cclean [OPTION ...] ROOT [PATTERN ...]
```

`ROOT` defaults to the working directory. Patterns given on the command line are added to the built-in set, never substituted for it.

```
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
| `-e`, `--exclude PATTERN` | Leave anything matching alone, contents included. Repeatable |
| `-v`, `--verbose` | Name every item as it is removed |
| `-h`, `--help` | Show usage |
| `-V`, `--version` | Show the version and exit |
| `--no-defaults` | Match only the patterns given on the command line |
| `--no-skip` | Descend into the protected directories listed below |
| `--` | Treat every later argument as `ROOT` or a pattern |

## What it removes by default

```
__pycache__   *.pyc   *.pyo   .*_cache   .DS_Store
```

`.*_cache` covers per-tool caches by shape, so `.pytest_cache`, `.mypy_cache`, `.ruff_cache` and `.tox_cache` all match without needing entries of their own.

## What it never touches

These directories are neither matched nor descended into:

```
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

`node_modules/` is deliberately absent. It is dependencies rather than build output, and restoring it needs the network, where everything above rebuilds offline. Remove it with a pattern if you want to: `cclean . node_modules`. A separate `--dependencies` flag is in `TODO.md`.

The `.git` requirement is literal, so a monorepo is not detected: a crate at `repo/rust-app/` with the repository's `.git` one level up will not match.

This project now matches its own rule. Running `cclean -b` at the top of this
repository will offer to delete `build/`, because `.git` and `CMakeLists.txt`
sit beside it. That is correct, and `make` regenerates it.

## Excluding

`--exclude` keeps anything matching a pattern, and is repeatable.

```
cclean . --exclude .venv --exclude "**/fixtures/**"
```

An exclude prunes: naming a directory keeps everything inside it, not just the directory itself. That is what makes `--exclude .venv` protect a virtual environment, which the built-in list no longer does.

Excludes are tested the way command-line patterns are, against the path relative to `ROOT` and against the final path component. They apply to built-in patterns, command-line patterns, and `--build-artifacts` alike.

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

## Output

Progress appears on standard error during long scans and is erased when they finish. Nothing is drawn for the first 80 milliseconds, so quick runs stay silent. The target list goes to standard output, so redirecting it leaves a clean file.

Colour is used for directories, totals, and failures. It is disabled automatically when output is not a terminal, and when `NO_COLOR` is set.

The matched list is printed once. After confirmation you get a one-line summary; only failures are named again. `--verbose` restores the per-item log.

### Exit codes

| Code | Meaning |
|-|-|
| 0 | Removed cleanly, or nothing matched, or cancelled, or dry run |
| 1 | `ROOT` is missing or is not a directory, or a removal failed |
| 2 | Bad option, or no patterns left to match |

## Sizes

Reported sizes are logical file sizes, not disk usage. They ignore block rounding, sparse files, and filesystem compression, so the space actually reclaimed may differ.

Directories are sized in parallel across all cores. The unit of work is a single directory level rather than a whole target, so one large `node_modules` parallelizes as well as a thousand small `__pycache__` directories.

The tree walk uses the same work queue, so listing directories is spread across cores too. On a 65,101-entry tree that took the walk from 102 ms to 49 ms, and a full dry run from 88 ms to 61 ms, on 8 cores.

## Limitations

- Unreadable directories are skipped silently rather than reported.

- Pointing `ROOT` directly at a protected directory scans it. The skip list applies to entries found during the walk, not to `ROOT` itself.

