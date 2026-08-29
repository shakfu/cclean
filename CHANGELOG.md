# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0]

First release. See `README.md` for usage.

### Added

- Removal by glob pattern. Every match is listed with its size before a confirmation prompt. `--dry-run` stops after the listing. Patterns given on the command line are added to the built-in set; `--no-defaults` uses only what is given.

- Built-in patterns: `__pycache__`, `*.pyc`, `*.pyo`, `.*_cache`, `.DS_Store`.

- Protected directories, neither matched nor descended into: `.git`, `.hg`, `.svn`, `.venv`, `venv`, `.config`, `.ssh`, `.gnupg`. `--no-skip` walks them.

- `--build-artifacts` removes `build/` in a CMake project and `target/` in a Cargo one.

- `--verbose` to name each removal, `--help`, and `--` to end option parsing.

- Unit and command-line test suites, run by `make test` or `ctest`.

- CMake build, with the Makefile as a frontend over it.

### Decisions worth recording

Built-in patterns are matched against the final path component only, where command-line patterns are matched against that and against the path relative to `ROOT`. Matching the built-ins against the path made `.*_cache` mean "anything under a dot-directory ending in `_cache`", which took `.venv/lib/foo_cache` and `.git/objects/blob_cache` with it.

`--build-artifacts` requires `.git` and the marker file, `CMakeLists.txt` or `Cargo.toml`, in the same directory as the artifact directory. Matching `build/` and `target/` by name alone would take any directory with those names. The cost is that a monorepo, whose `.git` sits above the crate, is not detected.

Glob matching is open-coded rather than translated to `std::regex`. Regex was 257 ms of a 414 ms scan over 64,201 entries, and libc++ threw an uncaught `regex_error` from `regex_match`, not from the constructor, on a pattern with a long run of `*`, so the guard around pattern construction never saw it and `cclean . "****a"` aborted. The replacement simulates the pattern rather than backtracking, which also bounds `*a*a*a*a*b` against a long name at tokens x length.

A matched directory is removed whole and is not descended into, so its contents appear in neither the walk nor the listing. Counting a directory and the matched files inside it separately doubles the reported total.

[0.1.0]: https://github.com/shakfu/cclean/releases/tag/v0.1.0
