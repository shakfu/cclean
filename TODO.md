# TODO

## Features

- [x] **`--dependencies`**: Marker-guarded dependency trees are handled separately from build artifacts because restoration may require a network, registry access, credentials, or post-install state.

- [x] **Config file**: `.cclean.toml` stores `patterns`, `excludes`, `defaults`, `build_artifacts`, `dependencies`, `skip_protected`, `dependency_markers`, `project_roots`, `older_than` and `larger_than`. `skip_protected` mirrors `--no-skip`, disabling the protected list wholesale; the list itself cannot be extended or replaced from configuration. It is discovered upward from `ROOT`, and CLI values take precedence.

- [x] **`--format json`**: Machine-readable output for scripting, including target reasons and per-reason statistics. Diagnostics remain on stderr.

- [x] **`--yes`**: Explicit unattended deletion without changing the default confirmation behavior.

- [x] **Age and size filters**: `--older-than` and `--larger-than` restrict candidates before confirmation; both are available in configuration.

- [ ] **An installed CMake package config**: `make install` places `libcclean.a` and the headers under the prefix, but exports no `cclean-config.cmake`, so a consumer cannot `find_package(cclean)` and has to name the archive and `-pthread` itself. Needs an export set, a generated version file, and a decision about whether the static library is the supported distribution form.

- [ ] **.NET build output**: `bin/` and `obj/`, marked by a `*.csproj`, `*.fsproj` or `*.sln` file. Needs marker matching by glob rather than by exact name, which `defaults::artifacts` does not currently support. `bin` and `obj` are generic enough names that the marker is the only safeguard.

## Known limitations

- [x] **Monorepo project roots**: `.cclean.toml` can list explicit `project_roots` relative to `ROOT`, allowing marker-guarded artifact detection without weakening nested-repository protection.

- [x] **Unreadable directories are reported**: permission failures become warnings and return status 3, which is theirs alone; status 1 is a root that cannot be read or a removal that failed.

- [x] **Atomic pre-delete validation**: every component from `ROOT` down is opened `O_NOFOLLOW` through the descriptor above it, and every removal names an entry within a directory descriptor, so a path re-resolved between the check and the removal can no longer point somewhere else. `ROOT` itself is opened by name, because the user typed it. The entry the walk ends on is checked for the device and inode the scan recorded, not merely its type, so a directory replaced by another directory while the list was on screen is refused; a matched directory is confirmed a second time through `fstat()` on its own descriptor and emptied through it. A target that is not a directory has no descriptor to unlink through, so its check and its `unlinkat()` are adjacent syscalls rather than one operation -- the remaining window, and the smallest one the POSIX interface allows.

- [x] **Configuration can be pinned or switched off**: the upward search does not stop at the project, so `--config FILE` names the file to read and `--no-config` reads none. `--verbose` and the JSON document name the file a run used.

- [ ] **`ROOT` is exempt from the protected list**: pointing cclean at `.git` scans it. Deliberate, in that naming a directory is explicit, but inconsistent with the same name being protected during a walk.

## Tests

- [x] **Terminal confirmation is tested**: the raw-mode branch is driven through a pseudo-terminal from `python3`, which behaves the same on macOS and Linux where `script` does not. The checks are skipped when `python3` is absent.

- [ ] **Fuzzing**: the configuration parser, the glob compiler and the numeric filter parsers all take untrusted text and none has a fuzz target. The malformed-input cases in the suites are hand-written, so they cover the shapes that were already known to be wrong.

- [ ] **Deletion races are not tested**: the removal path is descriptor-relative and no-follow, but nothing in the suites actually competes with it. Proving the property needs a second process renaming components mid-run, which is inherently timing-dependent.
