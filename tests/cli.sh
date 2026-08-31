#!/bin/sh
#
# End-to-end tests for the cclean command line: exit codes, what actually gets
# deleted, confirmation handling, and the shape of the output.
#
# Usage: tests/cli.sh [path-to-cclean]

set -u

CCLEAN=${1:-./cclean}

if [ ! -x "$CCLEAN" ]; then
    echo "cli: no executable at $CCLEAN" >&2
    exit 2
fi

CCLEAN=$(cd "$(dirname "$CCLEAN")" && pwd)/$(basename "$CCLEAN")

# Every command inherits an empty stdin unless it is given one through a pipe.
# A regression that made a non-confirming run prompt would otherwise hang the
# suite instead of failing it.
exec </dev/null

WORK=$(mktemp -d "${TMPDIR:-/tmp}/cclean-cli.XXXXXX")
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT INT TERM

passed=0
failed=0

pass() {
    passed=$((passed + 1))
}

fail() {
    failed=$((failed + 1))
    echo "  FAIL  $1"
    [ $# -gt 1 ] && echo "          $2"
}

# check NAME EXPECTED ACTUAL
check() {
    if [ "$2" = "$3" ]; then
        pass
    else
        fail "$1" "expected [$2], got [$3]"
    fi
}

# Builds a fresh tree and echoes its path: three matching targets, plus a
# source file and a log file that no default pattern names.
fixture() {
    d=$WORK/$1
    rm -rf "$d"
    mkdir -p "$d/pkg/__pycache__" "$d/src" "$d/.pytest_cache"
    printf 'aaaaaaaa' > "$d/pkg/__pycache__/mod.pyc"
    printf 'bb' > "$d/.DS_Store"
    printf 'cccc' > "$d/.pytest_cache/entry"
    printf 'keep' > "$d/src/main.py"
    printf 'log' > "$d/notes.log"
    echo "$d"
}

count_files() {
    find "$1" -type f 2>/dev/null | wc -l | tr -d ' '
}

echo "cli: running"

# ---------------------------------------------------------------- dry run

d=$(fixture dryrun)
before=$(count_files "$d")
"$CCLEAN" --dry-run "$d" >/dev/null 2>&1
check "dry run exits 0" 0 $?
check "dry run deletes nothing" "$before" "$(count_files "$d")"

# Even with a confirmation waiting on stdin, a dry run must not delete.
d=$(fixture dryrun_confirm)
before=$(count_files "$d")
printf 'y' | "$CCLEAN" --dry-run "$d" >/dev/null 2>&1
check "dry run ignores piped confirmation" "$before" "$(count_files "$d")"

# ----------------------------------------------------------- confirmation
#
# These drive the piped branch of the prompt. The terminal branch, which puts
# the terminal in raw mode to take a single keypress and restores it after,
# needs a pseudo-terminal and is not covered here.

# src/main.py and notes.log are not matched by any default pattern, so both
# survive a default run.
d=$(fixture confirm_yes)
printf 'y' | "$CCLEAN" "$d" >/dev/null 2>&1
check "y removes matches" 2 "$(count_files "$d")"
check "y keeps unmatched files" "keep" "$(cat "$d/src/main.py")"
check "y keeps files no pattern names" "log" "$(cat "$d/notes.log")"

d=$(fixture confirm_upper)
printf 'Y' | "$CCLEAN" "$d" >/dev/null 2>&1
check "Y removes matches" 2 "$(count_files "$d")"

d=$(fixture confirm_no)
before=$(count_files "$d")
printf 'n' | "$CCLEAN" "$d" >/dev/null 2>&1
check "n cancels" "$before" "$(count_files "$d")"

d=$(fixture confirm_eof)
before=$(count_files "$d")
"$CCLEAN" "$d" </dev/null >/dev/null 2>&1
check "end of input cancels" "$before" "$(count_files "$d")"

d=$(fixture confirm_other)
before=$(count_files "$d")
printf 'q' | "$CCLEAN" "$d" >/dev/null 2>&1
check "any other key cancels" "$before" "$(count_files "$d")"

# --------------------------------------------------------------- reporting

# The reported total must equal the bytes actually on disk. An implementation
# that counted a directory and its contents separately would double it.
d=$WORK/sizes
rm -rf "$d"
mkdir -p "$d/pkg/__pycache__"
printf '0123456789' > "$d/pkg/__pycache__/a.pyc"
printf '0123456789' > "$d/pkg/__pycache__/b.pyc"
total=$("$CCLEAN" -n "$d" | grep 'to reclaim')
check "size totals do not double count" "1 target, 20 B to reclaim" "$total"

# A matched directory is one line; its contents are not listed again.
lines=$("$CCLEAN" -n "$d" | grep -c 'pyc\|pycache')
check "matched directory listed once" 1 "$lines"

d=$(fixture verbose)
quiet_lines=$(printf 'y' | "$CCLEAN" "$d" | grep -c 'removed ')
check "default output does not list removals" 0 "$quiet_lines"

d=$(fixture verbose_on)
loud_lines=$(printf 'y' | "$CCLEAN" --verbose "$d" | grep -c 'removed ')
check "verbose lists each removal" 3 "$loud_lines"

# ----------------------------------------------------------- configuration

d=$WORK/config
rm -rf "$d"
mkdir -p "$d/sub"
printf 'x' > "$d/remove.tmp"
printf 'x' > "$d/keep.pyc"
printf 'x' > "$d/sub/cache.pyc"
printf 'patterns = ["*.tmp"]\n' > "$d/.cclean.toml"
printf 'excludes = ["keep.pyc"]\n' >> "$d/.cclean.toml"
printf 'defaults = true\n' >> "$d/.cclean.toml"

check "config adds patterns and defaults" "2 targets, 2 B to reclaim" \
      "$($CCLEAN -n "$d" | grep 'to reclaim')"
check "config pattern is reported" 1 "$($CCLEAN -n "$d" | grep -c 'remove.tmp')"
check "config exclude is applied" 0 "$($CCLEAN -n "$d" | grep -c 'keep.pyc')"
check "CLI defaults override config" 0 \
      "$($CCLEAN -n --no-defaults "$d" | grep -c 'cache.pyc')"

printf 'unknown_key = true\n' > "$d/.cclean.toml"
"$CCLEAN" -n "$d" >/dev/null 2>&1
check "unknown config key exits 2" 2 $?

# A # opens a comment only outside quotes; emacs lock and autosave files are
# named with it.
printf 'x' > "$d/#locked#"
printf 'patterns = ["#*#"]  # trailing comment\n' > "$d/.cclean.toml"
printf 'defaults = false\n' >> "$d/.cclean.toml"
check "quoted # is not a comment" 1 "$($CCLEAN -n "$d" | grep -c 'locked')"

d=$WORK/monorepo
rm -rf "$d"
mkdir -p "$d/packages/app/build"
printf 'x' > "$d/packages/app/CMakeLists.txt"
printf 'xx' > "$d/packages/app/build/out"
mkdir -p "$d/packages/other"
printf 'project_roots = ["packages/app", "packages/other"]\n' > "$d/.cclean.toml"
check "configured monorepo project is detected" \
      "1 target, 2 B to reclaim" "$(cd "$d" && "$CCLEAN" -n -b . | grep 'to reclaim')"

# project_roots resolve against the directory holding .cclean.toml, not ROOT.
# Resolving against ROOT made every run from a subdirectory fail, including
# runs that never consult build artifacts.
check "project_roots work from a subdirectory" \
      "1 target, 2 B to reclaim" \
      "$(cd "$d/packages/app" && "$CCLEAN" -n -b . | grep 'to reclaim')"
(cd "$d/packages/app" && "$CCLEAN" -n . >/dev/null 2>&1)
check "config from a parent does not break a plain run" 0 $?
(cd "$d" && "$CCLEAN" -n -b packages/app >/dev/null 2>&1)
check "project_roots survive a ROOT below the config" 0 $?

printf 'project_roots = ["../escape"]\n' > "$d/.cclean.toml"
"$CCLEAN" -n "$d" >/dev/null 2>&1
check "project_roots escaping the config directory exits 2" 2 $?
rm -f "$d/.cclean.toml"

d=$WORK/dependencies
rm -rf "$d"
mkdir -p "$d/node_modules/pkg" "$d/vendor" "$d/random/node_modules"
printf 'x' > "$d/package.json"
printf 'x' > "$d/package-lock.json"
printf 'xx' > "$d/node_modules/pkg/file"
printf 'x' > "$d/vendor/file"
printf 'x' > "$d/random/node_modules/file"
check "dependencies are excluded by default" 0 \
      "$($CCLEAN -n "$d" | grep -c 'node_modules')"
check "marker-guarded dependencies are detected" 1 \
      "$($CCLEAN -n --dependencies "$d" | grep -c 'node_modules/')"
check "unmarked dependency trees are ignored" 0 \
      "$($CCLEAN -n --dependencies "$d" | grep -c 'random/node_modules')"

# package.json carries ranges; only package-lock.json pins the tree that
# `npm ci` reinstalls.
rm -f "$d/package-lock.json"
check "node_modules needs the lock, not the manifest" 0 \
      "$($CCLEAN -n --dependencies "$d" | grep -c 'node_modules/')"
printf 'x' > "$d/package-lock.json"
check "the lock file qualifies node_modules" 1 \
      "$($CCLEAN -n --dependencies "$d" | grep -c 'node_modules/')"

# Every package manager installs into node_modules but writes its own lock,
# so each name needs its own row.
for lock in package-lock.json npm-shrinkwrap.json yarn.lock \
            pnpm-lock.yaml bun.lock bun.lockb; do
    rm -f "$d"/package-lock.json "$d"/npm-shrinkwrap.json "$d"/yarn.lock \
          "$d"/pnpm-lock.yaml "$d"/bun.lock "$d"/bun.lockb
    printf 'x' > "$d/$lock"
    check "$lock qualifies node_modules" 1 \
          "$($CCLEAN -n --dependencies "$d" | grep -c 'node_modules/')"
done

# The list is the six npm-compatible lock names. deno.lock also sits beside a
# node_modules, and is left for dependency_markers.
rm -f "$d"/bun.lockb
printf 'x' > "$d/deno.lock"
check "an unlisted lock name is left alone" 0 \
      "$($CCLEAN -n --dependencies "$d" | grep -c 'node_modules/')"
rm -f "$d/deno.lock"
printf 'x' > "$d/package-lock.json"

# --dependencies matches on layout, so like --build-artifacts it is by itself
# enough to give a --no-defaults run something to do.
check "--no-defaults --dependencies still matches" 1 \
      "$($CCLEAN -n --no-defaults --dependencies "$d" | grep -c 'node_modules/')"
"$CCLEAN" -n --no-defaults --dependencies "$d" >/dev/null 2>&1
check "--no-defaults --dependencies exits 0" 0 $?

# A .venv is a dependency tree only when uv.lock is beside it: that is what
# `uv sync` restores from. A pyproject.toml alone is also produced by poetry,
# pdm and hatch, and by `uv venv` with no lock, none of which rebuild a known
# environment.
d=$WORK/venv_deps
rm -rf "$d"
mkdir -p "$d/uvlocked/.venv/lib" "$d/nolock/.venv/lib" "$d/bare/venv/lib"
printf 'x' > "$d/uvlocked/pyproject.toml"
printf 'x' > "$d/uvlocked/uv.lock"
printf 'xx' > "$d/uvlocked/.venv/lib/f"
printf 'x' > "$d/nolock/pyproject.toml"
printf 'xx' > "$d/nolock/.venv/lib/f"
printf 'x' > "$d/bare/uv.lock"
printf 'xx' > "$d/bare/venv/lib/f"

check "uv-locked .venv is a dependency target" 1 \
      "$($CCLEAN -n --dependencies --no-defaults "$d" | grep -c 'uvlocked/.venv/')"
check "unlocked .venv is left alone" 0 \
      "$($CCLEAN -n --dependencies --no-defaults "$d" | grep -c 'nolock/.venv')"
check "a venv without the dot is left alone" 0 \
      "$($CCLEAN -n --dependencies --no-defaults "$d" | grep -c 'bare/venv')"
check "no .venv is touched without --dependencies" 0 \
      "$($CCLEAN -n "$d" | grep -c '.venv/$')"

# vendor/ is a generic name, so go.mod is what qualifies it. The list covers
# three ecosystems deliberately; a vendor/ belonging to anything else is left
# for a pattern in .cclean.toml.
d=$WORK/vendor_deps
rm -rf "$d"
mkdir -p "$d/golang/vendor" "$d/other/vendor"
printf 'x' > "$d/golang/go.mod"
printf 'xx' > "$d/golang/vendor/f"
printf 'x' > "$d/other/composer.lock"
printf 'xx' > "$d/other/vendor/f"

check "go vendor needs only go.mod" 1 \
      "$($CCLEAN -n --dependencies --no-defaults "$d" | grep -c 'golang/vendor/')"
check "an unlisted ecosystem's vendor is left alone" 0 \
      "$($CCLEAN -n --dependencies --no-defaults "$d" | grep -c 'other/vendor')"

# dependency_markers adds ecosystems the built-in list leaves out. A configured
# pair takes the same marker guard and the same --dependencies gate as a
# built-in, which is what separates it from a plain pattern.
d=$WORK/config_deps
rm -rf "$d"
mkdir -p "$d/app/deps/pkg" "$d/plain/deps" "$d/skipme/deps"
printf 'x' > "$d/app/mix.exs"
printf 'xx' > "$d/app/deps/pkg/f"
printf 'xx' > "$d/plain/deps/f"
printf 'x' > "$d/skipme/mix.exs"
printf 'xx' > "$d/skipme/deps/f"
printf 'dependencies = true\n' > "$d/.cclean.toml"
printf 'dependency_markers = [["deps", "mix.exs"]]\n' >> "$d/.cclean.toml"

check "configured dependency pair is matched" 1 \
      "$($CCLEAN -n --no-defaults "$d" | grep -c 'app/deps/')"
check "configured pair still needs its marker" 0 \
      "$($CCLEAN -n --no-defaults "$d" | grep -c 'plain/deps')"
check "configured pair honours excludes" 0 \
      "$($CCLEAN -n --no-defaults --exclude skipme "$d" | grep -c 'skipme/deps')"

# The gate is the flag, not the key: turning dependencies off in the config
# leaves the configured pairs inert.
printf 'dependencies = false\n' > "$d/.cclean.toml"
printf 'dependency_markers = [["deps", "mix.exs"]]\n' >> "$d/.cclean.toml"
check "configured pairs are gated by --dependencies" 0 \
      "$($CCLEAN -n "$d" | grep -c 'app/deps/')"
check "the flag re-enables configured pairs" 1 \
      "$($CCLEAN -n --dependencies "$d" | grep -c 'app/deps/')"

# A marker is looked up beside the directory, so it has to be a plain name.
for bad in 'dependency_markers = ["deps"]' \
           'dependency_markers = [["deps"]]' \
           'dependency_markers = [["deps", "a", "b"]]' \
           'dependency_markers = [["deps", "../mix.exs"]]' \
           'dependency_markers = [["", "mix.exs"]]'; do
    printf '%s\n' "$bad" > "$d/.cclean.toml"
    "$CCLEAN" -n "$d" >/dev/null 2>&1
    check "malformed dependency_markers exits 2: $bad" 2 $?
done


# Filters operate on matched files and use the directory's newest entry when
# the target is a directory.
d=$WORK/filters
rm -rf "$d"
mkdir -p "$d/cache"
printf 'x' > "$d/old.pyc"
printf '1234567890' > "$d/large.pyc"
touch -t 202001010000 "$d/old.pyc"
check "age filter keeps targets older than the duration" 1 \
      "$($CCLEAN -n --older-than 1d "$d" | grep -c 'old.pyc')"
check "age filter drops targets younger than the duration" 0 \
      "$($CCLEAN -n --older-than 1d "$d" | grep -c 'large.pyc')"
check "size filter selects large targets" 1 \
      "$($CCLEAN -n --larger-than 5B "$d" | grep -c 'large.pyc')"
check "size filter drops small targets" 0 \
      "$($CCLEAN -n --larger-than 5B "$d" | grep -c 'old.pyc')"
"$CCLEAN" -n --older-than nonsense "$d" >/dev/null 2>&1
check "invalid age filter exits 2" 2 $?
"$CCLEAN" -n --larger-than nonsense "$d" >/dev/null 2>&1
check "invalid size filter exits 2" 2 $?

# A suffix that is merely the last character let "1dd" and "1d5d" read as one
# day.
for bad in 1dd 1d5d 1d1; do
    "$CCLEAN" -n --older-than "$bad" "$d" >/dev/null 2>&1
    check "trailing garbage in --older-than $bad exits 2" 2 $?
done

# --------------------------------------------------------------- JSON output

d=$(fixture json)
json=$WORK/result.json
"$CCLEAN" -n --format json "$d" >"$json" 2>"$WORK/json.err"
check "JSON dry run exits 0" 0 $?
check "JSON has no human header" 0 "$(grep -c 'Matched targets' "$json")"
check "JSON has default reasons" 3 "$(grep -c '\"reason\": \"default\"' "$json")"
check "JSON has statistics" 1 "$(grep -c '\"stats\"' "$json")"
check "JSON keeps diagnostics off stdout" 0 "$(grep -c 'Warnings' "$json")"

d=$(fixture json_remove)
"$CCLEAN" --yes --format=json "$d" >"$json" 2>"$WORK/json_remove.err"
check "--yes JSON exits 0" 0 $?
check "--yes removes targets" 2 "$(count_files "$d")"
check "--yes does not prompt" 0 "$(grep -c 'Permanently remove' "$json")"
check "JSON reports removal" 1 "$(grep -c '\"status\": \"removed\"' "$json")"

# A scan warning sets exit 1 but must not relabel a dry run as a failed
# removal: status reports the action, warnings are their own array.
d=$WORK/json_warn
rm -rf "$d"
mkdir -p "$d/locked" "$d/ok"
printf 'x' > "$d/ok/a.pyc"
chmod 000 "$d/locked"
"$CCLEAN" -n --format json "$d" >"$json" 2>/dev/null
check "scan warning exits 1" 1 $?
check "warning keeps the dry-run status" 1 \
      "$(grep -c '\"status\": \"dry-run\"' "$json")"
check "warning is reported in the JSON" 1 \
      "$(grep -c 'Permission denied' "$json")"
chmod 755 "$d/locked"

# ------------------------------------------------------------- exit codes

"$CCLEAN" --help >/dev/null 2>&1
check "--help exits 0" 0 $?

"$CCLEAN" --bogus >/dev/null 2>&1
check "unknown option exits 2" 2 $?

"$CCLEAN" --version >/dev/null 2>&1
check "--version exits 0" 0 $?

version=$("$CCLEAN" --version)
check "--version names the program" "cclean" "${version%% *}"
check "-V matches --version" "$version" "$("$CCLEAN" -V)"

# The version is written once, in CMakeLists.txt, and reaches the program as a
# compile definition. CHANGELOG.md repeats it, so guard the pair against drift.
changelog=$(dirname "$0")/../CHANGELOG.md
if [ -f "$changelog" ]; then
    latest=$(sed -n 's/^## \[\([0-9][^]]*\)\].*/\1/p' "$changelog" | head -1)
    check "--version matches the newest CHANGELOG entry" \
          "cclean $latest" "$version"
fi

"$CCLEAN" -n --no-defaults "$WORK" >/dev/null 2>&1
check "--no-defaults without a pattern exits 2" 2 $?

"$CCLEAN" -n --no-defaults -b "$WORK" >/dev/null 2>&1
check "--no-defaults with -b exits 0" 0 $?

"$CCLEAN" -n "$WORK/does-not-exist" >/dev/null 2>&1
check "missing root exits 1" 1 $?

d=$(fixture not_a_dir)
"$CCLEAN" -n "$d/notes.log" >/dev/null 2>&1
check "root that is not a directory exits 1" 1 $?

d=$WORK/empty
mkdir -p "$d"
"$CCLEAN" -n "$d" >/dev/null 2>&1
check "no matches exits 0" 0 $?

# A directory the process cannot write to cannot have its children removed.
d=$WORK/locked
rm -rf "$d"
mkdir -p "$d/p/__pycache__"
printf 'x' > "$d/p/__pycache__/a.pyc"
chmod 555 "$d/p"
printf 'y' | "$CCLEAN" "$d" >/dev/null 2>&1
check "failed removal exits 1" 1 $?
chmod 755 "$d/p"

# ------------------------------------------------------------- default root

d=$(fixture bare)
( cd "$d" && "$CCLEAN" -n . >"$WORK/bare_dot.txt" 2>&1 )
( cd "$d" && "$CCLEAN" -n  >"$WORK/bare_none.txt" 2>&1 )
if cmp -s "$WORK/bare_dot.txt" "$WORK/bare_none.txt"; then
    pass
else
    fail "bare invocation equals '.'"
fi

# ------------------------------------------------------------- skip list

d=$WORK/skips
rm -rf "$d"
mkdir -p "$d/.git/objects" "$d/.ssh" "$d/src/__pycache__"
printf 'a' > "$d/.git/objects/cached.pyc"
printf 'c' > "$d/.ssh/id.pyc"
printf 'd' > "$d/src/__pycache__/m.pyc"

check "skip list leaves one match" "1 target, 1 B to reclaim" \
      "$("$CCLEAN" -n "$d" | grep 'to reclaim')"

check "protected directories are not entered" 0 \
      "$("$CCLEAN" -n "$d" | grep -c '\.git\|\.ssh')"

check "--no-skip enters them" 2 \
      "$("$CCLEAN" -n --no-skip "$d" | grep -c '\.git\|\.ssh')"

printf 'y' | "$CCLEAN" "$d" >/dev/null 2>&1
check "protected content survives removal" 1 "$(count_files "$d/.git")"

# Virtual environments are walked. They are where most of a Python project's
# __pycache__ lives, so protecting them gave up most of what the tool is for.
d=$WORK/venvs
rm -rf "$d"
mkdir -p "$d/.venv/lib/__pycache__" "$d/venv/lib/__pycache__"
printf 'ab' > "$d/.venv/lib/__pycache__/m.pyc"
printf 'cd' > "$d/venv/lib/__pycache__/m.pyc"

check "virtual environments are cleaned" "2 targets, 4 B to reclaim" \
      "$("$CCLEAN" -n "$d" | grep 'to reclaim')"

printf 'y' | "$CCLEAN" "$d" >/dev/null 2>&1
check "virtual environment caches are removed" 0 "$(count_files "$d")"

# A pattern naming a protected directory still cannot reach it.
d=$WORK/skips_explicit
rm -rf "$d"
mkdir -p "$d/.git/objects"
printf 'a' > "$d/.git/objects/f"
printf 'y' | "$CCLEAN" --no-defaults "$d" ".git" >/dev/null 2>&1
check "explicit pattern cannot delete a protected directory" 1 \
      "$(count_files "$d")"

# ---------------------------------------------------------- pattern scope

# The relative path ".tools/lib/foo_cache" begins with a dot and ends with
# _cache, so ".*_cache" matches it when tested against the path. The filename
# "foo_cache" has no leading dot, so it does not match when tested by name.
# This is the exact false positive that the built-in name scope prevents.
d=$WORK/scope
rm -rf "$d"
mkdir -p "$d/.tools/lib/foo_cache" "$d/.ruff_cache"
printf 'a' > "$d/.tools/lib/foo_cache/pkg.py"
printf 'b' > "$d/.ruff_cache/entry"

check "built-in pattern does not match by path" 0 \
      "$("$CCLEAN" -n "$d" | grep -c 'foo_cache')"
check "built-in pattern matches by name" 1 \
      "$("$CCLEAN" -n "$d" | grep -c 'ruff_cache')"

# The same pattern supplied on the command line is also path-scoped, and does
# match. The scope is the only difference between the two cases.
check "supplied pattern matches by path" 1 \
      "$("$CCLEAN" -n --no-defaults "$d" ".*_cache" | grep -c 'foo_cache')"

# ------------------------------------------------------------- symlinks

d=$WORK/links
rm -rf "$d"
mkdir -p "$d/real"
printf '0123456789012345678901234567890123456789' > "$d/real/big.dat"
ln -s real/big.dat "$d/link.pyc"
ln -s real "$d/dir.pyc"

check "symlink counts as zero bytes" 1 \
      "$("$CCLEAN" -n "$d" | grep -c 'link.pyc  0 B')"

printf 'y' | "$CCLEAN" "$d" >/dev/null 2>&1
check "symlink target survives" 1 "$(count_files "$d/real")"
check "symlink itself is removed" 0 "$(ls "$d" | grep -c 'link.pyc')"

# --------------------------------------------------------- build artifacts

d=$WORK/artifacts
rm -rf "$d"
mkdir -p "$d/cmake/.git" "$d/cmake/build" "$d/cargo/.git" "$d/cargo/target" \
         "$d/no_git/build" "$d/no_marker/.git/x" "$d/no_marker/build" \
         "$d/stray/build"
printf 'x' > "$d/cmake/CMakeLists.txt"
printf 'x' > "$d/cargo/Cargo.toml"
printf 'x' > "$d/no_git/CMakeLists.txt"
for p in cmake/build cargo/target no_git/build no_marker/build stray/build; do
    printf 'yy' > "$d/$p/out"
done

check "artifacts are not removed by default" 0 \
      "$("$CCLEAN" -n "$d" | grep -c 'build/\|target/')"

check "-b finds exactly the two real projects" "2 targets, 4 B to reclaim" \
      "$("$CCLEAN" -n -b "$d" | grep 'to reclaim')"

check "-b requires .git" 0 "$("$CCLEAN" -n -b "$d" | grep -c 'no_git')"
check "-b requires a marker file" 0 "$("$CCLEAN" -n -b "$d" | grep -c 'no_marker')"
check "-b ignores unmarked directories" 0 "$("$CCLEAN" -n -b "$d" | grep -c 'stray')"

printf 'y' | "$CCLEAN" -b "$d" >/dev/null 2>&1
check "-b removes the build directory" 0 "$(count_files "$d/cmake/build")"
check "-b leaves the marker file" 1 "$(count_files "$d/cmake" | tr -d ' ')"

# --------------------------------------------------------------- excludes

d=$WORK/excludes
rm -rf "$d"
mkdir -p "$d/src/__pycache__" "$d/.venv/lib/__pycache__" "$d/tests/fixtures/__pycache__"
printf 'aa' > "$d/src/__pycache__/m.pyc"
printf 'bb' > "$d/.venv/lib/__pycache__/m.pyc"
printf 'cc' > "$d/tests/fixtures/__pycache__/m.pyc"
printf 'd' > "$d/top.pyc"

check "no excludes matches everything" "4 targets, 7 B to reclaim" \
      "$("$CCLEAN" -n "$d" | grep 'to reclaim')"

# An exclude prunes, so naming a directory keeps everything under it. This is
# how a virtual environment is protected now that the built-in list does not.
check "--exclude prunes the subtree" 0 \
      "$("$CCLEAN" -n "$d" -e .venv | grep -c 'venv')"
check "--exclude=VALUE form" 0 \
      "$("$CCLEAN" -n "$d" --exclude=.venv | grep -c 'venv')"
check "-e prunes a nested subtree" 0 \
      "$("$CCLEAN" -n "$d" -e tests | grep -c 'fixtures')"
# "*.pyc" excludes top.pyc; src/__pycache__ is claimed whole and its name
# does not end in .pyc, so it survives the exclude and its 2 bytes remain.
check "-e is repeatable" "1 target, 2 B to reclaim" \
      "$("$CCLEAN" -n "$d" -e .venv -e tests -e "*.pyc" | grep 'to reclaim')"
check "-e excludes a file" 0 \
      "$("$CCLEAN" -n --no-defaults "$d" "*.pyc" -e top.pyc | grep -c 'top.pyc')"

"$CCLEAN" -n "$d" -e >/dev/null 2>&1
check "-e without a pattern exits 2" 2 $?
"$CCLEAN" -n "$d" --exclude >/dev/null 2>&1
check "--exclude without a pattern exits 2" 2 $?

# The value is taken verbatim, so it may itself begin with a dash.
"$CCLEAN" -n "$d" -e "-weird" >/dev/null 2>&1
check "-e takes a dashed value verbatim" 0 $?

printf 'y' | "$CCLEAN" "$d" -e .venv >/dev/null 2>&1
check "excluded content survives removal" 1 "$(count_files "$d/.venv")"

# ------------------------------------------------------- build ecosystems

d=$WORK/ecosystems
rm -rf "$d"
# One project per ecosystem: .git, the marker file, and the artifact directory.
for pair in "js:dist:package.json" "next:.next:package.json" \
            "svelte:.svelte-kit:package.json" "cra:build:package.json" \
            "gradle:build:build.gradle" "gradlekt:.gradle:build.gradle.kts" \
            "maven:target:pom.xml" "py:dist:pyproject.toml" \
            "pysetup:build:setup.py" "zig:zig-out:build.zig" \
            "swift:.build:Package.swift" "elixir:_build:mix.exs" \
            "flutter:build:pubspec.yaml" "meson:build:meson.build" \
            "rust:target:Cargo.toml" "cmake:build:CMakeLists.txt"; do
    name=${pair%%:*}
    rest=${pair#*:}
    art=${rest%%:*}
    marker=${rest#*:}
    mkdir -p "$d/$name/.git" "$d/$name/$art"
    printf 'x' > "$d/$name/$marker"
    printf 'yy' > "$d/$name/$art/out"
done

# Negatives: no .git, no marker, and a marker paired with the wrong directory.
mkdir -p "$d/nogit/dist" && printf 'x' > "$d/nogit/package.json"
printf 'yy' > "$d/nogit/dist/out"
mkdir -p "$d/nomarker/.git" "$d/nomarker/dist" && printf 'yy' > "$d/nomarker/dist/out"
mkdir -p "$d/crossed/.git" "$d/crossed/target" && printf 'x' > "$d/crossed/package.json"
printf 'yy' > "$d/crossed/target/out"

check "artifacts need -b" 0 \
      "$("$CCLEAN" -n "$d" | grep -c 'out\|dist\|_build\|zig-out')"

check "-b finds every ecosystem and no others" "16 targets, 32 B to reclaim" \
      "$("$CCLEAN" -n -b "$d" | grep 'to reclaim')"

check "-b requires .git" 0 "$("$CCLEAN" -n -b "$d" | grep -c 'nogit')"
check "-b requires a marker" 0 "$("$CCLEAN" -n -b "$d" | grep -c 'nomarker')"
check "-b pairs the marker with its own directory" 0 \
      "$("$CCLEAN" -n -b "$d" | grep -c 'crossed')"

# A submodule carries its own .git and marker file. Its build output is not
# top-level in the project being cleaned, so it must not match. This is the
# layout of sk-engines/lib/DaisySP and friends.
d=$WORK/submodules
rm -rf "$d"
mkdir -p "$d/outer/.git" "$d/outer/build" "$d/outer/lib/vendored/build"
printf 'x' > "$d/outer/CMakeLists.txt"
printf 'AA' > "$d/outer/build/out"
printf 'gitdir: ../../.git/modules/vendored' > "$d/outer/lib/vendored/.git"
printf 'x' > "$d/outer/lib/vendored/CMakeLists.txt"
printf 'BBBB' > "$d/outer/lib/vendored/build/out"

# A vendored checkout with a real .git directory, same rule.
mkdir -p "$d/outer/third_party/lib/.git" "$d/outer/third_party/lib/build"
printf 'x' > "$d/outer/third_party/lib/CMakeLists.txt"
printf 'CCCCCC' > "$d/outer/third_party/lib/build/out"

check "only the top-level build matches" "1 target, 2 B to reclaim" \
      "$("$CCLEAN" -n -b "$d" | grep 'to reclaim')"
check "a submodule build does not match" 0 \
      "$("$CCLEAN" -n -b "$d" | grep -c 'vendored')"
check "a nested checkout build does not match" 0 \
      "$("$CCLEAN" -n -b "$d" | grep -c 'third_party')"

# Naming the nested project as ROOT makes its build top-level again.
check "the nested build matches when named as ROOT" "1 target, 4 B to reclaim" \
      "$("$CCLEAN" -n -b "$d/outer/lib/vendored" | grep 'to reclaim')"

# ROOT inside the repository still cleans that repository's own build.
check "ROOT at the project cleans its build" "1 target, 2 B to reclaim" \
      "$("$CCLEAN" -n -b "$d/outer" | grep 'to reclaim')"

check "--exclude applies to artifacts too" 0 \
      "$("$CCLEAN" -n -b "$d" -e "**/next/**" -e next | grep -c '/next/')"

# ----------------------------------------------------------- output shape

d=$(fixture output)

# Escape sequences must not reach a pipe or a file.
check "no colour when redirected" 0 \
      "$("$CCLEAN" -n "$d" | grep -c "$(printf '\033')")"

# Progress is drawn on standard error, and only for a terminal.
"$CCLEAN" -n "$d" >/dev/null 2>"$WORK/err.txt"
check "no progress when stderr is a file" 0 "$(wc -c < "$WORK/err.txt" | tr -d ' ')"

check "NO_COLOR is honoured" 0 \
      "$(NO_COLOR=1 "$CCLEAN" -n "$d" | grep -c "$(printf '\033')")"

# ------------------------------------------------------ argument handling

d=$WORK/dashes
rm -rf "$d"
mkdir -p "$d"
printf 'x' > "$d/--no-skip"
check "-- ends option parsing" 1 \
      "$("$CCLEAN" -n --no-defaults "$d" -- "--no-skip" | grep -c 'no-skip')"

# Every argument after ROOT is still a pattern, not a path to walk.
d=$(fixture patterns)
check "command line pattern is added to the defaults" 4 \
      "$("$CCLEAN" -n "$d" "*.log" | grep -c '  ')"

# A pattern that cannot be a valid regular expression must not abort. An
# earlier build died with an uncaught regex_error here.
"$CCLEAN" -n --no-defaults "$d" "****a" >/dev/null 2>&1
check "pathological pattern does not abort" 0 $?

# ------------------------------------------------------------------ result

if [ "$failed" -eq 0 ]; then
    echo "cli: $passed checks passed"
    exit 0
fi

echo "cli: $failed of $((passed + failed)) checks FAILED"
exit 1
