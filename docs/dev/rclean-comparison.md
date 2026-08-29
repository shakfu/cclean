# cclean compared to rclean

Both tools remove development debris by glob pattern, with a dry-run mode, a confirmation prompt, a parallel walk, parallel directory sizing, and a set of directories they refuse to enter. They diverge on configurability, dependencies, and size. rclean is the larger and more configurable tool; cclean is the faster and smaller one.

Measurements were taken on 2026-08-29, macOS on 8 cores (4 performance, 4 efficiency), against cclean built with `make build` and rclean 0.4.0 built with `cargo build --release`.

This document has been revised twice as both tools changed. The first revision measured rclean 0.3.0 and reported it nine times slower, with a doubled size estimate and no protected directories; rclean 0.4.0 fixed all three and took the speed lead. This revision follows cclean replacing its serial `fs::recursive_directory_iterator` with a parallel walk, which takes the lead back. The table near the end records what changed.

All figures below were re-measured in one session against both current binaries. rclean's walk came out slower here than the previous revision recorded (78 ms against 61 ms), so read these as same-session relative numbers rather than as absolutes comparable across revisions.

## Summary

| | cclean | rclean |
|-|-|-|
| Language | C++17, one file | Rust, `src/` plus `tests/` |
| Source | 1139 lines, plus 825 of tests | 1487 lines, plus 1188 of tests |
| Dependencies | none | 12 runtime, 2 dev |
| Binary, stripped | 93,680 bytes | 1,470,880 bytes |
| Clean build | 1.1 s | 17.9 s |
| Automated tests | 16 unit functions (138 checks), 47 CLI checks | 69 tests |
| Dry run, 65,101 entries | 61 ms | 78 ms |
| Tree walk | parallel | parallel |
| Directory sizing | parallel | parallel |
| Protected directories | yes, fixed list | yes, replaceable list |

## Where cclean wins

### Speed

On a 65,101-entry tree holding 1,800 `__pycache__` directories.

Median of 15 runs, warm cache, both tools measured in the same session:

| | median | fastest |
|-|-|-|
| cclean `-n` | 61 ms | 57 ms |
| rclean `-d` | 79 ms | 75 ms |
| rclean `-d -y -q` | 78 ms | 75 ms |

Measured on its own, with a pattern that matches nothing so that neither tool prunes or sizes and both list all 65,101 entries:

| | median | fastest |
|-|-|-|
| cclean `-n --no-defaults tree nomatchxyz` | 49 ms | 47 ms |
| rclean `-d -q -g '**/nomatchxyz'` | 78 ms | 76 ms |

Both tools now spread directory listings across cores, and both stop descending once a directory matches. cclean's remaining margin comes from its glob matcher, which is open-coded, with exact, prefix, and suffix patterns short-circuiting to a string compare, where rclean compiles patterns through `globset`. That per-entry saving shows up on every one of the 65,101 entries.

For the record, cclean's walk before it was parallelized:

| | median |
|-|-|
| serial walk, whole tree | 102 ms |
| parallel walk, whole tree | 49 ms |
| serial full run | 88 ms |
| parallel full run | 61 ms |

The walk itself got 2.1x faster, and the full run 1.4x, on 8 cores. The full run gains less because sizing was already parallel, and because pruning 1,800 `__pycache__` directories takes 25,200 entries out of the walk before it starts.

### Project-aware build artifacts

cclean removes `build/` only when `.git` and `CMakeLists.txt` sit beside it, and `target/` only when `.git` and `Cargo.toml` do. The check is opt-in behind `--build-artifacts`.

rclean's `rust` preset is the single pattern `**/target`, and its `java` preset includes `**/build` and `**/target`. Neither consults the surrounding directory, so any directory named `target` matches wherever it appears.

The tradeoff is real: cclean's rule is stricter and misses monorepos, where the repository's `.git` sits above the crate. rclean would catch those, along with everything else named `target`.

### Confirmation

cclean takes a single keypress with no Enter, and discards typed-ahead input so a stray keystroke cannot answer the prompt. rclean uses a line-based prompt via `dialoguer`.

### Dependencies

cclean has no dependency tree to audit or keep current. rclean pulls in 12 crates, down from 14. For a tool that runs `remove_all` on paths it chose itself, that is a meaningful difference in what has to be trusted, though the crates involved are widely used.

### Size and build time

93,680 bytes against 1,470,880, and 1.1 s to build from clean against 17.9 s.

## Where rclean wins

### Tests

rclean has 69 tests: 68 across 10 files, plus one doc-test. They cover deletion, patterns, symlinks, path traversal, protected directories, relative paths, age filtering, config discovery, directory sizing, and size formatting.

cclean has 16 unit test functions holding 138 assertions, plus 47 command-line checks, run by `make test`. The counts are not comparable, since rclean's 69 are test functions and cclean's 138 are individual assertions.

Coverage differs in kind. cclean's unit tests go deeper on the glob matcher, including that `**/` does not degrade to a bare `*`, that regex metacharacters stay literal, and that matching a pathological pattern terminates. rclean covers ground cclean has no equivalent for: path traversal, age filtering, and config discovery, because cclean has none of those features.

Both suites were mutation-tested while writing this: 13 deliberate defects were introduced into cclean and each was caught. The gap that mutation testing exposed, and that is now closed, was a defaults test that hardcoded a pattern string instead of reading `defaults::patterns`, so a typo in the array passed unnoticed.

Two things cclean still does not cover: the terminal branch of the confirmation prompt, which needs a pseudo-terminal, and the parallel sizing path under contention, which is exercised only indirectly.

### Configuration and presets

rclean has named presets for python, node, rust, java, c, go, and common, plus an `all` that merges them. It reads `.rclean.toml`, searching upward from the working directory and falling back to `~/.config/rclean/`, and can write a default config with `-w`. CLI flags override file values.

cclean has five built-in patterns, extra patterns as arguments, and `--no-defaults`. There is no config file and no way to save a pattern set.

### Exclude patterns

rclean takes `--exclude` globs. cclean has only its protected list, so a pattern that matches too much cannot be narrowed except by rewriting it.

### Features cclean does not have

- `--older-than`, to restrict removal to files past an age.

- `--format json`, for scripting.

- Shell completions for five shells.

- `--stats`, a per-pattern breakdown.

- `--skip-confirmation`, for unattended runs. cclean always prompts unless `--dry-run` is given, so it cannot run in a script that deletes.

- Broken-symlink removal.

- Published on crates.io, with a changelog and an MIT license file. cclean has neither a license nor a release process.

## Where they now agree

### Protected directories

Neither tool matches or descends into `.git`, `.hg`, `.svn`, `.venv`, `venv`, `.config`, `.ssh`, or `.gnupg`. rclean 0.4.0 adopted cclean's list.

Given a tree with a `.pyc` file in each of `.git/objects`, `.venv/lib`, `.ssh`, and `src`, plus a `src/__pycache__`, both match the same two items under `src`. Both also agree on two edges:

- A protected directory named as the root is entered. Pointing either tool at `.git` is deliberate.

- Protection is by name, whatever the entry type, so the `.git` *file* that marks a submodule is protected too.

They differ only in how the list is overridden. cclean's is fixed, with `--no-skip` to disable it wholesale. rclean's `--no-protect` does the same for one run, and `protected_dirs` in a config file replaces the list, so a project can protect names of its own.

### Size estimates

On the same tree, which holds 17,640,000 bytes of `.pyc` files:

| | reported |
|-|-|
| on disk | 1,800 directories, 16.82 MiB |
| cclean | 1,800 targets, 16.82 MiB |
| rclean | 1,800 items, 16.82 MiB |

rclean 0.3.0 reported 27,000 items and 33.65 MiB here, because it counted a matched directory and then descended into it and counted the matched files inside it again.

## Defects found while testing, since fixed

All were in rclean 0.3.0 and were reported here because they affected the comparison, not as a general audit. All are fixed in 0.4.0.

| defect | fix |
|-|-|
| Default patterns reached inside `.git`, `.ssh`, `.venv` | Protected directory list, `--no-protect` to disable |
| Size estimate doubled, item count 15x | The walk stops descending at a matched directory |
| `--dry-run` prompted, then failed with `not a terminal` on a pipe | A dry run removes nothing, so it never prompts |
| `rclean -d --format json \| jq` could not parse: a log line preceded the JSON on stdout | Log output goes to stderr |
| `cargo test` failed on a clean checkout: two tests read `tests/.rclean.toml`, the fixture was named `tests/.drclean.toml` | Fixture renamed |

## Choosing between them

Use rclean when you want presets per ecosystem, a config file checked into a repository, exclude patterns, age filtering, JSON for scripting, or unattended deletion. It is the more complete and more configurable tool.

Use cclean when speed on a large tree matters, when the dependency tree matters, when a 94 KB binary that builds in a second matters, or when you want build-artifact removal that checks for a project marker before deleting `target/`.

The overlap is wider than it was. rclean is a configurable general-purpose glob remover. cclean is an opinionated one with no dependencies and a fixed safety model.

## What cclean should take from rclean

In rough order of value:

1. ~~A test suite.~~ Done: `make test` runs unit and command-line suites.

2. ~~A parallel walk.~~ Done: the walk and sizing now share one work-queue driver, and cclean is the faster of the two again.

3. **Exclude patterns.** The one composition primitive rclean has that cclean cannot express at all.

4. **A config file**, so a pattern set can live in a repository.

5. **`--format json`**, worth doing only if the log-to-stdout mistake above is avoided.

Presets are worth considering but overlap with cclean's `.*_cache` pattern, which already covers per-tool caches by shape rather than by name.
