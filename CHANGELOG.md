# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0]

### Added

- `.cclean.toml` configuration, discovered upward from `ROOT`, with strict schema validation. CLI options override configuration values, while command-line patterns extend configured patterns.

- `--format json` for machine-readable reports containing matched targets, match reasons, totals, per-reason statistics, warnings, and removal status. JSON stays on standard output; prompts and diagnostics use standard error.

- `-y, --yes` for explicit unattended deletion. The default confirmation prompt remains unchanged.

- `--older-than` and `--larger-than` filters, with equivalent configuration values. Directory age uses the newest entry in the target.

- `--dependencies` for marker-guarded `.venv` beside `uv.lock`, `node_modules` beside `package-lock.json`, and `vendor` beside `go.mod`. Dependency targets remain separate from rebuildable artifacts. Three ecosystems rather than every ecosystem: a wrong entry deletes a tree that cannot be rebuilt, and other ecosystems are a pattern in `.cclean.toml`. `uv.lock` over `pyproject.toml`, which poetry, pdm and hatch also write and which sits beside pip-populated virtualenvs, and a lock file over `package.json`. `node_modules` carries one row per package manager, `package-lock.json`, `npm-shrinkwrap.json`, `yarn.lock`, `pnpm-lock.yaml`, `bun.lock` and `bun.lockb`, because they share the install directory but not the lock name.

- `dependency_markers` in `.cclean.toml`, an array of `[directory, marker]` pairs, for ecosystems the built-in list leaves out. A configured pair takes the same marker guard and the same `--dependencies` gate as a built-in; a pattern would match the directory name on every run, unguarded.

- Explicit `project_roots` configuration for marker-guarded artifacts inside monorepos. Entries resolve against the directory holding `.cclean.toml`, so one repository-level file serves every `ROOT` beneath it.

### Changed

- Scan warnings now produce exit status 1. Status 2 covers invalid options, invalid configuration, and runs with no patterns.

- Unreadable paths are reported instead of skipped silently. Targets are revalidated before deletion to reject paths whose type changed after scanning.

- `make` decides what to recompile from a checksum of the sources rather than from mtimes alone. CMake's Makefile generator compares timestamps at one-second granularity, so an edit landing in the same second as the previous build was invisible and `make test` then exercised a stale binary: an edit-then-build loop missed 3 of 10 changes.

### Fixed

- An unreadable modification time no longer warns or sets exit status 1 on its own. The value is consulted only by `--older-than`, which reports `Cannot apply age filter` for the same target and drops it, so the earlier warning fired for a value the run never read and turned successful runs without the filter into status 1.

- A throwing scan callback in the parallel walk and sizing driver no longer deadlocks or aborts. Every scan takes the `error_code` overloads and so does not throw today, but a throw skipped the active-worker decrement, leaving a count that could never reach zero and the remaining workers parked on it, and then escaped a thread function into `std::terminate`. The first exception is now captured, the walk abandoned, and the exception rethrown to the caller once the pool is joined.

## [0.1.1]

### Added

- `-V, --version`. The number comes from `project(... VERSION ...)` in `CMakeLists.txt` and reaches the program as a compile definition, so it is written in one place. A build made outside CMake reports `unknown` rather than a number that would have to be kept in step by hand. The version does appear twice, here and in `CMakeLists.txt`, so a command-line test compares `--version` against the newest entry in this file.

- `-e, --exclude PATTERN`, repeatable. An exclude prunes rather than only suppressing the match, so naming a directory keeps everything under it. rclean's equivalent suppresses only, which leaves the contents of an excluded directory still eligible; pruning is what lets `--exclude .venv` restore the protection this release removes. Excludes are scoped like command-line patterns and apply to built-in patterns, command-line patterns and `--build-artifacts` alike.

- `--build-artifacts` covers 25 directory and marker pairs, up from 2: JavaScript and TypeScript (`dist`, `build`, `.next`, `.nuxt`, `.svelte-kit`, `.turbo`, `.parcel-cache`), JVM (`target`, `build`, `.gradle`), Python (`build`, `dist`), Zig, Swift, Elixir, Dart, and Meson alongside the existing CMake and Cargo. A marker licenses only the directory it is paired with, so `package.json` beside a `target/` does not qualify it. `node_modules` is left out: it is dependencies rather than build output, and restoring it needs the network.

### Fixed

- `--build-artifacts` matched build directories inside nested repositories. A git submodule or a vendored checkout carries its own `.git` and marker file, so it satisfied the test on its own: on one project this took `lib/DaisySP/build`, `lib/DaisySP/DaisySP-LGPL/build` and `lib/libDaisy/build` alongside the intended top-level `build`. An artifact directory now also requires that nothing between it and `ROOT` is a repository. Naming the nested project as `ROOT` still cleans it.

### Changed

- `.venv` and `venv` are no longer protected, leaving `.git`, `.hg`, `.svn`, `.config`, `.ssh`, `.gnupg`. A virtual environment holds the largest concentration of `__pycache__` in a typical Python project, so protecting it gave up most of what a default run is for, and recovering it needed `--no-skip`, which also unprotects `.git` and `.ssh`. The rest of the list is state managed by another tool, where a name match is likelier to be a false positive than a cache; a virtual environment's caches are ordinary Python caches.

## [0.1.0]

First release. See `README.md` for usage.

### Added

- Removal by glob pattern. Every match is listed with its size before a confirmation prompt. `--dry-run` stops after the listing. Patterns given on the command line are added to the built-in set; `--no-defaults` uses only what is given.

- Built-in patterns: `__pycache__`, `*.pyc`, `*.pyo`, `.*_cache`, `.DS_Store`.

- Protected directories, neither matched nor descended into: `.git`, `.hg`, `.svn`, `.venv`, `venv`, `.config`, `.ssh`, `.gnupg`. `--no-skip` walks them.

- `--build-artifacts` removes `build/` in a CMake project and `target/` in a Cargo one.

- `--verbose` to name each removal, `--help`, and `--` to end option parsing.

- Unit and command-line test suites, run by `make test` or `ctest`.

- CMake build, with the Makefile as a frontend over it.

### Decisions worth recording

Built-in patterns are matched against the final path component only, where command-line patterns are matched against that and against the path relative to `ROOT`. Matching the built-ins against the path made `.*_cache` mean "anything under a dot-directory ending in `_cache`", which took `.venv/lib/foo_cache` and `.git/objects/blob_cache` with it.

`--build-artifacts` requires `.git` and the marker file, `CMakeLists.txt` or `Cargo.toml`, in the same directory as the artifact directory. Matching `build/` and `target/` by name alone would take any directory with those names. The cost is that a monorepo, whose `.git` sits above the crate, is not detected.

Glob matching is open-coded rather than translated to `std::regex`. Regex was 257 ms of a 414 ms scan over 64,201 entries, and libc++ threw an uncaught `regex_error` from `regex_match`, not from the constructor, on a pattern with a long run of `*`, so the guard around pattern construction never saw it and `cclean . "****a"` aborted. The replacement simulates the pattern rather than backtracking, which also bounds `*a*a*a*a*b` against a long name at tokens x length.

A matched directory is removed whole and is not descended into, so its contents appear in neither the walk nor the listing. Counting a directory and the matched files inside it separately doubles the reported total.

[0.2.0]: <https://github.com/shakfu/cclean/releases/tag/0.2.0> [0.1.1]: <https://github.com/shakfu/cclean/releases/tag/0.1.1> [0.1.0]: <https://github.com/shakfu/cclean/releases/tag/0.1.0>
