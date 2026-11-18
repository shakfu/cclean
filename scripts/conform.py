#!/usr/bin/env python3
"""Black-box conformance checks for docs/dev/spec.md and its two profiles, run against four sibling tools.

Usage: scripts/conform.py [--json FILE] [TOOL ...]

TOOL is any of reclean, cclean, hclean, oclean (default: all). Binaries default to
sibling checkouts beside this repository; override with CONFORM_RECLEAN, CONFORM_CCLEAN,
CONFORM_HCLEAN, CONFORM_OCLEAN. Each tool runs the core checks and its own profile's.
Results are summarised in docs/dev/conformance.md.
Linux and macOS only; run as a normal user, since root ignores the permission checks.
"""
import json, os, pty, re, select, shutil, signal, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECTS = os.path.dirname(REPO)


def cabal_bin():
    try:
        return subprocess.run(["cabal", "list-bin", "hclean"], cwd=os.path.join(PROJECTS, "hclean"),
                              capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


TOOLS = {
    "reclean": os.environ.get("CONFORM_RECLEAN") or os.path.join(PROJECTS, "reclean/target/debug/reclean"),
    "cclean": os.environ.get("CONFORM_CCLEAN") or os.path.join(PROJECTS, "cclean/build/cclean"),
    "hclean": os.environ.get("CONFORM_HCLEAN") or None,
    "oclean": os.environ.get("CONFORM_OCLEAN") or os.path.join(PROJECTS, "oclean/oclean"),
}


# Each tool meets the core (spec.md) and one profile (spec-modern.md, spec-classic.md).
PROFILE = {"reclean": "modern", "cclean": "modern", "hclean": "classic", "oclean": "classic"}
USAGE_EXIT = {"modern": 2, "classic": 1}
WARN_EXIT = {"modern": 3, "classic": 0}


class NA(Exception):
    pass


# Abstract flag -> spelling per tool. None means the tool lacks it.
FLAGS = {
    "DRY":    {"reclean": ["-d"], "cclean": ["-n"], "hclean": ["-d"], "oclean": ["-d"]},
    "YES":    {t: ["-y"] for t in TOOLS},
    "ART":    {"reclean": ["-B"], "cclean": ["-b"], "hclean": ["-B"], "oclean": ["-B"]},
    "DEP":    {"reclean": ["-D"], "cclean": ["-d"], "hclean": None, "oclean": None},
    "NOPROT": {"reclean": ["--no-protect"], "cclean": ["--no-skip"],
               "hclean": ["--no-protect"], "oclean": ["--no-protect"]},
    "VERB":   {t: ["-v"] for t in TOOLS},
    "JSON":   {t: ["--format", "json"] for t in TOOLS},
    "STATS":  {"reclean": ["-s"], "cclean": None, "hclean": ["-s"], "oclean": ["-s"]},
}
VALUED = {
    "EXCL":   {t: "-e" for t in TOOLS},
    "OLDER":  {t: "--older-than" for t in TOOLS},
    "LARGER": {"reclean": "--larger-than", "cclean": "--larger-than", "hclean": None, "oclean": None},
    "COLOR":  {"reclean": "--color", "cclean": "--color", "hclean": None, "oclean": "--color"},
}
CONFIG_NAME = {t: f".{t}.toml" for t in TOOLS}


def argv(tool, root, pats=None, flags=(), config=False):
    out = []
    for f in flags:
        if isinstance(f, tuple):
            opt = VALUED[f[0]][tool]
            if opt is None:
                raise NA(f"no {f[0]}")
            out += [opt, f[1]]
        else:
            sp = FLAGS[f][tool]
            if sp is None:
                raise NA(f"no {f}")
            out += sp
    if tool == "cclean":
        pre = [] if config else ["--no-config"]
        return [TOOLS[tool]] + pre + out + ["--", root] + list(pats or [])
    pre = ["-c"] if config else []
    g = []
    for p in pats or []:
        g += ["-g", p]
    return [TOOLS[tool]] + pre + ["-p", root] + g + out


def run(tool, root, pats=None, flags=(), config=False, cwd=None, stdin=b"", env=None, raw=None):
    cmd = raw if raw is not None else argv(tool, root, pats, flags, config)
    e = dict(os.environ)
    e.pop("NO_COLOR", None)
    e.update(env or {})
    p = subprocess.run(cmd, cwd=cwd, input=stdin, capture_output=True, env=e, timeout=60)
    return p.returncode, p.stdout, p.stderr


def tree(base, spec):
    """spec: {relpath: content(bytes|str) | None for dir | ('link', target) | ('size', n)}"""
    for rel, v in spec.items():
        p = os.path.join(base, rel)
        if v is None:
            os.makedirs(p, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if isinstance(v, tuple) and v[0] == "link":
            os.symlink(v[1], p)
        elif isinstance(v, tuple) and v[0] == "size":
            with open(p, "wb") as f:
                f.truncate(v[1])
        elif isinstance(v, tuple) and v[0] == "bytes":
            with open(p, "wb") as f:
                f.write(b"x" * v[1])
        else:
            with open(p, "wb") as f:
                f.write(v.encode() if isinstance(v, str) else v)


def fresh():
    return tempfile.mkdtemp(prefix="conform-")


def cleanup(d):
    for r, ds, fs in os.walk(d):
        for x in ds:
            q = os.path.join(r, x)
            if not os.path.islink(q):
                try:
                    os.chmod(q, 0o755)
                except OSError:
                    pass
    os.chmod(d, 0o755)
    shutil.rmtree(d, ignore_errors=True)


def ex(d, rel):
    return os.path.lexists(os.path.join(d, rel))


def jdoc(out):
    return json.loads(out.decode("utf-8"))


def targets(doc):
    return doc.get("matches", doc.get("targets", []))


def git_init(path, separate=None):
    os.makedirs(path, exist_ok=True)
    cmd = ["git", "init", "-q"]
    if separate:
        cmd.append(f"--separate-git-dir={separate}")
    subprocess.run(cmd + [path], check=True)


# ---------------------------------------------------------------- checks
CHECKS = []


def check(rid, profile="core"):
    def deco(fn):
        CHECKS.append((rid, profile, fn))
        return fn
    return deco


def expect(cond, note):
    return ("PASS", "") if cond else ("FAIL", note)


@check("CLI-1", "modern")
def _(t, d):
    tree(d, {"x.pyc": ""})
    missing = []
    for name, fl in [("-v", ["VERB"]), ("-e", [("EXCL", "zz")]), ("--older-than", [("OLDER", "1d")]),
                     ("--larger-than", [("LARGER", "1K")]), ("--color", [("COLOR", "never")])]:
        try:
            c, o, e = run(t, d, ["**/*.pyc"], ["DRY"] + fl)
            if c not in (0, 3):
                missing.append(name)
        except NA:
            missing.append(name)
    for opt in ["-h", "-V"]:
        c, o, e = run(t, d, raw=[TOOLS[t], opt])
        if c != 0 or not o:
            missing.append(opt)
    return expect(not missing, "lacks " + ", ".join(missing))


@check("CLI-2", "modern")
def _(t, d):
    c, o, e = run(t, d, raw=[TOOLS[t], "--bogus-option"])
    return expect(c == 2, f"exit {c}")


@check("ROOT-1")
def _(t, d):
    tree(d, {"__pycache__/x.pyc": ""})
    c, o, e = run(t, os.path.join(d, "__pycache__"), ["**/__pycache__", "**/*.pyc"], ["YES"])
    return expect(ex(d, "__pycache__") and not ex(d, "__pycache__/x.pyc"),
                  f"root exists={ex(d, '__pycache__')}, child removed={not ex(d, '__pycache__/x.pyc')}")


@check("ROOT-2")
def _(t, d):
    tree(d, {".git/x.pyc": "", "venv/x.pyc": ""})
    run(t, os.path.join(d, ".git"), ["**/*.pyc"], ["YES"])
    run(t, os.path.join(d, "venv"), ["**/*.pyc"], ["YES"])
    bad = [p for p in [".git/x.pyc", "venv/x.pyc"] if ex(d, p)]
    return expect(not bad, "not scanned: " + ", ".join(bad))


@check("ROOT-3")
def _(t, d):
    tree(d, {"real/x.pyc": "", "link": ("link", "real")})
    run(t, os.path.join(d, "link"), ["**/*.pyc"], ["YES"])
    return expect(not ex(d, "real/x.pyc") and ex(d, "link"), "symlink root not followed")


@check("ROOT-4")
def _(t, d):
    tree(d, {"broken": ("link", "nowhere")})
    c1, _, _ = run(t, os.path.join(d, "missing"), ["**/*.pyc"], ["DRY"])
    c2, _, _ = run(t, os.path.join(d, "broken"), ["**/*.pyc"], ["DRY"])
    return expect(c1 == 1 and c2 == 1, f"missing exit {c1}, broken-link exit {c2}")


@check("GLOB-1")
def _(t, d):
    tree(d, {"A.PYC": "", "a.pyc": ""})
    run(t, d, ["**/*.pyc"], ["YES"])
    return expect(ex(d, "A.PYC") and not ex(d, "a.pyc"), "case-insensitive or no match")


@check("GLOB-2", "modern")
def _(t, d):
    tree(d, {"a/b/c": ""})
    run(t, d, ["a*c"], ["YES"])
    return expect(not ex(d, "a/b/c"), "`a*c` did not match a/b/c")


@check("GLOB-3")
def _(t, d):
    tree(d, {"x.tmp": "", "p/q/x.tmp": ""})
    run(t, d, ["**/x.tmp"], ["YES"])
    left = [p for p in ["x.tmp", "p/q/x.tmp"] if ex(d, p)]
    return expect(not left, "kept " + ", ".join(left))


@check("GLOB-4")
def _(t, d):
    tree(d, {"a.tmp": "", "ab.tmp": ""})
    run(t, d, ["?.tmp"], ["YES"])
    return expect(not ex(d, "a.tmp") and ex(d, "ab.tmp"), f"a.tmp kept={ex(d, 'a.tmp')}, ab.tmp kept={ex(d, 'ab.tmp')}")


@check("GLOB-5")
def _(t, d):
    tree(d, {".hidden.pyc": ""})
    run(t, d, ["**/*.pyc"], ["YES"])
    return expect(not ex(d, ".hidden.pyc"), "dotfile not matched")


@check("GLOB-6")
def _(t, d):
    tree(d, {"b.tmp": "", "ab.tmp": "", "b.tmpx": ""})
    run(t, d, ["b.tmp"], ["YES"])
    return expect(not ex(d, "b.tmp") and ex(d, "ab.tmp") and ex(d, "b.tmpx"), "not anchored")


@check("DEF-1")
def _(t, d):
    tree(d, {".bash_history": "h", ".python_history": "h", "x.pyc": ""})
    run(t, d, None, ["YES"])
    gone = [p for p in [".bash_history", ".python_history"] if not ex(d, p)]
    return expect(not gone, "removed " + ", ".join(gone))


@check("EXCL-1")
def _(t, d):
    tree(d, {"keep/x.pyc": "", "other/x.pyc": ""})
    run(t, d, ["**/*.pyc"], ["YES", ("EXCL", "keep")])
    return expect(ex(d, "keep/x.pyc") and not ex(d, "other/x.pyc"), "exclude did not prune")


@check("EXCL-2")
def _(t, d):
    tree(d, {"p/keep/x.pyc": "", "p/other/x.pyc": ""})
    run(t, d, ["**/*.pyc"], ["YES", ("EXCL", "**/k?ep")])
    return expect(ex(d, "p/keep/x.pyc") and not ex(d, "p/other/x.pyc"), "glob exclude did not prune")


def artifact_project(d, name="proj"):
    p = os.path.join(d, name)
    git_init(p)
    tree(p, {"CMakeLists.txt": "", "build/o.o": "x"})
    return p


@check("EXCL-3")
def _(t, d):
    artifact_project(d)
    run(t, d, None, ["YES", "ART", ("EXCL", "**/build")])
    return expect(ex(d, "proj/build"), "excluded artifact removed")


PROTECTED = [".git", ".hg", ".svn", ".config", ".ssh", ".gnupg"]


@check("PROT-1")
def _(t, d):
    tree(d, {f"{n}/x.pyc": "" for n in PROTECTED})
    run(t, d, ["**/*.pyc"], ["YES"])
    gone = [n for n in PROTECTED if not ex(d, f"{n}/x.pyc")]
    return expect(not gone, "entered " + ", ".join(gone))


@check("PROT-2")
def _(t, d):
    tree(d, {".hg": "file"})
    run(t, d, ["**/.hg", ".hg"], ["YES"])
    return expect(ex(d, ".hg"), "file named .hg removed")


@check("PROT-3")
def _(t, d):
    tree(d, {".git/HEAD": "x"})
    run(t, d, ["**/.git", ".git"], ["YES"])
    return expect(ex(d, ".git"), ".git removed by a pattern naming it")


@check("PROT-4")
def _(t, d):
    tree(d, {".ssh/x.pyc": ""})
    run(t, d, ["**/*.pyc"], ["YES", "NOPROT"])
    return expect(not ex(d, ".ssh/x.pyc"), "override did not enter .ssh")


@check("MATCH-1")
def _(t, d):
    tree(d, {"a/__pycache__/sub/__pycache__/x": ""})
    c, o, e = run(t, d, ["**/__pycache__"], ["DRY", "JSON"])
    n = len(targets(jdoc(o)))
    return expect(n == 1, f"{n} targets")


@check("WALK-1")
def _(t, d):
    root = os.path.join(d, "root")
    tree(d, {"outside/x.pyc": "", "root/link": ("link", "../outside")})
    run(t, root, ["**/*.pyc"], ["YES"])
    return expect(ex(d, "outside/x.pyc"), "followed symlink out of root")


@check("WALK-3")
def _(t, d):
    tree(d, {"locked/x.pyc": "", "ok/x.pyc": ""})
    os.chmod(os.path.join(d, "locked"), 0)
    c, o, e = run(t, d, ["**/*.pyc"], ["DRY", "JSON"])
    listed = any("ok/x.pyc" in x["path"] for x in targets(jdoc(o))) if o else False
    w = WARN_EXIT[PROFILE[t]]
    return expect(c == w and listed and b"locked" in e, f"exit {c}, continued={listed}, warned={b'locked' in e}")


@check("WALK-4")
def _(t, d):
    tree(d, {"a/f": "", "a.b/f": ""})
    c, o, e = run(t, d, ["a", "a.b"], ["DRY", "JSON"])
    paths = [x["path"] for x in targets(jdoc(o))]
    ia = next((i for i, p in enumerate(paths) if p.rstrip("/").endswith("/a") or p == "a"), None)
    ib = next((i for i, p in enumerate(paths) if p.rstrip("/").endswith("a.b")), None)
    return expect(ia is not None and ib is not None and ia < ib, f"order {paths}")


ART_TABLE = [
    ("build", m) for m in ["CMakeLists.txt", "meson.build", "package.json", "build.gradle",
                           "build.gradle.kts", "pyproject.toml", "setup.py", "pubspec.yaml"]
] + [("target", "Cargo.toml"), ("target", "pom.xml")] + [
    ("dist", m) for m in ["package.json", "pyproject.toml", "setup.py"]
] + [(n, "package.json") for n in [".next", ".nuxt", ".svelte-kit", ".turbo", ".parcel-cache"]] + [
    (".gradle", "build.gradle"), (".gradle", "build.gradle.kts"),
    ("zig-out", "build.zig"), ("zig-cache", "build.zig"), (".zig-cache", "build.zig"),
    (".build", "Package.swift"), ("_build", "mix.exs"),
]


@check("ART-1")
def _(t, d):
    miss = []
    for i, (name, marker) in enumerate(ART_TABLE):
        p = os.path.join(d, f"p{i}")
        git_init(p)
        tree(p, {marker: "", f"{name}/o": "x"})
        run(t, p, ["zz-none"], ["YES", "ART"])
        if ex(p, name):
            miss.append(f"{name}+{marker}")
    return expect(not miss, f"{len(miss)}/{len(ART_TABLE)} pairs not matched: " + ", ".join(miss))


@check("ART-2")
def _(t, d):
    p = os.path.join(d, "proj")
    git_init(p)
    tree(p, {"CMakeLists.txt": "", "real/o": "x", "build": ("link", "real")})
    run(t, d, ["zz-none"], ["YES", "ART"])
    return expect(ex(d, "proj/real/o") and ex(d, "proj/build"), "symlinked build/ treated as artifact")


@check("ART-3")
def _(t, d):
    p = os.path.join(d, "proj")
    git_init(p)
    tree(p, {"package.json": "", "target/o": "x"})
    run(t, d, ["zz-none"], ["YES", "ART"])
    return expect(ex(d, "proj/target"), "package.json qualified target/")


@check("ART-4")
def _(t, d):
    tree(d, {"nogit/CMakeLists.txt": "", "nogit/build/o": "x"})
    git_init(os.path.join(d, "sub"), separate=os.path.join(d, "sub.gitdir"))
    tree(d, {"sub/CMakeLists.txt": "", "sub/build/o": "x"})
    run(t, d, ["zz-none"], ["YES", "ART"])
    return expect(ex(d, "nogit/build") and not ex(d, "sub/build"),
                  f"no-.git kept={ex(d, 'nogit/build')}, .git-file removed={not ex(d, 'sub/build')}")


@check("ART-5", "modern")
def _(t, d):
    outer = os.path.join(d, "outer")
    git_init(outer)
    tree(outer, {"CMakeLists.txt": "", "build/o": "x"})
    inner = os.path.join(outer, "inner")
    git_init(inner)
    tree(inner, {"CMakeLists.txt": "", "build/o": "x"})
    run(t, outer, ["zz-none"], ["YES", "ART"])
    a = not ex(outer, "build")       # project == root: removed
    b = ex(inner, "build")           # nested repo: kept
    tree(inner, {"build/o": "x"})
    run(t, inner, ["zz-none"], ["YES", "ART"])
    c = not ex(inner, "build")       # nested repo as root: removed
    return expect(a and b and c, f"root-project removed={a}, nested kept={b}, nested-as-root removed={c}")


@check("ART-6")
def _(t, d):
    p = artifact_project(d)
    tree(p, {"build/x.pyc": ""})
    c, o, e = run(t, d, ["**/*.pyc"], ["DRY", "ART", "JSON"])
    paths = [x["path"] for x in targets(jdoc(o))]
    return expect(not any(x.endswith("build/x.pyc") for x in paths), f"listed {paths}")


DEP_TABLE = [(".venv", "uv.lock", "uv sync"), ("node_modules", "package-lock.json", "npm ci"),
             ("node_modules", "npm-shrinkwrap.json", "npm ci"),
             ("node_modules", "yarn.lock", "yarn install --immutable"),
             ("node_modules", "pnpm-lock.yaml", "pnpm install --frozen-lockfile"),
             ("node_modules", "bun.lock", "bun install --frozen-lockfile"),
             ("node_modules", "bun.lockb", "bun install --frozen-lockfile"),
             ("vendor", "go.mod", "go mod vendor")]


@check("DEP-1", "modern")
def _(t, d):
    miss = []
    for i, (name, lock, _r) in enumerate(DEP_TABLE):
        p = os.path.join(d, f"p{i}")
        tree(p, {lock: "", f"{name}/o": "x"})
        run(t, p, ["zz-none"], ["YES", "DEP"])
        if ex(p, name):
            miss.append(f"{name}+{lock}")
    tree(d, {"both/yarn.lock": "", "both/pnpm-lock.yaml": "", "both/node_modules/o": "x"})
    c, o, e = run(t, os.path.join(d, "both"), ["zz-none"], ["DRY", "DEP"])
    order = b"yarn install --immutable" in o and b"pnpm" not in o
    return expect(not miss and order, f"unmatched {miss}; first-row-wins={order}")


@check("DEP-2", "modern")
def _(t, d):
    tree(d, {"real.lock": "", "package-lock.json": ("link", "real.lock"), "node_modules/o": "x"})
    run(t, d, ["zz-none"], ["YES", "DEP"])
    return expect(ex(d, "node_modules"), "symlinked lock file counted")


@check("DEP-3", "modern")
def _(t, d):
    tree(d, {"p/go.mod": "", "p/vendor/o": "x"})
    run(t, d, ["zz-none"], ["YES", "DEP"])
    return expect(not ex(d, "p/vendor"), "needed .git")


@check("DEP-4", "modern")
def _(t, d):
    tree(d, {"package-lock.json": "", "node_modules/o": "x"})
    c, o, e = run(t, d, ["zz-none"], ["DRY", "DEP"])
    c2, o2, e2 = run(t, d, ["zz-none"], ["DRY", "DEP", "JSON"])
    js = [x.get("restore") for x in targets(jdoc(o2))]
    return expect(b"restore: npm ci" in o and "npm ci" in js, f"text={b'restore: npm ci' in o}, json={js}")


@check("DEP-5", "modern")
def _(t, d):
    tree(d, {"package-lock.json": "", "node_modules/x.pyc": ""})
    c, o, e = run(t, d, ["**/*.pyc"], ["DRY", "DEP", "JSON"])
    paths = [x["path"] for x in targets(jdoc(o))]
    return expect(len(paths) == 1, f"listed {paths}")


DAY = 86400


def age(d, rel, days):
    p = os.path.join(d, rel)
    t0 = time.time() - days * DAY
    os.utime(p, (t0, t0), follow_symlinks=False)


@check("FILT-2", "modern")
def _(t, d):
    tree(d, {"old_small.tmp": ("bytes", 10), "old_big.tmp": ("bytes", 4096), "new_big.tmp": ("bytes", 4096)})
    age(d, "old_small.tmp", 10); age(d, "old_big.tmp", 10)
    run(t, d, ["**/*.tmp"], ["YES", ("OLDER", "1d"), ("LARGER", "1K")])
    ok = ex(d, "old_small.tmp") and not ex(d, "old_big.tmp") and ex(d, "new_big.tmp")
    return expect(ok, "filters not ANDed")


@check("FILT-3")
def _(t, d):
    tree(d, {"old.tmp": "", "fresh_inside.tmp/f": "", "old_inside.tmp/f": ""})
    age(d, "old.tmp", 10)
    age(d, "fresh_inside.tmp", 10)
    age(d, "old_inside.tmp/f", 10); age(d, "old_inside.tmp", 10)
    run(t, d, ["**/*.tmp"], ["YES", ("OLDER", "1d")])
    ok = not ex(d, "old.tmp") and ex(d, "fresh_inside.tmp") and not ex(d, "old_inside.tmp")
    return expect(ok, f"old file removed={not ex(d, 'old.tmp')}, dir with fresh file kept={ex(d, 'fresh_inside.tmp')}, old dir removed={not ex(d, 'old_inside.tmp')}")


@check("FILT-4")
def _(t, d):
    tree(d, {"future.tmp": ""})
    age(d, "future.tmp", -10)
    run(t, d, ["**/*.tmp"], ["YES", ("OLDER", "1s")])
    return expect(ex(d, "future.tmp"), "future mtime treated as old")


@check("FILT-5")
def _(t, d):
    tree(d, {"x.tmp": ""})
    bad = []
    for u in ["1s", "1m", "1h", "1d", "1w"]:
        c, _, _ = run(t, d, ["**/*.tmp"], ["DRY", ("OLDER", u)])
        if c not in (0, 3):
            bad.append(f"{u} rejected")
    c, _, _ = run(t, d, ["**/*.tmp"], ["DRY", ("OLDER", "1D")])
    if c != USAGE_EXIT[PROFILE[t]]:
        bad.append(f"1D exit {c}")
    return expect(not bad, ", ".join(bad))


@check("FILT-6", "modern")
def _(t, d):
    tree(d, {"eq.tmp": ("bytes", 1024), "lt.tmp": ("bytes", 1023)})
    run(t, d, ["**/*.tmp"], ["YES", ("LARGER", "1K")])
    return expect(not ex(d, "eq.tmp") and ex(d, "lt.tmp"), "not inclusive at 1024")


@check("FILT-7", "modern")
def _(t, d):
    tree(d, {"x.tmp": ""})
    bad = []
    for s in ["4096", "1B", "1K", "1KiB", "1M", "1MiB", "1G", "1GiB", "1T", "1TiB", "1.5K"]:
        c, _, _ = run(t, d, ["**/*.tmp"], ["DRY", ("LARGER", s)])
        if c not in (0, 3):
            bad.append(f"{s} rejected")
    for s in ["1KB", "1k"]:
        c, _, _ = run(t, d, ["**/*.tmp"], ["DRY", ("LARGER", s)])
        if c != 2:
            bad.append(f"{s} exit {c}")
    tree(d, {"a.tmp": ("bytes", 1536), "b.tmp": ("bytes", 1535)})
    run(t, d, ["a.tmp", "b.tmp", "**/a.tmp", "**/b.tmp"], ["YES", ("LARGER", "1.5K")])
    if ex(d, "a.tmp") or not ex(d, "b.tmp"):
        bad.append("1.5K != 1536")
    return expect(not bad, ", ".join(bad))


@check("SIZE-1")
def _(t, d):
    tree(d, {"s.tmp": ("size", 1 << 20)})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", "JSON"])
    x = targets(jdoc(o))[0]
    n = x.get("size", x.get("bytes"))
    return expect(n == 1 << 20, f"size {n}")


@check("SIZE-2")
def _(t, d):
    tree(d, {"d.tmp/a": ("bytes", 1000), "d.tmp/sub/b": ("bytes", 24)})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", "JSON"])
    x = targets(jdoc(o))[0]
    n = x.get("size", x.get("bytes"))
    return expect(n == 1024, f"size {n}")


@check("SIZE-3")
def _(t, d):
    tree(d, {"d.tmp/a": ("bytes", 1000)})
    os.link(os.path.join(d, "d.tmp/a"), os.path.join(d, "d.tmp/b"))
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", "JSON"])
    x = targets(jdoc(o))[0]
    n = x.get("size", x.get("bytes"))
    return expect(n == 2000, f"size {n}")


@check("SIZE-4")
def _(t, d):
    tree(d, {"a.tmp": ("bytes", 1023), "b.tmp": ("size", 1048575), "c.tmp": ("bytes", 2000)})
    pats = ["**/a.tmp", "**/b.tmp", "**/c.tmp"]
    fl = ["DRY"] + (["STATS"] if FLAGS["STATS"][t] else [])
    c, o, e = run(t, d, pats, fl)
    c2, o2, e2 = run(t, d, pats, fl + ["JSON"])
    blob = o + o2
    want = [b"1023 B", b"1024.00 KiB", b"1.95 KiB"]
    miss = [w.decode() for w in want if w not in blob]
    return expect(not miss, "missing " + ", ".join(miss) + "; got " + ", ".join(sorted(set(re.findall(r"\d+(?:\.\d+)? [KMGT]?i?B", blob.decode(errors='replace'))))))


@check("ASK-1")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], [], stdin=b"")
    kept = ex(d, "x.tmp")
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"], stdin=b"y")
    dry_prompt = b"[y" in (o + e)
    c, o, e = run(t, d, ["**/none"], [], stdin=b"y")
    empty_prompt = b"[y" in (o + e)
    return expect(kept and not dry_prompt and not empty_prompt,
                  f"EOF kept={kept}, dry-run prompt={dry_prompt}, empty prompt={empty_prompt}")


@check("ASK-3", "modern")
def _(t, d):
    res = {}
    for inp, want_removed in [(b"y", True), (b"Y", True), (b"yes", True), (b"x", False), (b"", False),
                              (b" y", False), (b"n", False)]:
        tree(d, {"x.tmp": ""})
        run(t, d, ["**/*.tmp"], [], stdin=inp)
        removed = not ex(d, "x.tmp")
        if removed != want_removed:
            res[inp.decode() or "EOF"] = "removed" if removed else "kept"
    return expect(not res, "wrong: " + ", ".join(f"{k!r}->{v}" for k, v in res.items()))


@check("ASK-4")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], [], stdin=b"n")
    return expect(c == 0 and ex(d, "x.tmp"), f"exit {c}")


@check("ASK-5")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["JSON"], stdin=b"n")
    try:
        jdoc(o)
        valid = True
    except Exception:
        valid = False
    return expect(valid and b"[y" in e and b"[y" not in o, f"json valid={valid}, prompt on stderr={b'[y' in e}")


def pty_run(cmd, cwd, script, timeout=10):
    """script: list of (wait_for_bytes_or_None, send_bytes). Returns (exit, output)."""
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        os.environ.pop("NO_COLOR", None)
        os.execv(cmd[0], cmd)
    buf = b""
    end = time.time() + timeout
    steps = list(script)
    while time.time() < end:
        if steps and (steps[0][0] is None or steps[0][0] in buf):
            os.write(fd, steps.pop(0)[1])
            continue
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
        pid2, status = os.waitpid(pid, os.WNOHANG)
        if pid2:
            return os.waitstatus_to_exitcode(status), buf
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    _, status = os.waitpid(pid, 0)
    return None, buf


@check("ASK-2", "modern")
def _(t, d):
    tree(d, {"x.tmp": ""})
    cmd = argv(t, d, ["**/*.tmp"], [])
    code, out = pty_run(cmd, d, [(b"[y", b"y")], timeout=5)
    single = not ex(d, "x.tmp")
    tree(d, {"big/" + f"f{i}": "" for i in range(3000)})
    tree(d, {"x.tmp": ""})
    code2, out2 = pty_run(cmd, d, [(None, b"y\n"), (b"[y", b"n")], timeout=8)
    flushed = ex(d, "x.tmp")
    return expect(single and flushed, f"single keypress removed={single}, typeahead discarded={flushed} (timing-dependent)")


@check("RM-3")
def _(t, d):
    tree(d, {"outside/keep": "", "t.tmp/link": ("link", "../outside")})
    run(t, d, ["**/*.tmp"], ["YES"])
    return expect(not ex(d, "t.tmp") and ex(d, "outside/keep"), f"target gone={not ex(d, 't.tmp')}, outside intact={ex(d, 'outside/keep')}")


@check("RM-4", "modern")
def _(t, d):
    tree(d, {"t.tmp/ro/f": ""})
    os.chmod(os.path.join(d, "t.tmp/ro"), 0o555)
    c, o, e = run(t, d, ["**/*.tmp"], ["YES"])
    return expect(ex(d, "t.tmp") and c == 1, f"target removed={not ex(d, 't.tmp')}, exit {c}")


@check("RM-5")
def _(t, d):
    # A file in a read-only parent: no tool changes a target's parent, so this fails everywhere.
    tree(d, {"ro/a.tmp": "", "b.tmp": ""})
    os.chmod(os.path.join(d, "ro"), 0o555)
    c, o, e = run(t, d, ["**/*.tmp"], ["YES"])
    return expect(ex(d, "ro/a.tmp") and not ex(d, "b.tmp") and c == 1,
                  f"failed kept={ex(d, 'ro/a.tmp')}, other removed={not ex(d, 'b.tmp')}, exit {c}")


@check("OUT-1")
def _(t, d):
    tree(d, {"zq.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"])
    return expect(b"zq.tmp" in o and b"zq.tmp" not in e, f"stdout={b'zq.tmp' in o}, stderr={b'zq.tmp' in e}")


@check("OUT-2")
def _(t, d):
    tree(d, {"locked/x": "", "x.tmp": ""})
    os.chmod(os.path.join(d, "locked"), 0)
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"])
    return expect(b"locked" in e and b"locked" not in o, f"stderr={b'locked' in e}, stdout={b'locked' in o}")


@check("OUT-3")
def _(t, d):
    tree(d, {"locked/x": "", "x.tmp": ""})
    os.chmod(os.path.join(d, "locked"), 0)
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", "JSON"])
    try:
        jdoc(o); valid = True
    except Exception:
        valid = False
    return expect(valid and b"locked" in e, f"json valid={valid}, warning on stderr={b'locked' in e}")


def lines(o):
    return o.decode(errors="replace").split("\n")


@check("TXT-1", "modern")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"])
    L = lines(o)
    ok = "Matched targets:" in L and L.index("Matched targets:") > 0 and L[L.index("Matched targets:") - 1] == ""
    return expect(ok, "first stdout line: " + repr(L[0][:60]))


@check("TXT-2", "modern")
def _(t, d):
    tree(d, {"__pycache__/f": ("bytes", 12), "f.pyc": "", "real/": None, "lnk.pyc": ("link", "real")})
    c, o, e = run(t, d, ["**/__pycache__", "**/*.pyc"], ["DRY"])
    L = lines(o)
    dir_ok = any(re.fullmatch(r"  (\S*/)?__pycache__/  12 B", x) for x in L)
    file_ok = any(re.fullmatch(r"  \S*f\.pyc  0 B", x) for x in L)
    return expect(dir_ok and file_ok, f"dir line ok={dir_ok}, file line ok={file_ok}")


@check("TXT-3", "modern")
def _(t, d):
    tree(d, {"package-lock.json": "", "node_modules/o": "x"})
    c, o, e = run(t, d, ["zz-none"], ["DRY", "DEP"])
    ok = any(re.fullmatch(r"  (\S*/)?node_modules/  \S+ \S+  restore: npm ci", x) for x in lines(o))
    return expect(ok, "no `  restore: npm ci` suffix")


@check("TXT-4", "modern")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"])
    one = any(re.fullmatch(r"1 target, .* to reclaim", x) for x in lines(o))
    tree(d, {"y.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"])
    two = any(re.fullmatch(r"2 targets, .* to reclaim", x) for x in lines(o))
    return expect(one and two, f"singular={one}, plural={two}")


@check("TXT-5", "modern")
def _(t, d):
    p = artifact_project(d)
    tree(d, {"x.pyc": ""})
    c, o, e = run(t, d, ["**/*.pyc"], ["DRY", "ART"])
    two = any(re.match(r"\s+build-artifact\s+\d", x) for x in lines(o))
    c, o, e = run(t, d, ["**/*.pyc"], ["DRY"])
    one = any(re.match(r"\s+(pattern|default|command-line|config)\s+\d", x) for x in lines(o))
    return expect(two and not one, f"breakdown with 2 reasons={two}, with 1 reason={one}")


@check("TXT-6", "modern")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["YES", "VERB"])
    ok = any(re.fullmatch(r"  removed  \S*x\.tmp", x) for x in lines(o))
    return expect(ok, "no `  removed  <path>` line on stdout")


@check("COL-1")
def _(t, d):
    tree(d, {"x.tmp/f": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"])
    return expect(b"\x1b" not in o + e, "colour on a pipe")


@check("COL-2")
def _(t, d):
    tree(d, {"x.tmp/f": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", ("COLOR", "always")], env={"NO_COLOR": "1"})
    c2, o2, e2 = run(t, d, ["**/*.tmp"], ["DRY", ("COLOR", "never")])
    return expect(b"\x1b" in o and b"\x1b" not in o2 + e2, f"always+NO_COLOR coloured={b'\x1b' in o}")


@check("COL-5", "modern")
def _(t, d):
    tree(d, {"x.tmp/f": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", ("COLOR", "always")])
    return expect(re.search(rb"\x1b\[36m[^\n]*x\.tmp", o) is not None, "directory not cyan")


@check("COL-6")
def _(t, d):
    tree(d, {"x.tmp/f": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", "JSON", ("COLOR", "always")])
    return expect(b"\x1b" not in o, "JSON coloured")


@check("PROG-1")
def _(t, d):
    tree(d, {f"d/f{i}": "" for i in range(2000)})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"])
    return expect(b"scanning" not in e and b"\r" not in e, "progress on a pipe")


@check("NAME-1")
def _(t, d):
    tree(d, {"nl\nzz.pyc": "", "esc\x1b[31mq.pyc": "", "we\\ird.pyc": ""})
    try:
        c, o, e = run(t, d, ["**/*.pyc"], ["DRY", ("COLOR", "never")])
    except NA:
        c, o, e = run(t, d, ["**/*.pyc"], ["DRY"])
    L = lines(o)
    forged = any(x.startswith("zz.pyc") for x in L)
    esc = b"\x1b" in o
    bs = b"we\\\\ird" in o
    return expect(not forged and not esc and bs, f"newline forged a line={forged}, raw ESC={esc}, backslash escaped={bs}")


@check("NAME-2")
def _(t, d):
    with open(os.path.join(d.encode(), b"bad\xff.pyc"), "wb"):
        pass
    c, o, e = run(t, d, ["**/*.pyc"], ["DRY", "JSON"])
    try:
        doc = jdoc(o)
        ok = any("�" in x["path"] for x in targets(doc))
    except Exception as ex_:
        return ("FAIL", f"invalid JSON ({type(ex_).__name__})")
    return expect(ok, "no U+FFFD in path")


@check("JSON-1")
def _(t, d):
    tree(d, {"x.tmp": ""})
    bad = []
    for label, pats, fl, inp in [("dry run", ["**/*.tmp"], ["DRY", "JSON"], b""),
                                 ("declined", ["**/*.tmp"], ["JSON"], b"n"),
                                 ("empty", ["**/none"], ["JSON"], b"")]:
        c, o, e = run(t, d, pats, fl, stdin=inp)
        try:
            jdoc(o)
        except Exception:
            bad.append(label)
    return expect(not bad, "no document for " + ", ".join(bad))


@check("JSON-2", "modern")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", "JSON"])
    doc = jdoc(o)
    return expect(doc.get("schema") == 1 and "config" in doc and doc["config"] is None,
                  f"schema={doc.get('schema')}, config present={'config' in doc}")


@check("JSON-3", "modern")
def _(t, d):
    tree(d, {"f.tmp": "", "d.tmp/f": "", "l.tmp": ("link", "f.tmp")})
    fl = ["DRY", "JSON"]
    try:
        tree(d, {"package-lock.json": "", "node_modules/o": "x"})
        c, o, e = run(t, d, ["**/*.tmp"], fl + ["DEP"])
    except NA:
        c, o, e = run(t, d, ["**/*.tmp"], fl)
    probs = set()
    for x in targets(jdoc(o)):
        if "path" not in x: probs.add("path")
        if not isinstance(x.get("size", x.get("bytes")), int): probs.add("size")
        if x.get("type") not in ("file", "directory", "symlink"): probs.add("type")
        if "reason" not in x: probs.add("reason")
        if ("restore" in x) != (x.get("reason") == "dependency"): probs.add("restore")
    return expect(not probs, "missing or wrong: " + ", ".join(sorted(probs)))


@check("EXIT-3", "modern")
def _(t, d):
    tree(d, {"x.tmp": "", "locked/x": ""})
    c0, _, _ = run(t, os.path.join(d, "x.tmp") + "-nope", ["**/*.tmp"], ["DRY"])
    c2, _, _ = run(t, d, raw=[TOOLS[t], "--bogus-option"])
    os.chmod(os.path.join(d, "locked"), 0)
    c3, _, _ = run(t, d, ["**/*.tmp"], ["DRY"])
    os.chmod(os.path.join(d, "locked"), 0o755)
    shutil.rmtree(os.path.join(d, "locked"))
    cok, _, _ = run(t, d, ["**/*.tmp"], ["DRY"])
    got = {"0": cok, "1": c0, "2": c2, "3": c3}
    return expect(got == {"0": 0, "1": 1, "2": 2, "3": 3}, "got " + ", ".join(f"want {k}->{v}" for k, v in got.items()))


@check("EXIT-2")
def _(t, d):
    tree(d, {"ro/a.tmp": "", "locked/x": ""})
    os.chmod(os.path.join(d, "ro"), 0o555)
    os.chmod(os.path.join(d, "locked"), 0)
    c, o, e = run(t, d, ["**/*.tmp"], ["YES"])
    return expect(c == 1, f"exit {c}")


def write_cfg(t, path, pats, extra=""):
    arr = ", ".join(f'"{p}"' for p in pats)
    with open(path, "w") as f:
        f.write(f"patterns = [{arr}]\n{extra}")


@check("CFG-1")
def _(t, d):
    tree(d, {"x.tmp": ""})
    write_cfg(t, os.path.join(d, CONFIG_NAME[t]), ["**/*.tmp"])
    c, o, e = run(t, ".", None, ["YES"], config=True, cwd=d)
    return expect(not ex(d, "x.tmp"), f"config not applied (exit {c}: {e.decode(errors='replace').strip()[:80]})")


@check("CFG-2")
def _(t, d):
    tree(d, {"inner/x.a": "", "inner/x.b": ""})
    write_cfg(t, os.path.join(d, CONFIG_NAME[t]), ["**/*.b"])
    write_cfg(t, os.path.join(d, "inner", CONFIG_NAME[t]), ["**/*.a"])
    inner = os.path.join(d, "inner")
    c, o, e = run(t, ".", None, ["YES"], config=True, cwd=inner)
    return expect(not ex(inner, "x.a") and ex(inner, "x.b"), f"x.a removed={not ex(inner, 'x.a')}, x.b kept={ex(inner, 'x.b')}")


@check("CFG-3", "modern")
def _(t, d):
    tree(d, {"x.tmp": ""})
    write_cfg(t, os.path.join(d, CONFIG_NAME[t]), ["**/*.tmp"], "bogus_key = true\n")
    c, o, e = run(t, ".", None, ["DRY"], config=True, cwd=d)
    return expect(c == 2, f"unknown key: exit {c}")


@check("CFG-4")
def _(t, d):
    tree(d, {"x.tmp": ""})
    if t == "cclean":
        write_cfg(t, os.path.join(d, CONFIG_NAME[t]), ["**/*.tmp"], 'older_than = "9999w"\n')
        c, o, e = run(t, ".", None, ["YES", ("OLDER", "0s")], config=True, cwd=d)
    else:
        write_cfg(t, os.path.join(d, CONFIG_NAME[t]), ["**/*.tmp"], 'path = "does-not-exist"\n')
        c, o, e = run(t, d, None, ["YES"], config=True, cwd=d)
    return expect(not ex(d, "x.tmp"), f"CLI did not override (exit {c})")


@check("EXIT-1")
def _(t, d):
    tree(d, {"x.tmp": "", "ro/a.tmp": ""})
    c_ok, _, _ = run(t, d, ["**/x.tmp"], ["DRY"])
    c_root, _, _ = run(t, os.path.join(d, "missing"), ["**/*.tmp"], ["DRY"])
    c_usage, _, _ = run(t, d, raw=[TOOLS[t], "--bogus-option"])
    os.chmod(os.path.join(d, "ro"), 0o555)
    c_fail, _, _ = run(t, d, ["**/*.tmp"], ["YES"])
    got = (c_ok, c_root, c_usage != 0, c_fail)
    return expect(got == (0, 1, True, 1), f"ok {c_ok}, bad root {c_root}, usage {c_usage}, failed removal {c_fail}")


def swap_at_prompt(t, d, pats, swap, timeout=10):
    """Start a run, wait for its prompt, call swap(), then answer yes. Returns (exit, stdout, stderr)."""
    p = subprocess.Popen(argv(t, d, pats, []), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    bufs = {p.stdout.fileno(): b"", p.stderr.fileno(): b""}
    end = time.time() + timeout
    while time.time() < end and not any(b"[y" in b for b in bufs.values()):
        r, _, _ = select.select(list(bufs), [], [], 0.1)
        for fd in r:
            bufs[fd] += os.read(fd, 4096)
        if p.poll() is not None:
            break
    if not any(b"[y" in b for b in bufs.values()):
        p.kill()
        p.wait()
        raise RuntimeError("no prompt")
    swap()
    fo, fe = p.stdout.fileno(), p.stderr.fileno()
    o, e = p.communicate(b"y\n", timeout=timeout)
    return p.returncode, bufs[fo] + o, bufs[fe] + e


@check("RM-1")
def _(t, d):
    root = os.path.join(d, "root")
    out = os.path.join(d, "outside")
    tree(d, {"root/a/__pycache__/x": "", "outside/__pycache__/keep": ""})

    def swap():
        os.rename(os.path.join(root, "a"), os.path.join(root, "a.moved"))
        os.symlink(out, os.path.join(root, "a"))
    c, o, e = swap_at_prompt(t, root, ["**/__pycache__"], swap)
    kept = ex(out, "__pycache__/keep")
    return expect(kept and c == 1, f"outside kept={kept}, exit {c}")


@check("RM-2")
def _(t, d):
    tree(d, {"t.tmp/f": ""})

    def swap():
        os.rename(os.path.join(d, "t.tmp"), os.path.join(d, "old"))
        tree(d, {"t.tmp/precious": ""})
    c, o, e = swap_at_prompt(t, d, ["**/*.tmp"], swap)
    kept = ex(d, "t.tmp/precious")
    return expect(kept and c == 1, f"replacement kept={kept}, exit {c}")


# ---------------------------------------------------------------- classic profile
CLASSIC_OPTS = [
    (["-p", "."], ["--path", "."]), (["-g", "**/*.tmp"], ["--glob", "**/*.tmp"]),
    (["-e", "zz"], ["--exclude", "zz"]), (["--preset", "python"],), (["-d"], ["--dry-run"]),
    (["-y"], ["--skip-confirmation"]), (["-s"], ["--stats"]), (["-o", "1d"], ["--older-than", "1d"]),
    (["-B"], ["--build-artifacts"]), (["-i"], ["--include-symlinks"]), (["-r"], ["--remove-broken-symlinks"]),
    (["-c"], ["--configfile"]), (["--format", "text"], ["--format", "json"]), (["--no-protect"],),
    (["-q"], ["--quiet"]), (["-v"], ["--verbose"]), (["-P"], ["--progress"]), (["--no-progress"],),
]


@check("CLI-C1", "classic")
def _(t, d):
    tree(d, {"x.tmp": ""})
    bad = []
    for forms in CLASSIC_OPTS:
        for f in forms:
            c, o, e = run(t, d, raw=[TOOLS[t], "-d"] + f, cwd=d)
            if c != 0:
                bad.append(" ".join(f))
    for f in (["-l"], ["--list"], ["-h"], ["--help"], ["--version"]):
        c, o, e = run(t, d, raw=[TOOLS[t]] + f, cwd=d)
        if c != 0 or not o:
            bad.append(f[0])
    for f in (["-w"], ["--write-configfile"]):
        w = os.path.join(d, "w" + f[0])
        os.makedirs(w)
        c, o, e = run(t, d, raw=[TOOLS[t], "-p", w] + f)
        if c != 0 or not ex(w, CONFIG_NAME[t]):
            bad.append(f[0])
    return expect(not bad, "rejected " + ", ".join(bad))


@check("CLI-C2", "classic")
def _(t, d):
    c, o, e = run(t, d, raw=[TOOLS[t], "--bogus-option"])
    return expect(c == 1 and e.startswith(f"{t}: ".encode()), f"exit {c}, stderr {e[:40]!r}")


@check("GLOB-C1", "classic")
def _(t, d):
    bad = []
    for pat, rel, want in [("a*c", "a/b/c", False), ("a/*/c", "a/b/c", True), ("x.log", "p/q/x.log", True),
                           ("p/*.log", "p/q/x.log", False), ("**/q/*.log", "p/q/x.log", True),
                           ("p/**/x.log", "p/x.log", True), ("[!a].tmp", "b.tmp", True),
                           ("[!a].tmp", "a.tmp", False), ("[a-c].tmp", "b.tmp", True), ("?.tmp", "a/b", False)]:
        sub = os.path.join(d, f"g{len(bad)}{abs(hash((pat, rel)))}")
        tree(sub, {rel: ""})
        run(t, sub, [pat], ["YES"])
        if (not ex(sub, rel)) != want:
            bad.append(f"{pat} vs {rel}")
    c, o, e = run(t, d, ["[ab"], ["DRY"])
    if c != 1:
        bad.append(f"unclosed [ exit {c}")
    return expect(not bad, "wrong: " + ", ".join(bad))


@check("ART-C1", "classic")
def _(t, d):
    ws = os.path.join(d, "ws")
    git_init(ws)
    tree(ws, {"Cargo.toml": "", "crates/foo/Cargo.toml": "", "crates/foo/target/o": "x"})
    inner = os.path.join(ws, "inner")
    git_init(inner)
    tree(inner, {"CMakeLists.txt": "", "build/o": "x"})
    home = os.path.join(d, "home")
    git_init(home)
    tree(home, {"proj/CMakeLists.txt": "", "proj/build/o": "x"})
    run(t, ws, ["zz-none"], ["YES", "ART"])
    run(t, home, ["zz-none"], ["YES", "ART"], env={"HOME": home})
    member = not ex(ws, "crates/foo/target")
    nested = not ex(inner, "build")
    home_stop = ex(home, "proj/build")
    return expect(member and nested and home_stop,
                  f"member removed={member}, nested removed={nested}, ~/.git ignored={home_stop}")


@check("ASK-C1", "classic")
def _(t, d):
    bad = []
    for inp, want in [(b"y\n", True), (b"Y\n", True), (b"yes\n", True), (b"  Yes \n", True),
                      (b"n\n", False), (b"x\n", False), (b"ye\n", False), (b"", False)]:
        tree(d, {"x.tmp": ""})
        c, o, e = run(t, d, ["**/*.tmp"], [], stdin=inp)
        if (not ex(d, "x.tmp")) != want:
            bad.append(repr(inp))
        if b"Delete 1 item(s)? [y/N] " not in e:
            bad.append(f"prompt {e[-40:]!r}")
    return expect(not bad, "wrong: " + ", ".join(bad))


@check("RM-C1", "classic")
def _(t, d):
    tree(d, {"t.tmp/ro/f": ""})
    os.chmod(os.path.join(d, "t.tmp/ro"), 0o555)
    c, o, e = run(t, d, ["**/*.tmp"], ["YES"])
    return expect(not ex(d, "t.tmp") and c == 0, f"target removed={not ex(d, 't.tmp')}, exit {c}")


@check("TXT-C1", "classic")
def _(t, d):
    tree(d, {"b.tmp": "", "a.tmp/f": ""})
    c, o, e = run(t, ".", ["**/*.tmp"], ["DRY"], cwd=d)
    base = os.path.normpath(os.path.abspath(d))
    want = f"Matched: {base}/a.tmp\nMatched: {base}/b.tmp\n".encode()
    return expect(o == want, f"stdout {o[:120]!r}")


@check("TXT-C2", "classic")
def _(t, d):
    tree(d, {"a.tmp": "xy", "b.tmp": "xy"})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", "STATS"])
    return expect(b"\n  **/*.tmp: 2 item(s), 4 B\n" in o, f"stdout tail {o[-60:]!r}")


@check("TXT-C3", "classic")
def _(t, d):
    tree(d, {"a.tmp": "xy", "b.tmp": "xy", "ro/c.tmp": "xy"})
    os.chmod(os.path.join(d, "ro"), 0o555)
    c, o, e = run(t, d, ["**/*.tmp"], ["YES"])
    summary = o.endswith(b"Removed 2 item(s), 4 B.\n")
    err = any(x.startswith(f"{t}: ".encode()) and b"c.tmp" in x for x in e.split(b"\n"))
    return expect(summary and err, f"summary={summary}, failure on stderr={err}")


@check("JSON-C1", "classic")
def _(t, d):
    tree(d, {"a.tmp": "xy", "ro/c.tmp": "xy"})
    os.chmod(os.path.join(d, "ro"), 0o555)
    c, o, e = run(t, d, ["**/*.tmp"], ["YES", "STATS", "JSON"])
    doc = jdoc(o)
    want = {"": ["matches", "summary", "stats", "failures"],
            "matches": ["path", "size", "pattern"],
            "summary": ["total_count", "total_size", "total_size_human", "freed_size", "freed_size_human", "dry_run"],
            "stats": ["pattern", "count", "size", "size_human"],
            "failures": ["path", "error"]}
    got = {"": list(doc), "summary": list(doc.get("summary", {}))}
    for k in ("matches", "stats", "failures"):
        got[k] = list(doc[k][0]) if doc.get(k) else None
    bad = [k or "top" for k in want if got.get(k) != want[k]]
    return expect(not bad, "wrong keys: " + ", ".join(bad))


@check("EXIT-C1", "classic")
def _(t, d):
    tree(d, {"x.tmp": "", "locked/x": ""})
    os.chmod(os.path.join(d, "locked"), 0)
    c_warn, _, _ = run(t, d, ["**/*.tmp"], ["DRY"])
    c_usage, _, _ = run(t, d, raw=[TOOLS[t], "--bogus-option"])
    c_dur, _, _ = run(t, d, ["**/*.tmp"], ["DRY", ("OLDER", "1D")])
    c_h, _, _ = run(t, d, raw=[TOOLS[t], "-h"])
    c_v, _, _ = run(t, d, raw=[TOOLS[t], "--version"])
    got = (c_warn, c_usage, c_dur, c_h, c_v)
    return expect(got == (0, 1, 1, 0, 0), f"warning {c_warn}, usage {c_usage}, bad duration {c_dur}, -h {c_h}, --version {c_v}")


@check("CFG-C1", "classic")
def _(t, d):
    home = os.path.join(d, "home")
    work = os.path.join(home, "w")
    xdg = os.path.join(d, "xdg")
    tree(work, {"x.tmp": "", "y.glob": "", "z.home": ""})
    write_cfg(t, os.path.join(work, CONFIG_NAME[t]), ["**/*.tmp"], "bogus_key = true\n")
    env = {"HOME": home, "XDG_CONFIG_HOME": xdg}
    run(t, ".", None, ["YES"], cwd=work, env=env)
    no_c = ex(work, "x.tmp")
    c, _, _ = run(t, ".", None, ["YES"], config=True, cwd=work, env=env)
    unknown = c == 0 and not ex(work, "x.tmp")
    os.remove(os.path.join(work, CONFIG_NAME[t]))
    write_cfg(t, os.path.join(home, CONFIG_NAME[t]), ["**/*.home"])
    os.makedirs(os.path.join(xdg, t))
    write_cfg(t, os.path.join(xdg, t, "config.toml"), ["**/*.glob"])
    run(t, ".", None, ["YES"], config=True, cwd=work, env=env)
    home_stop = ex(work, "z.home")
    xdg_read = not ex(work, "y.glob")
    return expect(no_c and unknown and home_stop and xdg_read,
                  f"ignored without -c={no_c}, unknown key ignored={unknown}, ~ file ignored={home_stop}, global read={xdg_read}")


# ---------------------------------------------------------------- probes for divergences
PROBES = []


def probe(did):
    def deco(fn):
        PROBES.append((did, fn))
        return fn
    return deco


@probe("D-5 file as root: exit")
def _(t, d):
    tree(d, {"f": ""})
    c, o, e = run(t, os.path.join(d, "f"), ["**/*.tmp"], ["DRY"])
    return str(c)


@probe("D-7 `[ab].tmp` matches a.tmp")
def _(t, d):
    tree(d, {"a.tmp": ""})
    c, o, e = run(t, d, ["[ab].tmp"], ["YES"])
    return "yes" if not ex(d, "a.tmp") else f"no (exit {c})"


@probe("D-8 `x.log` matches p/q/x.log")
def _(t, d):
    tree(d, {"p/q/x.log": ""})
    run(t, d, ["x.log"], ["YES"])
    return "yes" if not ex(d, "p/q/x.log") else "no"


@probe("D-10 `-g *.log` keeps defaults")
def _(t, d):
    tree(d, {"x.pyc": "", "y.log": ""})
    run(t, d, ["**/*.log"], ["YES"])
    return "yes" if not ex(d, "x.pyc") else "no (replaces)"


@probe("D-11 default excludes venv")
def _(t, d):
    tree(d, {"venv/x.pyc": "", ".venv/x.pyc": ""})
    run(t, d, None, ["YES"])
    return "yes" if ex(d, "venv/x.pyc") and ex(d, ".venv/x.pyc") else "no"


@probe("D-15 .git inside matched dir removed")
def _(t, d):
    tree(d, {"__pycache__/.git/HEAD": "x"})
    run(t, d, ["**/__pycache__"], ["YES"])
    return "yes (removed)" if not ex(d, "__pycache__/.git") else "no (kept)"


@probe("D-18 matched symlink removed without -i")
def _(t, d):
    tree(d, {"real": "", "l.pyc": ("link", "real")})
    run(t, d, ["**/*.pyc"], ["YES"])
    return "yes" if not ex(d, "l.pyc") else "no"


@probe("D-20 FIFO matched: exit, removed")
def _(t, d):
    os.mkfifo(os.path.join(d, "f.pyc"))
    c, o, e = run(t, d, ["**/*.pyc"], ["YES"])
    return f"exit {c}, removed={not ex(d, 'f.pyc')}"


@probe("D-31 symlink target size")
def _(t, d):
    tree(d, {"realfile": "", "l.pyc": ("link", "realfile")})
    cmd = argv(t, d, ["**/*.pyc"], ["DRY", "JSON"])
    if t != "cclean":
        cmd.append("-i")
    c, o, e = run(t, d, raw=cmd)
    ts = targets(jdoc(o))
    return str(ts[0].get("size", ts[0].get("bytes"))) if ts else "not listed"


@probe("D-34 prompt stream")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], [], stdin=b"n")
    return "stdout" if b"[y" in o.lower() else ("stderr" if b"[y" in e.lower() else "none")


@probe("D-38 empty result stdout")
def _(t, d):
    c, o, e = run(t, d, ["**/none"], ["DRY"])
    return repr(o.decode(errors="replace").strip()[:40]) or "''"


@probe("D-39 dry-run trailer")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY"])
    last = [x for x in lines(o) if x.strip()]
    return repr(last[-1][:50]) if last else "''"


@probe("D-45 JSON top-level keys")
def _(t, d):
    tree(d, {"x.tmp": ""})
    c, o, e = run(t, d, ["**/*.tmp"], ["DRY", "JSON"])
    return " ".join(jdoc(o).keys())


def main():
    args = sys.argv[1:]
    out_json = None
    if args[:1] == ["--json"]:
        if len(args) < 2:
            sys.exit("--json needs a file")
        out_json, args = args[1], args[2:]
    only = args or list(TOOLS)
    unknown = [t for t in only if t not in TOOLS]
    if unknown:
        sys.exit("unknown tool: " + ", ".join(unknown))
    if "hclean" in only and not TOOLS["hclean"]:
        TOOLS["hclean"] = cabal_bin()
    missing = [t for t in only if not (TOOLS[t] and os.access(TOOLS[t], os.X_OK))]
    if missing:
        sys.exit("binary not found: " + ", ".join(f"{t} ({TOOLS[t]})" for t in missing))
    if os.geteuid() == 0:
        sys.exit("run as a normal user: root bypasses the permission checks")
    results = {}
    for rid, prof, fn in CHECKS + [(did, None, fn) for did, fn in PROBES]:
        for t in only:
            if prof not in (None, "core", PROFILE[t]):
                results.setdefault(rid, {})[t] = ("-", "other profile")
                continue
            d = fresh()
            try:
                r = fn(t, d)
            except NA as e:
                r = ("N/A", str(e))
            except Exception as e:
                r = ("ERROR", f"{type(e).__name__}: {e}"[:160])
            finally:
                cleanup(d)
            results.setdefault(rid, {})[t] = r
    if out_json:
        with open(out_json, "w") as f:
            json.dump(results, f, indent=1)
    for rid, row in results.items():
        cells = []
        for t in only:
            r = row[t]
            cells.append(f"{t}={r[0] if isinstance(r, tuple) else r}")
        print(f"{rid:40} " + "  ".join(cells))
        for t in only:
            r = row[t]
            if isinstance(r, tuple) and r[0] in ("FAIL", "ERROR") and r[1]:
                print(f"{'':40}   {t}: {r[1]}")
    print()
    for t in only:
        n = {}
        for rid, prof, fn in CHECKS:
            r = results[rid][t][0]
            n[r] = n.get(r, 0) + 1
        print(f"{t} ({PROFILE[t]}): " + ", ".join(f"{k} {n.get(k, 0)}" for k in ("PASS", "FAIL", "N/A", "ERROR")))
    failed = any(isinstance(row[t], tuple) and row[t][0] in ("FAIL", "ERROR")
                 for row in results.values() for t in only)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
