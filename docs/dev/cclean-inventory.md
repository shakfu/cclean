# cclean 0.3.0 behavioural inventory

Repo: ~/projects/cclean (HEAD 8f97c00 era tree, CMakeLists.txt:4 `VERSION 0.3.0`).
Binary: ~/projects/cclean/build/cclean (`cclean --version` -> `cclean 0.3.0`) [observed].
Paths below are relative to the repo. [observed] = ran it (Linux, uid 1000); [source] = read only.
Experiments ran in temporary directories, all with --no-config unless config was under test.

---

## 1. Invocation

Synopsis: `cclean [OPTION ...] ROOT [PATTERN ...]` (cli/options.cpp:13).

Parser model (cli/options.cpp:104-331) [source]:
- Hand-written loop over argv. No getopt. No bundling of short flags: `-nv` -> `Unknown option: -nv` exit 2 [observed].
- An argument is an option iff options have not ended, `size() > 1` and it starts with `-` (options.cpp:123). So a lone `-` is an operand (ROOT) [observed: `Cannot inspect root path: -: No such file or directory`, exit 1].
- `--` (first occurrence only) ends options; later args are operands, even `--` and `-n` (options.cpp:116-119) [observed: `cclean -n -- -n` -> ROOT `-n`].
- Options and operands may interleave in any order (operands pushed at options.cpp:316) [source].
- Options are applied left to right; repeating a scalar option: last wins [source].
- First operand is ROOT; every later operand is a CLI pattern (cli/main.cpp:282, 369-370).
- No operands -> ROOT = `.` (options.cpp:326-328).

Options (all at cli/options.cpp):

| Option | Arg | Default | Effect | Line |
|-|-|-|-|-|
| `-n`, `--dry-run` | none | off | list, never remove/prompt | 124 |
| `-b`, `--build-artifacts` | none | off (config `build_artifacts`) | enable artifact rule | 147 |
| `-d`, `--dependencies` | none | off (config `dependencies`) | enable dependency rule | 153 |
| `-e PAT`, `--exclude PAT`, `--exclude=PAT` | next argv verbatim (may start with `-`) | none | repeatable; any CLI exclude replaces config excludes | 286-303 |
| `-v`, `--verbose` | none | off | `Configuration: <path>` on stderr; `removed` lines | 159 |
| `-y`, `--yes` | none | off | skip prompt | 164 |
| `--color WHEN`, `--color=WHEN` | `auto`/`always`/`never` | `auto` | colour | 230-252 |
| `--format F`, `--format=F` | `human`/`json` | `human` | output format | 254-277 |
| `--config FILE`, `--config=FILE` | non-empty path | search upward | explicit config | 208-228 |
| `--no-config` | none | off | read no config | 196 |
| `--older-than D`, `--older-than=D` | duration | none (config `older_than`) | age filter | 169-188 |
| `--larger-than S`, `--larger-than=S` | size | none (config `larger_than`) | size filter | 169-194 |
| `-h`, `--help` | none | - | usage to stdout, exit 0 | 279-284 |
| `-V`, `--version` | none | - | `cclean 0.3.0` to stdout, exit 0 | 305-308 |
| `--no-defaults` | none | defaults on (config `defaults`) | drop built-in patterns | 129 |
| `--no-skip` | none | skip on (config `skip_protected`) | walk protected dirs | 135 |
| `--no-split` | none | split on (config `split`) | one thread per target | 141 |
| `--` | - | - | end of options | 116 |

- `--help`/`--version` stop parsing immediately; later bad args are not seen (options.cpp:283, 307) [source].
- No `=` forms for flags: `--dry-run=x` -> unknown option [source: exact string compare].
- No short form for `--color`, `--format`, `--config`, `--older-than`, `--larger-than`, `--no-*` [source].
- No flag re-enables something config turned off (`defaults`, `split`, `skip_protected`, `build_artifacts`, `dependencies`) (TODO.md:27) [source].
- Value-taking options with separate arg consume the next argv unconditionally, even if it starts with `-` (options.cpp:177-178, 294) [source]. Exception: `--config ""` rejected (209).
- `--older-than=` / `--older-than ""` (empty) is accepted, sets the "given" flag, and then means "no age filter" (cli/main.cpp:349 tests `.empty()`), silently overriding a config `older_than`. Same for `--larger-than` [observed: `--older-than=` exit 0, no error].

Error strings (stderr, exit 2 unless noted), verbatim:
- `Unknown option: <arg>` + blank line + full usage on stderr (options.cpp:310-311) [observed].
- `--older-than needs a value.` / `--larger-than needs a value.` (171) [observed].
- `--config needs a file.` for `--config` at end, `--config ""`, `--config=` (210, 221) [observed].
- `--color needs a setting.` (232); `Unknown colour setting: <v> (use auto, always, or never)` (97-98) [observed].
- `--format needs a format.` (256); `Unknown format: <v>` (77) [observed].
- `-e needs a pattern.` / `--exclude needs a pattern.` (288; name echoes spelling used) [observed].
- `--config and --no-config cannot both be given.` (320), checked after the whole loop, either order [observed].
- `Config file is not a readable file: <FILE>` when `--config` target is not a regular file (follows symlinks) (cli/main.cpp:297-300) [observed].
- `Invalid --older-than duration: <v> (use a number followed by s, m, h, d, or w)` (main.cpp:353-354) [observed].
- `Invalid --larger-than size: <v> (use B, K, M, G, or T)` (main.cpp:363-364) [observed].
- `--no-defaults leaves no patterns to match; supply at least one, or --build-artifacts, or --dependencies.` (main.cpp:375-383). Prefix is `defaults = false` instead when defaults were turned off by config only [observed both].

Validation order in `run()` (main.cpp:274-417) [source]: argv parse -> config locate/load -> apply config -> project_roots -> older_than -> larger_than -> empty-pattern check -> ROOT checks. So config/filter errors (2) win over a bad ROOT (1).

## 2. Root

- ROOT = first operand, else `.` (options.cpp:326-328). Kept verbatim (not absolutised) for scanning, display and JSON `root` (scan.cpp:505; json.cpp:46) [observed: `"root": "."`].
- Target paths = ROOT joined with names via `fs::path` `/` (scan.cpp:125). ROOT `./` yields `./__pycache__` [observed].
- Checks in order (main.cpp:390-417):
  - `fs::status(root)` (follows symlinks). Error -> `Cannot inspect root path: <root>: <strerror>` exit 1 [observed missing, broken symlink, empty string `""`].
  - not a directory -> `Root path is not a directory: <root>` exit 1 [observed file, symlink to file].
  - `directory_iterator(root)` fails -> `Cannot scan root path: <strerror>` exit 1 [observed chmod 000: `Cannot scan root path: Permission denied`].
- Symlink to directory as ROOT: accepted and followed (main.cpp:391-395; CHANGELOG.md:29) [observed].
- ROOT is never a target: only entries inside it are tested (scan.cpp:98-114) [observed: ROOT `__pycache__` lists its children only].
- ROOT is exempt from the skip list: ROOT `.git` is scanned (README.md:375; TODO.md:29) [observed `d/.git` -> `d/.git/a.pyc`].
- ROOT strings in these error messages are not escaped by `display()` (main.cpp:398, 404) [source].
- Removal opens ROOT by name, following it; everything below with O_NOFOLLOW (src/remove.cpp:150-151, 180-182).

## 3. Patterns

Default list, exact order (include/cclean/defaults.hpp:14-20) [observed in help]:
`__pycache__`, `*.pyc`, `*.pyo`, `.*_cache`, `.DS_Store`.
Help renders them joined by two spaces (src/defaults.cpp:8-19).

Glob dialect (src/glob.cpp:14-45, 146-204) [source + observed]:
- `*` = any sequence of any characters, including `/`. Runs of `*` collapse to one (glob.cpp:30-36). `**` not followed by `/` == `*` [observed `**` == `*`].
- `**/` (exactly `*`,`*`,`/`) = zero or more whole directory levels: empty, or any string ending in `/` (glob.cpp:21-27, 183-192) [observed `**/f.o` matches `f.o` at top].
- `?` = exactly one character, `/` included (glob.cpp:172-175) [source].
- `[...]`, `{...}`, `!`, `\` are literal characters. No escaping [observed: `[ab].txt` matches only a file literally named `[ab].txt`; `{a,b}.txt` likewise; `\*.o` matches nothing].
- Case-sensitive, byte-wise (glob.cpp:166) [observed `A.TXT` no match; `TOP.PYC` not matched by `*.pyc`].
- Dotfiles: no special rule; `*` matches leading dot [observed `*.o` matches `.hidden.o`; `*` matches everything].
- Whole-string anchored match. Literal/prefix/suffix fast paths are equivalent (glob.cpp:48-105).
- Matching is an NFA simulation, O(tokens*length), no backtracking (glob.cpp:143-146).
- Empty pattern: Literal "" -> never matches a non-empty name [source inference].

What a pattern is matched against (glob.cpp:206-235; scan.cpp:458-486):
- Built-in patterns: scope NameOnly = final component only (scan.cpp:469-471) [observed `.venv/lib/foo_cache` not matched].
- Config and CLI patterns: scope NameOrPath = final component first, then `path.lexically_relative(root).generic_string()` (glob.cpp:216-230). Relative path has no leading `./` and no trailing `/`.
  - [observed] `f.o` matches `a/b/c/f.o` (by name); `a/b/f.o` matches only that path; `./f.o` matches nothing; `a/**` matches `a/b` but not `a`; `a/*/f.o` matches `a/b/c/f.o` (since `*` spans `/`); `**/` alone matches nothing; a pattern ending in `/` never matches [source].
- First matching pattern decides the reason; order is defaults, config, CLI (scan.cpp:63-64, 458-486; glob.cpp:215).

Combination:
- Effective pattern list = (defaults if enabled) + config `patterns` + CLI patterns (scan.cpp:458-486; main.cpp:386-387).
- `--no-defaults` / `defaults = false` drops only the built-ins. Config patterns still apply [observed: `--no-defaults` with config `patterns = ["*.tmp"]` lists `s/a.tmp`, reason `config`]. Contradicts help text (see 19).
- Empty-pattern guard: error only if defaults off AND no CLI patterns AND no config patterns AND neither -b nor -d (main.cpp:375-376).
- Reason precedence for one entry: build-artifact > dependency > first matching pattern (scan.cpp:227-234) [observed: `build` matched by CLI pattern and -b -> `build-artifact`].
- Directory-name note: for each CLI pattern with no `*` or `?` that names an existing directory either relative to cwd or under ROOT, stderr gets (main.cpp:250-269) [observed]:
  ```
  Note: <pat> names a directory, and was read as a pattern, not as a second root.
        It matches every entry called <pat> anywhere under <root>. One root per run.
  ```
  Printed only when at least one target exists (after the empty check, main.cpp:434-451). With ROOT `.` it reads `under ..` [observed].

## 4. Excludes

- Sources: CLI `-e/--exclude` (repeatable) or config `excludes`. Any CLI exclude replaces the config list entirely (main.cpp:52-54) [observed: config `.venv` + `-e keep` -> `.venv` contents listed].
- Compiled with scope NameOrPath, same as CLI patterns (scan.cpp:488-497).
- Default excludes: none (include/cclean/config.hpp:17).
- Semantics: prune. Checked per entry during the walk, after the skip list and before matching; a matching entry is neither matched nor descended (scan.cpp:143-150) [observed `-e .venv` hides `.venv/lib/__pycache__`].
- Excludes apply to defaults, config, CLI patterns, artifacts and dependencies alike, because the check precedes all of them (scan.cpp:147-172; README.md:271).
- Excludes do NOT apply inside a matched directory: sizing and removal ignore them (scan.cpp:305-413; remove.cpp:242-274) [observed: `-e '*.pyc'` still lists `./src/__pycache__/` which holds `m.pyc`]. See 19 (REVIEW.md:15-35).
- An exclude matching ROOT itself has no effect (ROOT is never tested) [source].

## 5. Protected (skipped) directories

- List, exact order (include/cclean/defaults.hpp:115-122): `.git`, `.hg`, `.svn`, `.config`, `.ssh`, `.gnupg`.
- Test: exact, case-sensitive filename equality on any entry type (file, dir, symlink) (src/project.cpp:155-159; scan.cpp:139-141). A `.git` file is skipped too [source].
- Effect: neither matched nor descended; precedes excludes and patterns, so no pattern can reach inside [observed `.git/x/z.pyc` not listed].
- Override: `--no-skip` or config `skip_protected = false` (options.cpp:135-139; main.cpp:46-48).
- Not applied to ROOT (2), nor inside matched directories during sizing/removal [observed: `__pycache__/.git/obj` (7 B) counted in `./__pycache__/  7 B`].
- `has_repository` (the -b `.git` test) is independent of the skip list (project.cpp:27-32).

## 6. Walk

Algorithm: parallel BFS-ish work queue over directory paths (src/scan.cpp:88-277; src/parallel.hpp:43-142). Workers = min(hardware_concurrency or 1, 32) (parallel.hpp:22-26).

Per entry, in order (scan.cpp:114-254):
1. `is_symlink` (no-follow, cached from readdir). Error -> warning `Cannot inspect <path>: <msg>`, skip entry.
2. Skip list (5). 3. Excludes (4).
4. `is_directory` = not symlink and `is_directory()`; error -> `Cannot inspect ...`.
5. Pattern match (any entry type), artifact test (real dirs only, -b), dependency test (real dirs only, -d).
6. Nothing matched: queue the entry if it is a real directory; else drop.
7. Matched: `lstat()`; failure -> warning `Cannot inspect <path>: <strerror>` and the entry is dropped (scan.cpp:186-202; CHANGELOG.md:49).
8. Record dev/ino, type, mtime (symlink: lstat mtime; else `last_write_time`, errors ignored), reason, restore.
9. Non-directory, non-symlink: size = `file_size()` if regular file, else 0. Status error -> warning, kept at size 0 (scan.cpp:240-252).

Properties:
- Symlinks: never followed or descended, during walk or sizing (scan.cpp:172-177, 341-345). A symlink can match a pattern by its own name and becomes a target of type symlink, 0 bytes, including broken links [observed `broken.pyc`, `link.pyc` -> dir].
- A matched directory is one target and is not descended; nested matches inside it are not listed separately (scan.cpp:78-80) [observed `__pycache__/__pycache__` only via ROOT=`__pycache__`].
- Other filesystems: no device check; mount points are crossed (no st_dev comparison in scan.cpp) [source].
- Special files (FIFO, socket, device) can match; size 0, JSON type `file` [observed FIFO `f.pyc`: `"type": "file", "bytes": 0`].
- Unreadable directory during walk: warning `Error scanning <dir>: <msg>`, walk continues (scan.cpp:114-120, 257-260) [observed `Error scanning ./locked: Permission denied`]. Unreadable dir inside a matched target: same message from sizing; target kept with partial size [observed `./p/__pycache__/inner`].
- Any warning -> exit 3 if nothing failed (15).
- Warning order: scan-phase warnings sorted lexicographically, then sizing-phase warnings sorted, then age-filter warnings in target order, then removal errors in target order (scan.cpp:65-75, 276, 415, 435; main.cpp:530).
- Result order: `std::sort` by `fs::path operator<`, i.e. component-wise byte comparison (scan.cpp:513-516). So `./a/` sorts before `./a.b/` [observed], unlike plain string order.
- Scan is path-based, not descriptor-relative; a dir swapped for a symlink mid-scan can be followed once (README.md:373) [source].
- Progress callback phase `scanning` with cumulative entry count, once per directory (scan.cpp:272-273).

## 7. Build artifacts (-b)

Table (include/cclean/defaults.hpp:31-74), directory -> marker(s) [source; names observed in help]:

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

25 pairs, 14 distinct names. Help prints the distinct names sorted, wrapped at 68 columns (src/defaults.cpp:21-50) [observed].

Rule `is_artifact_directory` (src/project.cpp:56-121) [source; observed cases]:
1. Entry is a real directory (not symlink) whose name is in the table.
2. `project` = normalized absolute path of its parent (fs::absolute + lexically_normal; no symlink resolution). Absolute failure -> not an artifact.
3. If `project` equals a resolved `project_roots` entry: skip steps 4-5.
4. Parent must contain `.git` that is a directory or regular file via no-follow stat; a symlink `.git` does not count (project.cpp:27-32).
5. Outermost rule: walking up from `project` (exclusive) to ROOT (inclusive), no directory may contain `.git` (dir or file) (project.cpp:34-54). Only directories between project and ROOT count; anything above ROOT is ignored.
6. A marker paired with this name must be a regular file (no-follow) in the parent (project.cpp:20-25, 113-118). Pairing is strict: `package.json` does not license `target`.
- [observed] Root `b` containing `repo/.git`, `repo/CMakeLists.txt`, `repo/package.json`: `repo/build/` and `repo/dist/` matched; `repo/lib/sm/build` (sm has `.git` file) rejected; `repo/nogit/build` rejected; `repo/target` (no Cargo.toml) rejected. With ROOT `repo/lib/sm`, its `build/` and `target/` match.
- Nested repos: rejected iff some directory strictly above `project` up to and including ROOT has `.git` [source project.cpp:38-51]. Naming the nested repo as ROOT makes it match.
- ROOT inside a repo: a `build/` beside ROOT's own `.git`? If ROOT == project, loop does not run, so it matches (cli.sh:899) [source].
- No git-tracked/ignored check; `.git` presence only [source].
- Matched artifact dir is not descended (so nested `build/sub/build` is not separately listed) [observed].
- project_roots: see 16. Matching is exact path equality against the parent; no `.git` or outermost check for those parents [observed `repo/pk/a/build` matched with no `.git`].

## 8. Dependencies (-d)

Built-in table (include/cclean/defaults.hpp:90-109), checked in order; first row whose marker is a regular file (no-follow) in the parent wins (src/project.cpp:123-141):

| Directory | Marker | Restore (verbatim) |
|-|-|-|
| `.venv` | `uv.lock` | `uv sync` |
| `node_modules` | `package-lock.json` | `npm ci` |
| `node_modules` | `npm-shrinkwrap.json` | `npm ci` |
| `node_modules` | `yarn.lock` | `yarn install --immutable` |
| `node_modules` | `pnpm-lock.yaml` | `pnpm install --frozen-lockfile` |
| `node_modules` | `bun.lock` | `bun install --frozen-lockfile` |
| `node_modules` | `bun.lockb` | `bun install --frozen-lockfile` |
| `vendor` | `go.mod` | `go mod vendor` |

- No `.git`, outermost-project, or project_roots requirement [source project.cpp:123-153].
- Real directories only (scan.cpp:167-170). Matched tree is not descended, so nested `node_modules/x/node_modules` is not separate [observed].
- [observed] `both/` with `yarn.lock` + `pnpm-lock.yaml` -> `yarn install --immutable` (table order). `nolock/node_modules` beside `package.json` only -> not matched.
- `dependency_markers` (config): array of `[dir, marker]` pairs, checked after the built-ins; match carries no restore (project.cpp:143-150) [observed `ex/deps` no restore].
- Pair elements must be non-empty, not `.`/`..`, contain no `/` (src/toml.cpp:212-219) [observed error].
- Gated by -d / `dependencies = true`; configured pairs are inert without it (cli.sh:400-401).
- Restore shown in human list as `  restore: <cmd>` and JSON `restore` (main.cpp:212-215; json.cpp:72-74) [observed].

## 9. Filters

Applied after sizing, to the target list only (scan.cpp:418-454). Both must pass (AND). No filter -> no-op.

`--older-than D` (scan.cpp:433-442; src/filters.cpp:158-181):
- Timestamp: mtime only.
- File target: its mtime (`last_write_time`). Symlink target: the link's own lstat mtime, whole seconds (scan.cpp:212-215).
- Directory target: max of the directory's own mtime and the mtimes of all non-symlink descendants (files and subdirectories) found while sizing (scan.cpp:223, 347-356, 403-406) [observed: dir mtime 2020 with a fresh file inside is excluded by `--older-than 1d`].
- Keep iff `floor_sec(mtime) <= floor_sec(now) - limit` (filters.cpp:163-180). `0s` keeps everything [observed].
- No readable time -> dropped with warning `Cannot apply age filter to <path>` (scan.cpp:434-437).
- Grammar (filters.cpp:96-124, 18-92): `DIGITS [ '.' DIGITS ] UNIT`, at least one digit total, unit one of `s m h d w` (1, 60, 3600, 86400, 604800 s), case-sensitive. Fraction digits beyond 9 validated then ignored. Result truncated to whole seconds. Max INT64_MAX seconds.
  - [observed accepted] `1d`, `1.5h`, `0s`, `.5d`, `5.d`, `1w`. [observed rejected] `1D`, `1`, `1dd`, `1d5d`, `-1d`, `1e3s`, `.`.

`--larger-than S` (scan.cpp:444-446; filters.cpp:126-156):
- Keep iff `target.size >= limit` (inclusive). Size = logical size from 10.
- Grammar: `DIGITS [ '.' DIGITS ] [SUFFIX]`; suffix `""`/`B` = 1, `K`/`KiB` = 1024, `M`/`MiB` = 1024^2, `G`/`GiB` = 1024^3, `T`/`TiB` = 1024^4. Case-sensitive; no spaces; no `KB`, `k`, `P`. Exact fixed-point, truncated toward zero; max UINTMAX_MAX.
  - [observed accepted] `1536`, `1.5K`, `1.5KiB`, `0`, `101B`, `18446744073709551615B` (matches nothing), `1.4999999999K`. [observed rejected] `1k`, `1KB`, `1 K`, `18446744073709551616`, `16777216T`.
- Config values use the same parsers; error message still names `--older-than`/`--larger-than` (main.cpp:349-367) [observed].

## 10. Sizing

- Apparent (logical) bytes: `directory_entry::file_size()` = st_size of regular files (scan.cpp:49-63, 378-390). Not allocated blocks [observed sparse 1 MiB file counted 1048576; `du -sB1` = 8192].
- Hard links: counted once per link, no inode dedup [observed 1000-byte file + hard link -> 2000].
- Symlinks: 0 bytes, never followed (scan.cpp:341-345; target.hpp:36-37).
- Directories: own st_size not counted; only regular files beneath (recursively) [observed `__pycache__` with 10+2 bytes -> `12 B`].
- Special files: 0.
- Additions saturate at UINTMAX_MAX (src/text.cpp:9-15).
- `Target::entries` = count of all entries below a directory target (scan.cpp:329, 402); used for removal progress and the split threshold.
- Sizing errors: `Cannot inspect ...`, `Cannot determine size of <path>: <msg>`, `Cannot determine modification time of <path>: <msg>`, `Error scanning <dir>: <msg>` (scan.cpp:57, 321, 335, 351, 367, 381, 394).
- Formatting `format_size` (src/text.cpp:17-41): units `B KiB MiB GiB TiB PiB`, base 1024.
  - < 1024: `"%llu B"` exact integer, e.g. `0 B`, `12 B`.
  - else divide by 1024 while >= 1024 (max PiB) then `"%.2f <unit>"` (printf rounding, round-half-even on the binary double), e.g. 2000 -> `1.95 KiB`, 2012 -> `1.96 KiB` [observed]. 1024 -> `1.00 KiB` [source].
- Progress phase `sizing`, count = directories sized so far (scan.cpp:412).

## 11. Confirmation

Flow (cli/main.cpp:457-498; cli/terminal.cpp:171-190):
- `--dry-run`: no prompt; after list prints `Dry run: nothing was removed.` (dim) [observed]. Exit 0/3.
- Empty result: no prompt (main.cpp:434-443).
- `-y`: no prompt, removal proceeds [observed].
- Otherwise prompt:
  - human: to stdout: `\nPermanently remove? [y/N] ` (`[y/N]` bold when stdout colour on) (main.cpp:480-481).
  - json: to stderr: `Permanently remove <N> targets? [y/N] ` (no singular form: `1 targets`) (main.cpp:477-478) [observed].
  - Then flushed; after the answer writes `y` or `n` and `\n` to the same stream (main.cpp:484-486) [observed `[y/N] y$`].
- Answer: yes iff the single byte read is `y` or `Y`. Anything else, including EOF, is no (terminal.cpp:171-190) [observed `n`, EOF `</dev/null`, `Y`; `printf yes` -> yes after reading one byte].
- stdin not a tty: `getchar()` one byte; no line required (terminal.cpp:172-175).
- stdin a tty: termios raw (ICANON and ECHO off, VMIN=1, VTIME=0) applied with TCSAFLUSH, which discards typeahead; one keypress, no Enter; restored with TCSAFLUSH, retried on EINTR (terminal.cpp:131-169). If raw mode fails: `getchar()` (line-buffered) (179-183).
- Restore failure message: `Warning: could not restore the terminal: <strerror>\n  Run 'stty sane' if typing stops echoing.` (terminal.cpp:164-166).
- Declined: human `Cancelled.`; json document status `cancelled`. Exit 0 (3 if warnings) (main.cpp:489-498) [observed].
- pty branch tested via python3 (tests/cli.sh:1154-1242).

## 12. Removal

Dispatch (src/remove.cpp:752-889; cli/main.cpp:500-521):
- All targets passed to `remove_targets(result, progress, split)`.
- Overlap pass: targets keyed by lexically normalized path; a target covered by an earlier directory target (or an identical path) is not dispatched and inherits its owner's result (remove.cpp:776-826, 870-878). Never triggers for scan output [source].
- Targets with `entries >= 4096` and split on -> "large"; removed one at a time after the pool, each split across the pool. Others go to the worker pool, one target per job (remove.cpp:760, 835-864).
- Results returned in list order (remove.cpp:82-83, 874-880).

Per target `remove_counted` (remove.cpp:675-748):
1. Reject name empty/`.`/`..` or relative path empty/`.`: `Target is not a removable entry: <path>` (685-689).
2. `open_parent`: `open(root, O_RDONLY|O_DIRECTORY|O_CLOEXEC)` (follows), then each interior component `openat(..., O_NOFOLLOW|O_DIRECTORY)` (145-221). Errors:
   - component `..`/`.`/empty: `Target is not below the root it was found in: <path>` (169-174).
   - ENOENT: `Target changed or disappeared: <path>` (187-191).
   - now a symlink: `Path component is now a symlink, refusing to follow it: <walked>` (201-206).
   - else `<walked>: <strerror>` (208-209).
3. `fstatat(parent, name, AT_SYMLINK_NOFOLLOW)`; ENOENT -> `Target changed or disappeared: <path>`, else `<path>: <strerror>` (699-705).
4. Type check vs scan (symlink flag; dir flag for non-symlinks): `Target changed type: <path>` (710-715).
5. Identity check st_dev+st_ino vs scan: `Target was replaced since it was scanned: <path>` (722-727). Hand-built targets without identity skip this (226-230).
6. Directory: `openat(O_NOFOLLOW|O_DIRECTORY)`, `fstat` the fd and re-check dir + identity (same messages; ENOTDIR/ELOOP -> `Target changed type`), empty it through the fd, then `unlinkat(parent, name, AT_REMOVEDIR)` (512-572).
7. Non-directory: `unlinkat(parent, name, 0)` one syscall after the identity check (TOCTOU window narrowed, not closed) (735-743).
- Emptying (`remove_entry_at`, 283-338): read all names first, then per child try `unlinkat(0)`; on EISDIR/EPERM open it O_NOFOLLOW|O_DIRECTORY, recurse, `unlinkat(AT_REMOVEDIR)`. Error message `<shown path>: <strerror>`.
- Non-split emptying stops at the first failing child (remove.cpp:263-268). Split emptying runs every job, reports the first error in job order (467-491; CHANGELOG.md:17).
- Split (`empty_directory_split`, 380-505): BFS opening subdirectories via held fds until >= 4*workers subdirs, depth 4, or > 256 held; leaf entries become jobs; held dirs removed deepest-first afterwards [observed: 5050-entry `__pycache__` removed with and without `--no-split`].
- Read-only directories: no chmod; children of a non-writable dir fail with `Permission denied` [observed: `failed  rm1/ro/z.pyc: Permission denied`, exit 1]. Partial removal of a directory target is possible.
- Entries created inside a directory during emptying are removed too (README.md:371).
- Excludes/skips are not honoured inside a matched directory (4, 5).
- Progress: phase `removing`, entries unlinked, reported per 256 and at end; expected total = sum(entries+1) (remove.cpp:53-84; main.cpp:506-519).
- After removal, CLI (main.cpp:525-552):
  - each failure: stderr `  failed  <display(error)>` (`failed` red), error appended to warnings [observed].
  - each success with `-v` (human only): stdout `  removed  <path>` (`removed` dim; no trailing `/`), printed after all removals complete [observed].
  - summary (human): no failures -> `Removed <n>, <format_size(total)> reclaimed` (`Removed <n>` green); else `Removed <n>, <m> failed` (`<m> failed` red) with no byte figure [observed].
- "Bytes freed" = sum of scanned target sizes (apparent, pre-removal), not measured; printed only when 0 failures (main.cpp:445-449, 545-547). JSON has no bytes-freed field; `total.bytes` is the scanned total.

## 13. Text output

Streams [source main.cpp; observed]:
- stdout: matched list, totals, reason breakdown, `Dry run: ...`, `No matching targets found.`, human prompt and its echo, `removed` lines, summary, `Cancelled.`, JSON document, `--help`, `--version`.
- stderr: warnings block, `Note:` lines, `failed` lines, JSON-mode prompt, `Configuration: <path>` (-v), progress, all errors, usage after an unknown option.

Order of a run: progress (erased) -> warnings (stderr) -> [empty: `No matching targets found.`] -> notes (stderr) -> list (stdout) -> dry-run line | prompt -> removal progress -> failed/removed lines -> summary.

Formats (main.cpp:108-227, 434-552):
- Warnings (only if any): `\n` + `Warnings:` (yellow) + `\n`, then per warning `  <display(w)>\n` (main.cpp:114-118). Printed in human and JSON modes.
- No targets: `No matching targets found.\n` (main.cpp:439).
- List: `\nMatched targets:\n`; per target:
  - directory: `  <cyan><display(path)>/<reset><dim>  <size><reset>`
  - other (file, symlink, special): `  <display(path)><dim>  <size><reset>`
  - dependency with restore: append `<dim>  restore: <cmd><reset>`; then `\n`.
- Total: `\n<bold>N<reset> target(s), <bold><size><reset> to reclaim\n`; `target` when N == 1 (main.cpp:220-224).
- Reason breakdown only if >= 2 reasons present (main.cpp:162-165). Rows in fixed order default, config, command-line, build-artifact, dependency; each `  ` + dim + name left-padded to longest present name + `  ` + count right-aligned width 4 + `  ` + size right-aligned to widest present size + reset (main.cpp:180-191). Example [observed]:
  ```
  9 targets, 1.96 KiB to reclaim
    default          7  1.96 KiB
    command-line     2       0 B
  ```
- Dry run: `<dim>Dry run: nothing was removed.<reset>\n`.
- Symlink to dir shown without `/` [observed `./link.pyc  0 B`].

Colour (cli/terminal.cpp:18-40; main.cpp:429-430):
- Decided separately for stdout and stderr, after the scan.
- `never`: none. `always`: always, overrides NO_COLOR [observed]. `auto`: colour iff the fd is a tty AND `NO_COLOR` is unset or empty.
- TERM is not consulted; no other env var read (only getenv is NO_COLOR, terminal.cpp:24) [source].
- SGR codes: reset `\033[0m`, dim `\033[2m`, bold `\033[1m`, directory cyan `\033[36m`, warning yellow `\033[33m`, failure red `\033[31m`, success green `\033[32m`. `Note:` uses warning colour.
- JSON output is never coloured (write_json has no styles).

Progress (cli/terminal.hpp:34-61; terminal.cpp:42-121; main.cpp:419-427, 512-521):
- Enabled iff stderr is a tty; independent of --color, --format, NO_COLOR.
- Scan/size: `\r  <phase> <count>` padded with spaces to the previous width; redrawn only every 256th callback and >= 80 ms since the last draw (or since construction).
- Removal: `\r  removing [<30 chars of # and ->] <pct 3-wide>%  <done> / <total> entries`; 80 ms limit only.
- Erased with `\r<spaces>\r` on finish.

Verbose: `Configuration: <display(path)>` to stderr when a config was loaded (main.cpp:317-319) [observed]; `  removed  <path>` per success (human only). No per-entry log within a directory.
Quiet mode: none [source].

Escaping, human (src/text.cpp:43-64) [observed]:
- `\` -> `\\`; bytes 0x00-0x1f and 0x7f -> `\xNN` lowercase hex; bytes >= 0x80 passed through raw (invalid UTF-8 untouched).
- e.g. newline -> `\x0a`, ESC -> `\x1b`, TAB -> `\x09`, DEL -> `\x7f`.
- Applied to target paths, warnings, failure errors, note patterns/root, config path. Not to restore strings or to ROOT in root error messages.

## 14. JSON

`--format json`: one document on stdout, written by `write_json` (src/json.cpp:11-107). Hand-formatted; exact layout below. Always valid UTF-8.

Schema (all keys always present, in this order):
- `schema`: integer, `1` (include/cclean/json.hpp:26).
- `root`: string, ROOT as given.
- `config`: string path of the config file used (absolute if discovered, as typed if `--config`), or `null` (json.cpp:47-48) [observed both].
- `status`: string, first true of: `failed` (failed != 0), `empty` (no targets), `cancelled`, `dry-run`, `removed` (json.cpp:31-42). Empty + dry run -> `empty` [observed].
- `targets`: array of objects, sorted as the list; each: `path` (string), `type` (`file`|`directory`|`symlink`; symlink wins over directory; specials are `file`), `bytes` (integer), `reason` (`default`|`config`|`command-line`|`build-artifact`|`dependency`), `restore` (string, only for built-in dependency matches). One object per line.
- `total`: `{"targets": int, "bytes": int}` over all targets (not only removed ones).
- `stats`: object keyed by reason name, only reasons present, keys in std::map (alphabetical) order; each `{"targets": int, "bytes": int}`.
- `removed`: int, `failed`: int (0 unless removal ran).
- `warnings`: array of strings: scan, sizing, filter warnings and removal error messages.

String escaping (src/text.cpp:107-147): `"`->`\"`, `\`->`\\`, `\n`, `\r`, `\t`; other C0 and 0x7f -> `\u00XX` lowercase; valid UTF-8 passed; each invalid byte -> U+FFFD [observed `bad\377\376.pyc` -> two U+FFFD; ESC -> `\u001b`].

When emitted: empty result (main.cpp:435-437), dry run (458-460), declined (490-492), after removal (543-544). Human list/summary suppressed; warnings, notes, prompt, failed lines still go to stderr [observed].

Example 1 (dry run) [observed, `cclean --no-config -n --format json . '*.log'`]:
```
{
  "schema": 1,
  "root": ".",
  "config": null,
  "status": "dry-run",
  "targets": [
    {"path": "./.DS_Store", "type": "file", "bytes": 0, "reason": "default"},
    {"path": "./.pytest_cache", "type": "directory", "bytes": 2000, "reason": "default"},
    {"path": "./a.pyo", "type": "file", "bytes": 0, "reason": "default"},
    {"path": "./broken.pyc", "type": "symlink", "bytes": 0, "reason": "default"},
    {"path": "./link.pyc", "type": "symlink", "bytes": 0, "reason": "default"},
    {"path": "./notes.log", "type": "file", "bytes": 0, "reason": "command-line"},
    {"path": "./pkg/__pycache__", "type": "directory", "bytes": 12, "reason": "default"},
    {"path": "./sub/deep/n.log", "type": "file", "bytes": 0, "reason": "command-line"},
    {"path": "./top.pyc", "type": "file", "bytes": 0, "reason": "default"}
  ],
  "total": {"targets": 9, "bytes": 2012},
  "stats": {
    "command-line": {"targets": 2, "bytes": 0},
    "default": {"targets": 7, "bytes": 2012}
  },
  "removed": 0,
  "failed": 0,
  "warnings": [
  ]
}
```
Empty arrays/objects print as `[\n  ]` / `{\n  }` [observed].

Example 2 (removal with one failure, `echo y | cclean --no-config --format json rm1`) [observed]; stderr got `Permanently remove 3 targets? [y/N] y` and `  failed  rm1/ro/z.pyc: Permission denied`; exit 1:
```
  "status": "failed",
  ...
  "total": {"targets": 3, "bytes": 4},
  "removed": 2,
  "failed": 1,
  "warnings": [
    "rm1/ro/z.pyc: Permission denied"
  ]
```
Declined: `"status": "cancelled"`, removed 0, failed 0 [observed]. `-y`: `"status": "removed"` [observed].

## 15. Exit codes

| Code | Triggers |
|-|-|
| 0 | Completed with no warnings: removed all, nothing matched, dry run, declined; `--help`; `--version` (options.cpp:283, 307; main.cpp:442, 466, 497, 558) |
| 1 | ROOT status error, ROOT not a dir, ROOT not listable (main.cpp:397-416); any removal failed, regardless of warnings (main.cpp:554-556); uncaught std::exception -> `cclean: <what>` (main.cpp:570-575) |
| 2 | Any argv error (1); `--config` and `--no-config`; `--config` file not regular; config load/parse error; project_roots error; invalid older/larger value (CLI or config); no patterns left (main.cpp:297-383) |
| 3 | Non-empty warnings and no removal failure, on empty, dry-run, declined, or successful runs (main.cpp:31, 442, 466, 497, 558) [observed dry run with unreadable dir] |

## 16. Config file

- Name: `.cclean.toml` (src/config.cpp:173).
- Discovery `find_config(root)` (config.cpp:147-186): `fs::absolute(root).lexically_normal()` (lexical, `..` collapsed without resolving symlinks); if `is_directory` errors -> no config; if ROOT is not a directory (missing or file) start at its parent; check `<dir>/.cclean.toml` with `is_regular_file` (follows symlinks; errors ignored); walk up to `/`. No stop at repo, home or filesystem boundary. First hit wins; no merging of multiple files.
- `--config FILE`: must be a regular file (follows symlinks) else exit 2; read instead of searching. `--no-config`: none (main.cpp:290-305).
- Config dir for `project_roots` = parent of the config file (absolute, normalized) (main.cpp:327-337).
- Keys (config.cpp:71-141; include/cclean/config.hpp:15-27):

| Key | Type | Default | Meaning |
|-|-|-|-|
| `patterns` | string array | `[]` | extra patterns, reason `config`, NameOrPath scope; always applied |
| `excludes` | string array | `[]` | excludes; replaced wholesale by any CLI `-e` |
| `defaults` | bool | `true` | built-in patterns on/off |
| `build_artifacts` | bool | `false` | like `-b` |
| `dependencies` | bool | `false` | like `-d` |
| `skip_protected` | bool | `true` | skip list on/off |
| `split` | bool | `true` | split large targets |
| `dependency_markers` | array of 2-string arrays | `[]` | extra `[dir, marker]` dependency pairs |
| `project_roots` | string array | `[]` | dirs (relative to config dir) treated as projects for -b |
| `older_than` | quoted string | `""` | like `--older-than` |
| `larger_than` | quoted string | `""` | like `--larger-than` |

- Precedence (main.cpp:33-61): a CLI option that appeared overrides the config value; else config; else built-in default. Patterns: concatenated, never overridden. `dependency_markers` and `project_roots` have no CLI counterpart.
- `project_roots` validation (main.cpp:70-106), run on every run even without -b [observed]:
  - absolute -> `project_roots entries must be relative to .cclean.toml: <entry>`
  - normalizes to `..` or `../...` -> `project_roots entry is outside .cclean.toml's directory: <entry>`
  - not an existing dir -> `project_roots entry is not a directory: <entry>`; all exit 2.
- Errors: unreadable -> `Cannot read config file: <path>`; others `<path>:<line>: <message>` exit 2 (config.cpp:13-15, 20-24) [observed]. Messages: `expected key = value`, `duplicate key <k>`, `unknown key <k>`, `patterns must be an array of strings`, `excludes must be an array of strings`, `project_roots must be an array of strings`, `<bool key> must be true or false`, `dependency_markers must be an array of [directory, marker] string pairs, each a plain name`, `older_than must be a quoted duration`, `larger_than must be a quoted size`.
- Unknown key: error. Missing key: default. Duplicate key: error at second occurrence (config.cpp:66-69) [observed].

TOML subset (src/toml.hpp:6-10; toml.cpp; config.cpp:30-60) [source + observed]:
- Line-oriented; each non-blank line must be `key = value` on one line. No tables (`[table]` -> `expected key = value`), no dotted/quoted keys (`"patterns"` -> `unknown key "patterns"`), no multi-line arrays (`patterns = [` rejected), no inline tables, no integers/floats/dates.
- Comment: `#` outside double quotes to end of line; quote state toggles on every `"` (config.cpp:38-46). `#*#` inside quotes kept [observed].
- Trim: space, tab, CR, LF at both ends (toml.cpp:112-119). Key = text before first `=` trimmed; spaces around `=` optional [observed `patterns=[...]`].
- Bool: exactly `true` or `false` (`False`, `"false"` rejected) [observed].
- String: basic `"..."` only; no single quotes, no escapes, backslash anywhere rejected [observed `'*.tmp'`, `"a\\b"` rejected].
- String array: `[` ... `]`, elements `"..."` separated by exactly one comma; blanks = space/tab; one trailing comma allowed; `[]` ok; `[,"a"]`, `["a",,]` rejected; trailing text after `]` rejected [observed].
- Pair array: `[[ "a", "b" ], ...]`, each inner exactly 2 strings; bracket matching is quote-aware (toml.cpp:76-108).
- A UTF-8 BOM would become part of the first key -> unknown key [source inference].

## 17. Platform differences

- POSIX only: dirent/fcntl/unistd/termios (README.md:55; CMakeLists.txt:22-23). No Windows support or code. No `#ifdef` for platforms anywhere in src/, cli/, include/ (grep) [source].
- CI: Linux GCC + Clang, macOS Clang; ASan+UBSan and TSan on Linux (.github/workflows/ci.yml:14-25, 52-64; CHANGELOG.md:121).
- unlinkat on a directory returns EISDIR (Linux) or EPERM (BSD/macOS); both trigger the directory path (src/remove.cpp:302-310).
- O_NOFOLLOW on a symlink reports ELOOP or ENOTDIR; disambiguated with fstatat (remove.cpp:194-206).
- APFS rejects non-UTF-8 names with EILSEQ, so that test is skipped on macOS (tests/cli.sh:1105-1108).
- macOS `$TMPDIR` under `/var -> private/var` affects PATH_MAX tests (tests/cli.sh:605-606).
- pty TCSAFLUSH blocks until master reads; relevant on macOS (README.md:114; CHANGELOG.md:155).
- `fs::path` sort and `lexically_*` are byte-based on POSIX; no Unicode normalization (macOS NFD names compared as bytes) [source inference].

## 18. Other

- Library `libcclean.a` (CMake target `cclean_core`, OUTPUT_NAME `cclean`), static, links Threads::Threads (CMakeLists.txt:31-61). The library prints nothing, reads no env var (include/cclean/cclean.hpp:4-7).
- Public headers (include/cclean/, installed): `cclean.hpp` (umbrella + `const char* version()`), `scan.hpp` (`ScanOptions`, `ScanResult{root,targets,warnings}`, `ProgressFn`, `compile_patterns`, `compile_excludes`, `scan`), `target.hpp` (`Reason`, `reason_name`, `Target{path,size,entries,is_directory,is_symlink,device,inode,has_identity,restore,reason,newest_time,has_time}`), `remove.hpp` (`remove_target`, `RemovalResult`, two `remove_targets` overloads), `config.hpp` (`Config`, `load_config`, `find_config`), `glob.hpp` (`Glob`, `matches_any`, `first_match`), `filters.hpp` (`parse_duration`, `parse_size`, `is_older_than`), `project.hpp` (`normalize_directory`, `has_marker_file`, `has_repository`, `has_enclosing_project`, `is_artifact_directory`, `is_dependency_directory`, `is_skipped`), `defaults.hpp` (tables + `builtin_patterns`, `artifact_directories`, `skipped_directories`), `json.hpp` (`Outcome`, `json_schema_version`, `write_json`), `text.hpp` (`saturating_add`, `format_size`, `display`, `json_string`).
- Internal, not installed: `src/toml.hpp`, `src/parallel.hpp`, `src/removal_hooks.hpp` (test hooks `cclean::testing::set_removal_window_hook`, `set_walk_step_hook`, compiled into every build).
- `load_config` merges into an existing Config (arrays append) (config.hpp:29-34).
- Install (CMakeLists.txt:76-78): `bin/cclean` (component `runtime`), `lib/libcclean.a` and `include/cclean/` (component `development`). No CMake package config, no pkg-config, no man page (TODO.md:11).
- Make (Makefile): `make` (Release into `build/`, `-Werror` via `WERROR`), `make test` (ctest), `make install` (PREFIX default `/usr/local`, `--strip`), `make install-bin` (BIN_PREFIX default `~/.local`, runtime only), `make clean`. Rebuild decided by source cksum (Makefile:131-191).
- Default build type Release (CMakeLists.txt:14-18). Version from CMake `project()`; non-CMake build reports `unknown` (src/version.cpp:5-7).
- Tests: `tests/unit.cpp` (416 checks) and `tests/cli.sh` (234 checks) via ctest, timeouts 120 s / 300 s (CMakeLists.txt:84-111; CHANGELOG.md:21). cli.sh flags any exit status outside {0,1,2,3} (tests/cli.sh:27-56).
- Thread pool: min(cores, 32); thread-creation failure tolerated (src/parallel.hpp:22-26, 120-131). Exceptions in workers rethrown after join.
- No signal handling; no logging; no locale use [source].

## 19. Known gaps, bugs, doc/source disagreements

Bugs / gaps (marked):
- [GAP, observed] Excludes and the skip list do not apply inside a matched directory. `-e '*.pyc'` still removes `.pyc` files inside a matched `__pycache__/`; a `.git` inside a matched dir is sized and removed (scan.cpp:305-413; remove.cpp:242-274; REVIEW.md:15-35). Contradicts help `contents included` (options.cpp:24-25).
- [DOC MISMATCH, observed] `--no-defaults` help/README say "Match only the patterns given on the command line" (options.cpp:41; README.md:149) but config `patterns` still match (scan.cpp:475-478).
- [QUIRK, observed] `--older-than=` / `--larger-than=` with empty value silently disables the filter, including one set in config (options.cpp:184-194; main.cpp:349, 359).
- [QUIRK, observed] Invalid config `older_than`/`larger_than` is reported as `Invalid --older-than duration: ...` with no file:line (main.cpp:349-367).
- [QUIRK, observed] JSON prompt has no singular: `Permanently remove 1 targets? [y/N]` (main.cpp:477-478).
- [QUIRK, observed] Note text with ROOT `.` reads `anywhere under .. One root per run.` (main.cpp:266-268).
- [GAP, source] No CLI way to re-enable `defaults`, `split`, `skip_protected`, `build_artifacts`, `dependencies` after config disables/enables them (TODO.md:27).
- [INCONSISTENCY, source] Sizing still warns `Cannot determine modification time of <child>` even without `--older-than` (scan.cpp:347-353), while the target's own mtime error is deliberately silent (scan.cpp:217-224; CHANGELOG.md:209). Reachability is inference: `last_write_time` on a directory_entry normally comes from a stat that already succeeded.
- [GAP, source] Non-directory removal has a one-syscall TOCTOU window (remove.hpp:36-50; REVIEW.md:45-51).
- [GAP, source] Scan is path-based; a directory replaced by a symlink mid-scan can be followed once for listing/sizing (README.md:373).
- [GAP, source] Mount points crossed; no one-file-system option.
- [GAP, source] Hard links double-counted; sparse files counted at apparent size (README.md:343 acknowledges logical sizes).
- [GAP, source] "reclaimed" figure is the scanned total, not measured; omitted when any failure.
- [GAP, source] ROOT error messages print ROOT unescaped (main.cpp:398, 404).
- [GAP, source] `-v` lines print after all removals, not "as it is removed" (help options.cpp:26 says "Name every item as it is removed"); per-target only, not per item.

Doc vs source:
- README.md:190 claims `.tox_cache` matches; tox's directory is `.tox`, which `.*_cache` does not match. The claim about `.tox_cache` itself is true but the name is unlikely (inference).
- README.md:300 "Colour is used for directories, totals, and failures": also warnings header, notes, dim sizes/restore/breakdown, green success (terminal.cpp:31-38; main.cpp).
- README.md:334 lists cancellation under exit 0; with warnings it is 3 (main.cpp:497).
- Help `--larger-than SIZE (B, K, M, G, T)` omits accepted `KiB/MiB/GiB/TiB` and bare numbers (filters.cpp:141-146).
- Help "Removed by --build-artifacts, when .git and the marker file ... sit beside them" omits the outermost-project rule and project_roots (options.cpp:47-49 vs project.cpp:96-111). README.md:223 and 257 document both.
- README.md:156 "CLI options override configuration values": true only for options that exist; patterns concatenate, excludes replace.
- docs/dev/ is empty in this repo (ls) [observed]; no dev docs to cross-check.
