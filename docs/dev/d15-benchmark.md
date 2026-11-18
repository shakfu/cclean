# D-15 benchmark: cost of checking protected names inside targets

Status: measured 2026-11-18. Decision: deferred.

[D-15](spec.md) proposes refusing a directory target that contains a protected name. This file records what that check would cost if it were on by default.

## Method

- Script: `scripts/bench_walk.py`. It builds two trees in a temporary directory, then times each tool on them.
- No tool has the check yet. The cost is estimated from the size walk, which visits every entry in a target.
- hclean and oclean walk targets only with `-s` or JSON. Their added cost is the `-s` time minus the plain time.
- reclean and cclean walk every target on every run. Their added cost is one name compare per entry.
- Linux, NVMe, 16 cores, warm page cache. Dry runs: median of 7. Removal: median of 5, each on a fresh copy.
- Builds: reclean release, cclean `-O3`, hclean cabal default, oclean native.

## Results

Tree 1: one `node_modules` target with 106k entries (100k files, 19 MiB).

| Tool | Dry run | Dry run + walk | Removal | Removal + walk |
|-|-|-|-|-|
| reclean | 22 ms (walks already) | - | 1316 ms | - |
| cclean | 55 ms (walks already) | - | 230 ms | - |
| hclean | 2 ms | 601 ms (+599) | 1548 ms | 2141 ms (+593, +38%) |
| oclean | 1.5 ms | 294 ms (+292) | 1430 ms | 1729 ms (+299, +21%) |

Tree 2: 5000 `__pycache__` targets of 4 files, beside 20k source files.

| Tool | Dry run | Dry run + walk | Removal | Removal + walk |
|-|-|-|-|-|
| reclean | 37 ms (walks already) | - | 87 ms | - |
| cclean | 46 ms (walks already) | - | 94 ms | - |
| hclean | 320 ms | 511 ms (+191, +60%) | 950 ms | 1154 ms (+204, +21%) |
| oclean | 130 ms | 219 ms (+89, +68%) | 604 ms | 689 ms (+85, +14%) |

A second run gave hclean +535 ms and oclean +292 ms on Tree 1 removal. cclean varied by 2x between runs.

## Findings

- The walk costs about 6 us per entry in hclean and 3 us in oclean. Removal costs 10 to 15 us per entry.
- A check during removal needs no walk. Its cost is one compare per `unlinkat`, too small to measure here. A dry run would not show the refusal.
- reclean removes Tree 1 about 6 times slower than cclean. This is unrelated to D-15.

## Not measured

- Cold cache: dropping caches needs root.
- A names-only walk using `readdir`'s `d_type`, which skips most `lstat` calls.
- macOS, other file systems, real trees.

## Options considered

1. Reject, and document D-15 as the rule.
2. Check during removal only, on by default.
3. Check during scan and removal, on by default.
4. Check only targets matched by a custom glob (`-g`, config patterns), not defaults, presets or `-B`. A `.git` inside known detritus means misuse, and a hostile `.git` could block cleaning.
5. Defer. Chosen.
