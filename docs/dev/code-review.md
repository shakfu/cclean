# Project Review, Validated

The original review was written against commit `770fc75` and delivered as a
gitignored `REVIEW.md` at the repo root, so its text is not in git history.
This document supersedes it. It records what each finding claimed, whether the
claim held, and what was done. Every claim was checked against the code and,
where it made a testable prediction, reproduced or refuted at the command line.

## Verdicts

| # | Finding | Claimed | Verdict | Actual | Outcome |
|-|-|-|-|-|-|
| 1 | File replacement race | High | Real, already documented | Low | Closed, no change |
| 2 | Directory replacement race | High | Real, already documented | Low | Closed, no change |
| 3 | Scan follows replaced directory | Medium | Confirmed, new | Low-Medium | Documented, not fixed |
| 4 | Root not identity-bound | Medium | Real, library-only | Low | Closed, see analysis |
| 5 | No CTest timeouts | Medium | Confirmed | Medium | Fixed |
| 6 | `cli.sh` masks failures | Medium | Overstated | Low | Fixed differently |
| 7 | Makefile path quoting | Medium | Confirmed, reproduced | Low-Medium | Fixed |
| 8 | Empty `--config=` | Low | Confirmed, reproduced | Low-Medium | Fixed |

One defect the review did not find is recorded under "New finding" below.

## 1 and 2: the removal races

**Claim.** `remove_target()` checks device, inode and type with `fstatat()`,
then unlinks by name. A concurrent process can replace the entry between the
two calls. The directory form has the same gap after `empty_directory()`.

**Verdict.** The races are real. The severity is not High, and neither is new.

`CHANGELOG.md:9` already states the residual: the unlink "narrows the window to
two adjacent syscalls rather than closing it, since there is no descriptor to
unlink through", and `AT_REMOVEDIR` "bounds what that can reach to an empty
directory". The code comments at `src/remove.cpp:446-453` and
`src/remove.cpp:305-308` say the same. This is a documented, deliberate
trade-off, not an oversight.

Two arguments against the High rating:

- **No privilege boundary is crossed.** `unlinkat(parent, name, ...)` requires
  write and execute on `parent`. Replacing `name` requires the same permission.
  Anyone who can win the race can already delete any entry in that directory
  directly. Under a sticky bit the attacker can only replace entries they own,
  so the deleted replacement is their own file. A hardlink substitution removes
  one link and the original name survives.
- **The recommended fix does not exist on the supported platforms.** A
  handle-based deletion means `funlinkat(2)`, which is FreeBSD only. Linux and
  macOS have no unlink-by-descriptor and no unlink-if-inode-matches. CI targets
  `ubuntu-latest` and `macos-latest`.

The realistic failure is the benign one the CHANGELOG names: a build tool
rewriting a cache directory atomically mid-run. The loss is a rebuildable
artifact.

**Outcome.** No change. Closed.

## 3: the scan follows a replaced directory

**Claim.** The scan queues directory paths. `size_directories()` builds a
`std::filesystem::directory_iterator` from a queued path. A directory replaced
by a symlink before that point is followed, and the scan can report entries
outside `ROOT`.

**Verdict.** Confirmed, and the only genuinely new defect in the review.

Both queues hold `fs::path`, not descriptors: `src/scan.cpp:98`,
`src/scan.cpp:173`, `src/scan.cpp:288`. `fs::directory_iterator(p)` resolves
`p` following symlinks; `directory_options::follow_directory_symlink` governs
only `recursive_directory_iterator`.

The impact is narrower than claimed:

- Removal still refuses. `open_parent()` opens every component with
  `O_NOFOLLOW` and reports "Path component is now a symlink"
  (`src/remove.cpp:105-134`).
- The escape is one level deep. A symlink below the replaced directory is
  recognised by `entry.is_symlink()` and not descended into.
- What escapes is reported names, error strings and byte totals. Nothing is
  deleted through the followed path.

**Outcome.** Documented in the README Limitations section, not fixed. A
descriptor-relative scan means replacing `std::filesystem` with
`openat`/`fdopendir`/`readdir` throughout, and holding one open descriptor per
queued directory against a breadth-first parallel queue risks `EMFILE` on a
wide tree. A pre-iteration `lstat` would narrow the window without closing it,
which is the same class of non-fix as findings 1 and 2.

## 4: root identity

**Claim.** The removal API reopens `root` by path and does not validate its
identity. Replacing the root after scanning can redirect operations into a
different tree. Caller-built targets have no identity and can be removed from
the replacement.

**Verdict.** Real, but reachable only through the library, and the suggested
remedy does not close the window. See the analysis below.

**Outcome.** Closed rather than deferred.

## 5: CTest timeouts

**Claim.** Neither registered test has a timeout.

**Verdict.** Confirmed. No timeout existed in `CMakeLists.txt`, `tests/cli.sh`
or the CI workflow. A hang ran until GitHub's six-hour job default.

**Outcome.** Fixed. `TIMEOUT 120` on `unit`, `TIMEOUT 300` on `cli`, and
`timeout-minutes` on every CI job. The budgets are sized for both suites under
ThreadSanitizer on a Debug build, not for the 0.06 s and 1.6 s they take
locally, so a timeout means a hang and not a slow runner.

## 6: the CLI suite masking failures

**Claim.** `tests/cli.sh` sets `set -u` but not `set -e`. Assertions read
output through pipelines and inspect only the filter's result.

**Verdict.** Overstated. The suite already checks `$?` at more than 30 sites.
The real gap is narrower: an output-shape assertion passes if the binary prints
the expected line and then exits nonzero.

`set -e` is the wrong remedy. The suite deliberately runs commands expected to
exit 1, 2 and 3.

**Outcome.** Fixed differently. Rather than retrofit 80 call sites, every
invocation now runs through a `cclean` shell function. It records any status
outside the documented 0, 1, 2 and 3 to a file, which a subshell can write and
one check at the end reads. It returns the status unchanged, so the existing
`exits N` assertions are untouched. Explicit status assertions were added at
the two sites the review cited.

## 7: Makefile path quoting

**Claim.** `BUILD`, `PREFIX` and the source list are expanded unquoted.

**Verdict.** Confirmed and reproduced. `make BUILD="my build"` failed with
cmake's usage text.

**Outcome.** Fixed. All expansions quoted. The source list moved from
`$(shell find ...)` into the recipe as `find ... -exec cksum {} +`, sorting the
checksum lines rather than the filenames. That keeps the result independent of
find's walk order and needs neither `sort -z` nor `xargs -0`, neither of which
is POSIX, preserving the file's stated portability claim.

The stale-binary checksum guard was re-verified: stable across runs, changes on
edit, returns to its previous value on revert, and equal to the stamp after a
build.

## 8: empty `--config=`

**Claim.** `--config=` stores an empty path, which `main()` reads as "search
upward", so an explicit empty value can load an ancestor `.cclean.toml`.

**Verdict.** Confirmed and reproduced. Both `--config=` and `--config ""`
loaded the ancestor file and exited 0, with no diagnostic.

**Outcome.** Fixed. Both forms now exit 2. The value is rejected at the parse
rather than tracked by a `set_config` flag alongside the other options, which
keeps the invariant that `config_path` is non-empty exactly when the option was
given, and needs one field fewer than the review proposed.

## New finding: unquoted `$CCLEAN` in the CLI suite

Not in the original review. Found while verifying the Makefile fix.

`$CCLEAN` was unquoted at 28 call sites in `tests/cli.sh`, all inside command
substitutions. A binary whose path held a space word-split, and 18 checks
failed.

The defect was unreachable before finding 7 was fixed: `make` could not
configure a build directory with a space in its name, so no such binary ever
reached the suite. Confirmed pre-existing by running the committed `cli.sh`
from `770fc75` against a spaced path, which produced the same 18 failures.

Fixed, and a CI job now builds, tests and installs through spaced paths.

## Finding 4: performance analysis

An earlier draft of this review suggested revisiting finding 4 for performance
rather than safety, on the grounds that `remove_target()` reopens the root once
per target. **That suggestion was wrong.** It is recorded here with the
measurements that refute it.

### The syscall arithmetic

`open_parent()` (`src/remove.cpp:72-145`) does, per target at relative depth
`d`:

- 1 `open(root)` by path, resolving every component of the root's pathname
- `d-1` pairs of `openat` and `close` for interior components
- 1 final `close(parent)` in `remove_target()`

The root is reopened by name N times per run. Retaining one descriptor and
borrowing it removes exactly 2 syscalls per target, at any depth. That much is
correct.

### The measurement

Two prototypes were built in a scratch copy, both passing all 218 checks:

1. **Dup.** Open the root once, `fcntl(F_DUPFD_CLOEXEC)` per target. This only
   substitutes one syscall for another.
2. **Borrow.** The walk owns only descriptors it opened itself, so a target
   directly under the root costs no `open` and no `close`. This eliminates the
   two syscalls.

Borrow against current, medians of 5 to 7 runs:

| Workload | Current | Borrow |
|-|-|-|
| 4000 flat targets, root 8 components | 0.148 s | 0.154 s |
| 4000 flat targets, root 20 components | 0.179 s | 0.183 s |
| 2000 `__pycache__` targets, 40,000 files | 1.195 s | 1.189 s |

Every difference is inside the run-to-run spread and the direction is
inconsistent. The first workload was built to maximise the effect: 4000 targets
directly under the root, where the root open is a third of the walk's syscalls.
It shows nothing.

The error was counting syscalls saved, 8000 across the run, without checking
them against wall time. Removal is dominated by `unlinkat` and the filesystem
metadata work behind it, spread across 8 cores. 8000 cheap syscalls over 8
threads is roughly 1 ms against 150 ms, under the noise floor.

### Where root depth does cost

Same tree, root reached through 8 versus 20 path components, medians of 5 runs:

| Phase | Root 8 | Root 20 | Delta |
|-|-|-|-|
| Dry run, scan only | 0.020 s | 0.035 s | +0.015 s |
| Full run | 0.153 s | 0.180 s | +0.027 s |
| Removal, by subtraction | 0.133 s | 0.145 s | +0.012 s |

Most of the depth penalty is in the scan, not removal, and the borrow prototype
does not recover the removal-side residue. That residue is therefore in path
string handling, most likely `lexically_relative()` comparing a longer root
against every target, which no descriptor change can help.

If a path-handling optimisation is ever wanted, the scan is where the
measurable cost is. `fs::directory_iterator` is constructed from a full path
for every directory, so every level re-resolves the whole prefix. That is the
same code finding 3 concerns.

### Why finding 4 is closed rather than deferred

Retaining a descriptor inside `remove_targets()` does not close the window the
finding describes. The root would be opened at the start of removal, which is
after the scan and after the user answers the prompt. A root replaced while the
list was on screen is opened as the replacement.

Binding the identity that was actually scanned requires holding the descriptor
from `scan()` through `remove_targets()`. That means `ScanResult` owning a file
descriptor: a public, currently copyable struct becomes an RAII type with move
semantics, `include/cclean/scan.hpp` changes, and callers that copy a result
break.

That is an API break to protect only hand-built targets, which by construction
were never reviewed against a displayed list and already carry no identity. The
CLI cannot reach the gap at all, since it always goes through `scan()`. The
argument that would reopen this is one about the library's callers.

## Changes made

- `CMakeLists.txt`, `.github/workflows/ci.yml`: CTest timeouts, CI job
  timeouts, and a `make` job that builds, tests and installs through paths
  containing a space.
- `cli/options.cpp`, `cli/options.hpp`: empty `--config` rejected at the parse.
- `Makefile`: quoting, and a POSIX source-list checksum.
- `README.md`: the scan traversal boundary from finding 3.
- `tests/cli.sh`: the status wrapper, the quoting fix, and new coverage.
- `CHANGELOG.md`: entries for all of the above.

`src/` and `include/` are unchanged.

### Verification

- `make test` passes. The command-line suite goes from 211 checks to 218, with
  none removed. It passes with the binary at a normal path and at one
  containing a space.
- The four steps of the new CI job were run locally before being committed to.
- No performance impact. Every object in `cclean_core` and `cli/main.cpp.o` is
  byte-identical to the `770fc75` build; only `cli/options.cpp.o` differs, and
  the binary is the same 219,512 bytes. 20 dry runs over a 60,001-entry tree
  measure 1.73 s against 1.75 s, within noise.

## Coverage gaps

From the original review, with current status.

- No deterministic tests exercise replacement races. Still true. Findings 1, 2
  and 4 are closed, so only finding 3 would want one, and it is documented
  rather than fixed.
- No test installs the library and compiles an external consumer against the
  installed headers and archive. **Partly closed.** The new CI job installs and
  checks that the binary, the archive and the headers are present. Compiling an
  external consumer against them is still untested.
- CI bypasses `make`. **Closed.** The new job exercises the Makefile, its
  checksum guard and its install path.
- Raw-terminal tests are skipped when `python3` is unavailable. Still true.
- Permission and failed-removal tests are skipped under root. Still true.
- JSON checks rely on `grep` rather than validating complete documents. Still
  true. One check does parse the document with `json.load`.

## Remaining work, in priority order

1. Compile an external consumer against the installed headers and archive in
   CI. It is the largest untested path that reaches users.
2. Validate complete JSON documents rather than grepping them.
3. Decide whether the scan should be a security boundary. If yes, finding 3
   needs descriptor-relative traversal and an answer to descriptor exhaustion.
   If no, the README note is the whole of it.
