#!/usr/bin/env python3
"""Time each tool's dry run and removal, with and without a full walk of directory targets.

Usage: scripts/bench_walk.py

Measures what a check for protected names inside targets (D-15 in docs/dev/spec.md) would
cost. hclean and oclean walk directory targets only for -s, so plain minus -s is the cost
of that walk. reclean and cclean walk every target on every run. Binaries default to
sibling checkouts; override with BENCH_RECLEAN, BENCH_CCLEAN, BENCH_HCLEAN, BENCH_OCLEAN.
Results are in docs/dev/d15-benchmark.md. The page cache is warm; cold runs need root.
"""
import os, shutil, statistics, subprocess, sys, tempfile, time

PROJECTS = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def cabal_bin():
    try:
        return subprocess.run(["cabal", "list-bin", "hclean"], cwd=os.path.join(PROJECTS, "hclean"),
                              capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


BIN = {
    "reclean": os.environ.get("BENCH_RECLEAN") or os.path.join(PROJECTS, "reclean/target/release/reclean"),
    "cclean": os.environ.get("BENCH_CCLEAN") or os.path.join(PROJECTS, "cclean/build/cclean"),
    "hclean": os.environ.get("BENCH_HCLEAN") or cabal_bin(),
    "oclean": os.environ.get("BENCH_OCLEAN") or os.path.join(PROJECTS, "oclean/oclean"),
}
DRY = {"reclean": ["-d"], "cclean": ["-n"], "hclean": ["-d"], "oclean": ["-d"]}
STATS = ("reclean", "hclean", "oclean")  # cclean has no -s
SIZE_ON_STATS = ("hclean", "oclean")  # the tools whose walk -s turns on


def build(root, kind):
    if kind == "node":  # one target: 1000 packages x 5 dirs x 20 files = 100k files
        for p in range(1000):
            for d in range(5):
                q = f"{root}/app/node_modules/pkg{p}/d{d}"
                os.makedirs(q)
                for f in range(20):
                    with open(f"{q}/f{f}.js", "w") as fh:
                        fh.write("x" * 200)
    else:  # 5000 targets of 4 files, beside 20k source files
        for a in range(100):
            for b in range(50):
                q = f"{root}/src/a{a}/b{b}"
                os.makedirs(f"{q}/__pycache__")
                for f in range(4):
                    for name in (f"{q}/m{f}.py", f"{q}/__pycache__/m{f}.pyc"):
                        with open(name, "w") as fh:
                            fh.write("x" * 200)


def cmd(tool, root, pat, extra):
    if tool == "cclean":
        return [BIN[tool], "--no-config"] + extra + ["--", root, pat]
    return [BIN[tool], "-p", root, "-g", pat] + extra


def timeit(c, reps, prepare=None):
    ts = []
    for _ in range(reps):
        if prepare:
            prepare()
        t = time.perf_counter()
        r = subprocess.run(c, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        ts.append(time.perf_counter() - t)
        if r.returncode not in (0, 3):
            sys.exit(f"{c}: exit {r.returncode}: {r.stderr[:300]!r}")
    return statistics.median(ts), min(ts), max(ts)


def bench(base, kind, pat):
    tmpl, work = f"{base}/tmpl-{kind}", f"{base}/work-{kind}"
    build(tmpl, kind)

    def prep():
        shutil.rmtree(work, ignore_errors=True)
        subprocess.run(["cp", "-a", tmpl, work], check=True)

    print(f"\n{kind} ({pat})")
    for t in BIN:
        prep()
        variants = [("dry", DRY[t])] + ([("dry -s", DRY[t] + ["-s"])] if t in STATS else [])
        rows = []
        for name, extra in variants:
            subprocess.run(cmd(t, work, pat, extra), capture_output=True)  # warm the cache
            rows.append((name, *timeit(cmd(t, work, pat, extra), 7)))
        variants = [("-y", ["-y"])] + ([("-y -s", ["-y", "-s"])] if t in SIZE_ON_STATS else [])
        for name, extra in variants:
            rows.append((name, *timeit(cmd(t, work, pat, extra), 5, prep)))
        for name, med, lo, hi in rows:
            print(f"  {t:8} {name:8} median {med * 1000:8.1f} ms  [{lo * 1000:.1f} - {hi * 1000:.1f}]")


def main():
    missing = [t for t, b in BIN.items() if not b or not os.access(b, os.X_OK)]
    if missing:
        sys.exit("no binary for " + ", ".join(missing))
    base = tempfile.mkdtemp(prefix="bench-walk-")
    try:
        bench(base, "node", "**/node_modules")
        bench(base, "pyc", "**/__pycache__")
    finally:
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    main()
