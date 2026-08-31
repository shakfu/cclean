# cclean compared to rclean

Both tools remove development debris by glob pattern, with a dry-run mode, a confirmation prompt, a parallel walk, parallel directory sizing, and a set of directories they refuse to enter. They diverge on presets, dependencies, and size. rclean is the larger tool and the one with per-ecosystem presets; cclean is the faster and smaller one, and since 0.2.0 it carries its own config file, exclude patterns, age and size filters, JSON output, and unattended deletion.

Measurements were taken on 2026-08-29, macOS on 8 cores (4 performance, 4 efficiency), against cclean built with `make build` and rclean 0.4.0 built with `cargo build --release`.

This document has been revised three times as both tools changed. The first revision measured rclean 0.3.0 and reported it nine times slower, with a doubled size estimate and no protected directories; rclean 0.4.0 fixed all three and took the speed lead. The second revision followed cclean replacing its serial `fs::recursive_directory_iterator` with a parallel walk, which took the lead back. The table near the end records what changed.

All figures below were re-measured in one session against both current binaries. rclean's walk came out slower here than the previous revision recorded (78 ms against 61 ms), so read these as same-session relative numbers rather than as absolutes comparable across revisions.

This third revision (2026-08-31) updates the feature comparison for cclean 0.2.0, which closed most of the configurability gap: a config file, age and size filters, JSON output, unattended deletion, and marker-guarded dependency removal. **The measurements were not retaken.** Every timing, binary size, and build time below still describes the 0.1.1-era binary measured on 2026-08-29, against rclean 0.4.0, on macOS; rclean was not available on the machine where this revision was written. Source and test counts are current; performance rows are marked where they are not.

## Summary

| | cclean | rclean |
|-|-|-|
| Language | C++17, one file | Rust, `src/` plus `tests/` |
| Source | 2239 lines, plus 1308 of tests | 1487 lines, plus 1188 of tests |
| Dependencies | none | 12 runtime, 2 dev |
| Binary, stripped* | 93,680 bytes | 1,470,880 bytes |
| Clean build* | 1.1 s | 17.9 s |
| Automated tests | 18 unit functions (178 checks), 136 CLI checks | 69 tests |
| Dry run, 65,101 entries* | 61 ms | 78 ms |
| Tree walk | parallel | parallel |
| Directory sizing | parallel | parallel |
| Protected directories | yes, fixed list, prunable with `--exclude` | yes, replaceable list |
| Config file | `.cclean.toml`, upward from `ROOT` | `.rclean.toml`, plus `~/.config/rclean/` |
| Presets | none | 7, plus `all` |

\* Measured against cclean 0.1.1 on 2026-08-29; not retaken for 0.2.0. cclean has roughly doubled in source size since, so treat the size and build-time rows as a floor.

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

cclean removes a build directory only when `.git` and the marker file for that ecosystem sit beside it: `build/` beside `CMakeLists.txt`, `target/` beside `Cargo.toml`, and 25 such pairs in all since 0.1.1, covering JavaScript, JVM, Python, Zig, Swift, Elixir, Dart and Meson. A marker licenses only the directory it is paired with, so `package.json` beside a `target/` does not qualify it, and a directory with a nested repository between it and `ROOT` is disqualified outright, which keeps submodules and vendored checkouts intact. The check is opt-in behind `--build-artifacts`.

rclean's `rust` preset is the single pattern `**/target`, and its `java` preset includes `**/build` and `**/target`. Neither consults the surrounding directory, so any directory named `target` matches wherever it appears.

The tradeoff was that cclean's rule missed monorepos, where the repository's `.git` sits above the package. 0.2.0 answers that with `project_roots` in `.cclean.toml`, which names the package roots explicitly rather than relaxing the marker rule for everyone.

### Marker-guarded dependency removal

`--dependencies` removes `.venv` beside `uv.lock`, `node_modules` beside a lock file, and `vendor` beside `go.mod`, with `dependency_markers` in `.cclean.toml` for other ecosystems. rclean reaches the same directories through its `node` and `go` presets, by name and without a marker check.

The distinction cclean draws is that these trees need a network, a registry, credentials or post-install state to rebuild, so they sit behind their own flag rather than under `--build-artifacts`. The lock file rather than the manifest is deliberate: `pyproject.toml` is also written by poetry, pdm and hatch, and sits beside pip-populated virtualenvs that `uv.lock` does not claim.

### Confirmation

cclean takes a single keypress with no Enter, and discards typed-ahead input so a stray keystroke cannot answer the prompt. rclean uses a line-based prompt via `dialoguer`.

### Dependencies

cclean has no dependency tree to audit or keep current. rclean pulls in 12 crates, down from 14. For a tool that runs `remove_all` on paths it chose itself, that is a meaningful difference in what has to be trusted, though the crates involved are widely used.

### Size and build time

93,680 bytes against 1,470,880, and 1.1 s to build from clean against 17.9 s. Both cclean figures are from the 0.1.1-era binary; the source has roughly doubled since, so read them as a floor rather than as current numbers.

## Where rclean wins

### Tests

rclean has 69 tests: 68 across 10 files, plus one doc-test. They cover deletion, patterns, symlinks, path traversal, protected directories, relative paths, age filtering, config discovery, directory sizing, and size formatting.

cclean has 18 unit test functions holding 178 assertions, plus 136 command-line checks, run by `make test`. The counts are not comparable, since rclean's 69 are test functions and cclean's 178 are individual assertions.

Coverage differs in kind. cclean's unit tests go deeper on the glob matcher, including that `**/` does not degrade to a bare `*`, that regex metacharacters stay literal, and that matching a pathological pattern terminates. Age filtering and config discovery are no longer rclean-only ground: cclean's command-line suite covers config discovery upward from `ROOT`, `project_roots` resolution, schema rejection, and both filters. Path traversal remains rclean-only, as cclean resolves nothing beyond `ROOT`.

Both suites were mutation-tested while writing this: 13 deliberate defects were introduced into cclean and each was caught. The gap that mutation testing exposed, and that is now closed, was a defaults test that hardcoded a pattern string instead of reading `defaults::patterns`, so a typo in the array passed unnoticed.

One thing cclean still does not cover: the terminal branch of the confirmation prompt, which needs a pseudo-terminal. The parallel driver is now exercised directly for exception propagation, though its sizing path under contention is still reached only indirectly.

### Configuration and presets

rclean has named presets for python, node, rust, java, c, go, and common, plus an `all` that merges them. It reads `.rclean.toml`, searching upward from the working directory and falling back to `~/.config/rclean/`, and can write a default config with `-w`. CLI flags override file values.

cclean 0.2.0 reads `.cclean.toml`, searching upward from `ROOT` for the first one and stopping there; it does not fall back to `~/.config`, so configuration outside the ancestor chain is never consulted. CLI options override file values, command-line patterns extend configured ones, and `--exclude` replaces configured excludes when at least one is supplied. The schema is small and strict, rejecting unknown keys and invalid values. Ten keys: `patterns`, `excludes`, `defaults`, `build_artifacts`, `dependencies`, `skip_protected`, `dependency_markers`, `project_roots`, `older_than`, `larger_than`.

What rclean still has here is presets. cclean has no named ecosystem sets and no `-w` to write a starter config. Its protected-directory list can be switched off from configuration with `skip_protected`, the file equivalent of `--no-skip`, but not redefined the way rclean's `protected_dirs` redefines its own. `project_roots` has no rclean equivalent: it names package roots in a monorepo so marker-guarded artifact detection works where `.git` sits above the package.

### Features cclean does not have

- Named ecosystem presets, and `-w` to write a starter config.

- A configuration-replaceable protected-directory list. cclean's is fixed: `skip_protected` and `--no-skip` turn it off wholesale, and `--exclude` prunes around it, but no name can be added to it.

- Shell completions for five shells.

- `--stats`, a per-pattern breakdown. `--format json` carries per-reason statistics, which is close but groups by match reason rather than by pattern.

- Broken-symlink removal. cclean matches symlinks by pattern and sizes them as zero, but has no rule that targets dangling links as such.

- Published on crates.io. cclean has a changelog, tags, and an MIT license, but no package registry and no binary distribution.

## Where they mostly agree

### Protected directories

Both tools refuse to enter version-control metadata and user configuration. rclean 0.4.0 adopted cclean's list, which cclean 0.1.1 then shortened; 0.2.0 left the list unchanged.

| | cclean 0.2.0 | rclean 0.4.0 |
|-|-|-|
| `.git`, `.hg`, `.svn` | protected | protected |
| `.config`, `.ssh`, `.gnupg` | protected | protected |
| `.venv`, `venv` | walked | protected |

Given a tree with a `.pyc` file in each of `.git/objects`, `.venv/lib`, `venv/lib`, `.ssh`, and `src`, cclean matches three items and rclean one: cclean cleans both virtual environments, and neither touches `.git` or `.ssh`. A virtual environment's `__pycache__` is an ordinary Python cache, and it is usually the largest concentration of them in a project; the rest of the list is state another tool owns. Both agree on two edges:

- A protected directory named as the root is entered. Pointing either tool at `.git` is deliberate.

- Protection is by name, whatever the entry type, so the `.git` *file* that marks a submodule is protected too.

They also differ in how the list is overridden. cclean's is fixed, with `--no-skip` to disable it wholesale. rclean's `--no-protect` does the same for one run, and `protected_dirs` in a config file replaces the list, so a project can protect names of its own.

cclean answers the narrower half of that with excludes rather than with a replaceable list: `--exclude .venv`, or an `excludes` entry in `.cclean.toml`, leaves virtual environments alone and can be committed to a repository. `skip_protected` mirrors `--no-skip` in the file, but only to disable the list, never to extend it. What it still cannot do is add a name to the *protected* set proper, which differs in that protection survives `--no-defaults` and applies to every pattern source at once.

### Exclude patterns

Both take exclude globs. rclean's `--exclude` suppresses the match; cclean's `-e, --exclude` prunes, so naming a directory keeps everything under it rather than leaving its contents individually eligible. Pruning is what makes `--exclude .venv` restore the protection the list gave up in 0.1.1.

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

Use rclean when you want presets per ecosystem, a protected-directory list a project can redefine, shell completions, a per-pattern breakdown, or an install from crates.io.

Use cclean when speed on a large tree matters, when the dependency tree matters, when a small binary that builds in a second matters, or when you want removal that checks for a project marker before deleting `target/` or `node_modules/`.

The overlap is wider than it was, and 0.2.0 widened it further: config file, excludes, age and size filters, JSON, and unattended deletion are now common ground. rclean is a configurable general-purpose glob remover. cclean is an opinionated one with no dependencies and a marker-guarded safety model.

## What cclean should take from rclean

In rough order of value:

1. ~~A test suite.~~ Done: `make test` runs unit and command-line suites.

2. ~~A parallel walk.~~ Done: the walk and sizing now share one work-queue driver, and cclean is the faster of the two again.

3. ~~Exclude patterns.~~ Done: `-e, --exclude`, which prunes rather than only suppressing the match.

4. ~~A config file.~~ Done in 0.2.0: `.cclean.toml`, discovered upward from `ROOT`, with a strict schema.

5. ~~`--format json`.~~ Done in 0.2.0, and it avoids the log-to-stdout mistake above: JSON is alone on stdout, diagnostics and prompts go to stderr.

6. ~~Age filtering.~~ Done in 0.2.0: `--older-than`, alongside a `--larger-than` that has no rclean equivalent.

7. ~~Unattended deletion.~~ Done in 0.2.0: `-y, --yes`, leaving the default prompt unchanged.

That is the whole of the original list. What remains unclaimed from rclean is shell completions, a per-pattern `--stats`, a configuration-replaceable protected list, and distribution through a package registry.

Remaining items are tracked in `TODO.md`, along with a `--dependencies` flag that has no rclean equivalent: rclean folds `node_modules` and `vendor` into its `node` and `go` presets, so they come out with a preset rather than behind a marker check and a separate flag.

Presets are still worth considering but overlap with cclean's `.*_cache` pattern, which already covers per-tool caches by shape rather than by name, and with `.cclean.toml`, which now lets a project commit a pattern set of its own.
