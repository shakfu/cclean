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

# ------------------------------------------------------------- exit codes

"$CCLEAN" --help >/dev/null 2>&1
check "--help exits 0" 0 $?

"$CCLEAN" --bogus >/dev/null 2>&1
check "unknown option exits 2" 2 $?

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
mkdir -p "$d/.git/objects" "$d/.venv/lib/__pycache__" "$d/.ssh" "$d/src/__pycache__"
printf 'a' > "$d/.git/objects/cached.pyc"
printf 'b' > "$d/.venv/lib/__pycache__/m.pyc"
printf 'c' > "$d/.ssh/id.pyc"
printf 'd' > "$d/src/__pycache__/m.pyc"

check "skip list leaves one match" "1 target, 1 B to reclaim" \
      "$("$CCLEAN" -n "$d" | grep 'to reclaim')"

check "protected directories are not entered" 0 \
      "$("$CCLEAN" -n "$d" | grep -c '\.git\|\.ssh\|\.venv')"

check "--no-skip enters them" 3 \
      "$("$CCLEAN" -n --no-skip "$d" | grep -c '\.git\|\.ssh\|\.venv')"

printf 'y' | "$CCLEAN" "$d" >/dev/null 2>&1
check "protected content survives removal" 1 "$(count_files "$d/.git")"

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
