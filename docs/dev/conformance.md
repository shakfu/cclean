# Conformance against spec.md

Status: run on 2026-11-18, Linux, uid 1000 (not root).

This file records how four tools meet [spec.md](spec.md), the core, and their profiles: [spec-modern.md](spec-modern.md) for reclean and cclean, [spec-classic.md](spec-classic.md) for hclean and oclean. A black-box harness ran each check against each binary, on fresh directory trees.

The harness is `scripts/conform.py`. Re-run it with `scripts/conform.py [TOOL ...]`. Each tool runs the core checks and its own profile's. The script exits 1 if any check fails.

| Tool | Profile | Binary | Revision |
|-|-|-|-|
| reclean | modern | `target/debug/reclean` | `6281e10` (0.6.0 + Unreleased) |
| cclean | modern | `build/cclean` | 0.3.0 (`0927f41`) |
| hclean | classic | `cabal list-bin hclean` | `8f97c00` + working tree |
| oclean | classic | `./oclean` | `72bf1a9` + working tree |

## Method

- Each check builds a tree, runs the tool, then inspects the file system, exit status, stdout and stderr.
- Removal checks run with `-y` and test what is left on disk. Output checks use dry runs.
- A flag the tool lacks makes the check `N/A`. hclean has no colour, so `COL-2` and `COL-6` are `N/A`; the core `COL` requirements apply only to a tool that colours.
- cclean runs with `--no-config`, except for config checks. The other three read no config without `-c`.
- `-B` checks use real repositories (`git init`).
- `RM-1` and `RM-2` start a run without `-y`, wait for the prompt, change the tree, then answer `y`. `RM-1` swaps a path component for a symlink to a directory outside the root. `RM-2` replaces the target with a new directory.

## Summary

| Tool | Profile | Checks | PASS | FAIL | N/A |
|-|-|-|-|-|-|
| reclean | modern | 81 | 81 | 0 | 0 |
| cclean | modern | 81 | 81 | 0 | 0 |
| hclean | classic | 67 | 65 | 0 | 2 |
| oclean | classic | 67 | 67 | 0 | 0 |

The core has 55 checks. The modern profile adds 26 and the classic profile 12.

## Changes made to reach this

hclean failed 8 core checks before the working-tree changes, oclean 9. Each fix is in the tool's CHANGELOG under Unreleased.

| Check | Defect | Fix |
|-|-|-|
| RM-1 | a component swapped for a symlink was followed; files outside the root were deleted | descriptor walk from the root with `O_NOFOLLOW` |
| RM-2 | a replaced target was removed | type, device and inode compared with the scan |
| WALK-3, OUT-2, OUT-3 | an unreadable directory was skipped silently | `<tool>: warning: cannot read PATH: REASON` on stderr |
| FILT-3 | an old directory holding a recent file was removed | every entry below must be old and readable |
| NAME-1 | control characters printed raw | `\xNN`, and `\\` for backslash |
| NAME-2 | invalid UTF-8 produced invalid JSON | U+FFFD per invalid byte |
| SIZE-4 | oclean only: decimal units (`1.05 MB`) | binary units (`1.00 MiB`) |

Moving the interface requirements into profiles removed the other 17 failures without code changes. The classic profile records what hclean and oclean already shared.

## Not tested

| ID | Why |
|-|-|
| CLI-3 | covered by the profile's `CLI` checks |
| WALK-2 | needs a mount point |
| WALK-5, RM-7 | parallelism is not visible from outside |
| FILT-1 | covered in part by the filter checks |
| FILT-8 | cannot make an mtime unreadable as non-root |
| RM-6, RM-8 | permissions, not obligations |
| COL-3, COL-4 | needs separate terminals on stdout and stderr |
| PROG-2 to PROG-4 | progress is throttled; small trees draw nothing |
| LIB-1 | library, not CLI |

Notes:

- **ASK-2** uses a pseudo-terminal. The typeahead half depends on timing: it sends `y` before the prompt appears.
- **RM-5, EXIT-1, EXIT-2** get their removal failure from a file in a read-only parent. A read-only directory inside a target does not fail in the classic profile (`RM-C1`).
- **EXIT-C1** and **WALK-3** check that the unreadable path appears on stderr, not the warning's wording.

## Divergence probes

Observations, not pass or fail.

| ID | Probe | reclean | cclean | hclean | oclean |
|-|-|-|-|-|-|
| D-5 | file as root: exit status | 0 | 1 | 1 | 1 |
| D-7 | `[ab].tmp` matches `a.tmp` | yes | no | yes | yes |
| D-8 | `x.log` matches `p/q/x.log` | no | yes | yes | yes |
| D-10 | a user pattern keeps the defaults | no | yes | no | no |
| D-11 | `venv/` and `.venv/` excluded by default | yes | no | no | no |
| D-15 | `.git` inside a matched directory removed | yes | yes | yes | yes |
| D-18 | matched symlink removed without `-i` | no | yes | no | no |
| D-20 | matched FIFO | exit 1, kept | removed | removed | removed |
| D-31 | size of a symlink to `realfile` | 8 | 0 | 8 | 8 |
| D-34 | prompt stream | stderr | stdout | stderr | stderr |
| D-38 | stdout when nothing matched | empty | `No matching targets found.` | empty | empty |
| D-39 | last line of a dry run | total | `Dry run: nothing was removed.` | last `Matched:` line | last `Matched:` line |
| D-45 | JSON top-level keys | `config failures matches reasons schema stats summary warnings` | `schema root config status targets total stats removed failed warnings` | `matches summary stats failures` | same as hclean |

Findings:

- **D-15** holds in all four tools. A `.git` inside a matched directory is deleted with it. It is the largest remaining safety gap.
- **D-8**: reclean is the only tool that matches a slash-free pattern against the path alone.
- **D-1**: reclean, hclean and oclean use `-d` for dry run. Only cclean uses it for dependencies.
