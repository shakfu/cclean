# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.1]

### Added

- `--color WHEN`, one of `auto` (the default), `always` or `never`. Colour was decided by the terminal check and `NO_COLOR` alone, so a run piped to `less -R` could not keep it and a single run could not drop it without setting an environment variable. `always` overrides both, since a flag typed for one run is the more specific instruction.

- A breakdown by reason under the total in human output, printed when a run matched more than one. The total says how much, not how much of what, and the reasons differ in what it costs to undo the removal: a dependency tree needs the network, a build artifact rebuilds offline, a cache is free. JSON has carried the same split as `stats` since the format existed. Rows follow the order the reasons are declared, cheapest to restore first, so a report does not reorder itself as a tree changes.

- `--config FILE` and `--no-config`. The upward search for `.cclean.toml` does not stop at the project, so a file in a home directory supplied `patterns`, `build_artifacts`, `dependencies` and `skip_protected` to every run beneath it, with nothing in the output naming the file responsible -- and `skip_protected = false` in a forgotten ancestor turned off the protection on `.git`, `.ssh` and `.gnupg` for every run after it. Neither of the new options depends on what sits above the checkout, which is what a scripted or CI run needs. `--verbose` now names the file a run used, and JSON output carries it as `config`.

- The command that restores a dependency tree, printed beside the target and carried as `restore` in JSON: `uv sync`, `npm ci`, `yarn install --immutable`, `pnpm install --frozen-lockfile`, `bun install --frozen-lockfile`, `go mod vendor`. It was documented in the README and nowhere the user would see it while deciding whether to confirm the removal of something only the network can rebuild. It is now a field of the built-in table, so a new row cannot be added without answering the question. A pair configured through `dependency_markers` carries none: only the user knows what puts it back.

- Exit status 3, for a run that completed with warnings. Status 1 was a root that could not be read, a directory the walk could not descend, and a removal that failed, all at once, so a script could not tell "cleaned everything, but one directory was unreadable" from "tried to delete and could not" -- a distinction the JSON document has always made. 1 now belongs to the failures.

- A note on standard error when an operand that names a directory is read as a pattern. `ROOT` is one directory and everything after it is a glob, so `cclean a b` scans `a` and silently turns `b` into a pattern; the same rule makes `cclean projects/api build` match every `build` under that root, without the marker file and the `.git` that `--build-artifacts` requires. It is a note rather than an error because the same shape is documented use: `cclean . node_modules` is how the README says to remove a tree the dependency rules will not.

- `"schema": 1` in the JSON document, so a consumer can refuse a shape it does not know rather than read a renamed field as a missing one.

- `-d` as a short form of `--dependencies`, matching the short options the other mode flags already carry.

- `remove_targets()`, which removes a reviewed list across the same worker pool the scan uses, and which the frontend now calls instead of looping over `remove_target()`. Removal is one `unlinkat` per entry and was the entire cost of a run: over a 134,400-file tree with 2,400 targets, the walk, the sizing and the sort took 30 ms together while removing them took 940 ms, so every previous round of parallel tuning had gone into 3% of the wall clock. The same run now takes 270 ms, which is what `xargs -P8 rm -rf` reaches over the same targets. The unit of work is one target rather than one directory level as it is when sizing: emptying a single target across threads means passing a directory descriptor between them and unlinking the directory only once every worker below it has finished, which the flat work queue cannot express and which this path, where the descriptor discipline is the safety property, is the wrong place to try. Results stay in the order the list was reviewed in, so neither the failure lines nor a `--verbose` log depends on which worker drew which target.

- `libcclean`, a static library holding everything that finds, sizes and removes, with `cclean` reduced to a frontend over it. `make install` now installs `lib/libcclean.a` and the headers under `include/cclean/` beside the binary. The entry point is `scan()`, which walks a root, sizes matched directories, applies the age and size filters, and returns the targets sorted by path; `remove_target()` is the deletion boundary. The library touches no terminal, prints nothing of its own, and reads no environment variable.

- A CI workflow: GCC and Clang on Linux and Clang on macOS, and both suites under AddressSanitizer with UndefinedBehaviorSanitizer and under ThreadSanitizer.

- Regression coverage for everything below: numeric limits at zero, at the maximum, and one past it; marker directories, FIFOs, symlinks, broken symlinks, and `.git` as a file; symlink timestamps; filenames containing newlines, escape sequences, and invalid UTF-8; malformed and duplicated configuration keys; and the terminal confirmation branch, driven through a pseudo-terminal. The suites go from 178 to 327 unit checks and from 136 to 212 command-line checks; all but four of the latter run under root, which cannot be denied the permissions two of them need.

### Changed

- A target's `type` in JSON output is `directory`, `file` or `symlink`, where it was only the first two. A symlink is not a kind of file here: it is unlinked without being followed and counts as zero bytes, so a consumer summing sizes or re-checking a path before acting on the document could not see the one distinction that changes what a removal means.

- `remove_target()` takes the root the scan was given, since resolving a target one component at a time from a known directory is what the fix above needs. A target that is not at or below that root is refused rather than reached through `..`.

- `ScanResult` carries the root it was scanned with, and `remove_targets(result)` is the form to prefer: it is the one call that cannot pair a result with the wrong root. `remove_targets(root, targets)` remains for a caller removing a list it filtered or built itself. `write_json()` reads the root from the result rather than taking it again, so there is one source of truth for it rather than two that can disagree.

- CI promotes warnings to errors. The build job's comment said it already did; nothing in the repository set `-Werror`. It is set on the CI configure line rather than in `CMakeLists.txt`, so a new compiler version still cannot break an ordinary local build.

- The program is split into `include/cclean` and `src` for the library, `cli` for the frontend, and one module per concern rather than a single 2,900-line translation unit. Command-line output is byte-identical across the flag, config, JSON, filter and error paths. The reason a target reports is carried on the pattern that matched it, replacing index arithmetic against two running counts that every caller had to keep in step with the order the patterns were built in. The unit suite links the library instead of including its source and renaming `main`.

- The two command-line checks that need a permission denial are skipped under root, and the CI job that ran the suites as a separate unprivileged user is gone. Root holds `DAC_OVERRIDE` and `DAC_READ_SEARCH`, so the bits those checks set deny it nothing, and since both assert exit status 1 they reported a failure rather than testing anything. The job could not have caught that: `ubuntu-latest` already runs every step as a non-root user, so it only repeated the ordinary build jobs. The guard belongs in the suite, where it also covers `make test` inside a root container.

- The configuration parser rejects duplicate keys and empty array elements. Values were appended, so a key repeated by a bad merge widened what a run deleted, and every comma before the next value was skipped, so `patterns = [, "*.tmp"]` and `["*.a",,,"*.b"]` both parsed. A single trailing comma is still accepted, as TOML accepts it. Nested arrays are parsed structurally rather than by searching for the next `]`, so a marker name containing that character is no longer rejected.

- The worker pool is capped at 32 threads, and a thread that cannot be created is no longer fatal. The pool was sized one per core, so a many-core host made 127 threads for a scan with work for a handful. Growing the pool to match the work instead, which is the obvious answer, was implemented, measured, and rejected: in every form tried it cost 6 to 8 percent of a scan of a real tree, because the state the growth step needs stays live across a very tight loop. The reasoning is recorded beside the code so it is not retried blind.

- Performance is unchanged on scanning and startup, and 2 to 3 percent slower on deletion. Scans measured within noise of the previous release over a 32,000-entry tree (+1.8% sizing-heavy, -3.6% full walk, +2.2% every file matched). Deletion pays 2 to 3 percent for resolving each level through a descriptor instead of a path, which is what closes the race above; the unlink is attempted before anything is known about an entry, so the common case costs one syscall where a stat first cost two.

- A target whose parent cannot be opened, or whose own status cannot be read, reports the underlying error. Every failure at that point previously read `Target changed or disappeared`, which is now reserved for the case where it actually did.

- The README states the contracts these changes settle: the accepted configuration subset rather than an implication of full TOML, how filenames are escaped in each output format, that a symlink is filtered on its own timestamp, and how deletion resolves a path.

### Fixed

- `--help` writes to standard output. It went to standard error, so `cclean --help | less` showed an empty screen and `cclean --help > usage.txt` wrote an empty file, while `--version` already used standard output. Usage printed after a rejected argument stays on standard error, where it is a diagnostic rather than the thing asked for.

- The no-follow property of the deletion path stopped at the target's parent. That directory was opened by its whole path, which resolved every component by name after the scan had listed them and before the identity check could mean anything, so an interior directory replaced by a symlink in that window sent the removal wherever the link pointed -- while every open below the parent already refused to follow one. Reproduced by calling `remove_target()` with a target under a symlinked interior component: the file outside the tree was removed and the call reported success. Every component from the root down is now opened with `O_NOFOLLOW` through the descriptor above it, and a component that has become a symlink is named as such rather than reported as "Not a directory". The root itself is still opened by name and followed, because the user typed it. It costs nothing measurable: a run over the benchmark tree takes the same 270 ms it did before the walk.

- The pseudo-terminal test driver reads the pty continuously instead of sleeping before it answers, which deadlocked `make test` on macOS. `tcsetattr(TCSAFLUSH)` drains output first, and on a pty that blocks until the master reads; the keypress written during that window was queued, then discarded by the same call, and the program waited on an answer that no longer existed. The driver also has a deadline, so a real regression fails rather than hangs.

- `touch -h -d` in the command-line suite uses the ISO 8601 `T` form, which BSD `touch` requires and GNU `touch` accepts. The symlink-timestamp check could not run on macOS.

- `--older-than` and `--larger-than` no longer perform an out-of-range floating-point conversion. Both parsed through `double` and range-checked against `static_cast<double>` of the integer maximum; neither maximum is representable, so the bound rounded up to the next power of two, and a value written at the boundary passed the check and then reached a conversion that is undefined in C++. In practice the limit came out as zero, so `--larger-than 18446744073709551615B` matched every file instead of none, which is the opposite of what a filter used as a safety boundary before deletion is for. Both are now converted exactly, as fixed-point decimals truncated toward zero, and a value that does not fit is rejected. An exponent, a sign, or any trailing character is likewise an error rather than a silently different limit.

- The age comparison no longer overflows. `now - limit` is not representable on a clock counting nanoseconds in 64 bits once the limit is measured in centuries; the comparison is made in whole seconds and saturates.

- `--older-than` judges a symlink by the link's own timestamp rather than its target's. The scan treats a symlink as the object throughout, reporting it at zero bytes and never descending into it, but the timestamp query resolved the link: a link whose own mtime was years old was kept out of the list because the file it pointed at, possibly outside the scanned tree entirely, had been touched today.

- Failed filesystem status queries are reported instead of being read as "not a directory" or "not a regular file". A permission error, or an entry disappearing mid-walk, could drop a whole subtree from a directory's size in silence, or carry a target forward on an incomplete assessment, against the documented behaviour that an unreadable path is named and the run exits 1.

- JSON output is always valid UTF-8. A POSIX filename need not be, and the bytes were copied through unchanged, so one undecodable byte in one name made the entire document unparseable, totals and warnings included. Invalid bytes become U+FFFD, one per byte, so a `path` may be a lossy rendering of the real name; the human output remains the faithful form.

- Removal failures appear in the JSON `warnings` array. Only pre-delete validation failures were recorded there; an error from the removal itself went to standard error alone and left no trace in the document.

- A failed terminal restoration is reported rather than ignored, and restoration happens on every path out of the prompt, the destructor included. Both `tcsetattr()` results were discarded, and a terminal left in raw no-echo mode is invisible until the user types the next command and sees nothing back.

### Security

- Deletion is descriptor-relative and never follows a symlink below `ROOT`. Validation checked a path and the removal then re-resolved the same path, and `remove_all()` re-resolved every component at every level of the subtree, so a concurrent process could put a different object of the same type at a reviewed path between the two lookups. The target's parent is now opened once and the type check runs against that descriptor; each level below is opened with `O_NOFOLLOW` and its entries unlinked within it, so a component swapped after the check cannot be reached by name and a symlink substituted for a directory is an error rather than a way out of the tree. The parent itself is still opened by path, because `ROOT` may legitimately have been reached through a symlink and the user named it.

- Marker guards require a regular file. `exists()` follows symlinks and is satisfied by a directory or a FIFO, so a directory named `CMakeLists.txt`, or a symlink pointing at an unrelated file elsewhere, could license the removal of a build directory in a tree holding none of the project metadata the guard exists to prove. `.git` still accepts a directory or a file, since a working tree carries the former and a submodule or linked worktree the latter, but no longer a symlink.

- Control characters in filenames are escaped in human output. The matched list is what the user reads before confirming a permanent deletion: a name containing a newline could forge an entry in it, and one containing a terminal escape sequence could erase an entry already printed. Every C0 control and DEL is shown as `\xNN`, and a literal backslash is doubled so the escape is unambiguous.

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

[0.2.1]: <https://github.com/shakfu/cclean/releases/tag/0.2.1> [0.2.0]: <https://github.com/shakfu/cclean/releases/tag/0.2.0> [0.1.1]: <https://github.com/shakfu/cclean/releases/tag/0.1.1> [0.1.0]: <https://github.com/shakfu/cclean/releases/tag/0.1.0>
