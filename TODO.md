# TODO

## Critical

## High

## Medium

### Features

- [ ] **An installed CMake package config**: `make install` places `libcclean.a` and the headers under the prefix, but exports no `cclean-config.cmake`, so a consumer cannot `find_package(cclean)` and has to name the archive and `-pthread` itself. Needs an export set, a generated version file, and a decision about whether the static library is the supported distribution form.

- [ ] **.NET build output**: `bin/` and `obj/`, marked by a `*.csproj`, `*.fsproj` or `*.sln` file. Needs marker matching by glob rather than by exact name, which `defaults::artifacts` does not currently support. `bin` and `obj` are generic enough names that the marker is the only safeguard.

### Tests

- [ ] **Fuzzing**: the configuration parser, the glob compiler and the numeric filter parsers all take untrusted text and none has a fuzz target. The malformed-input cases in the suites are hand-written, so they cover the shapes that were already known to be wrong.

- [ ] **Deletion races are not tested**: the removal path is descriptor-relative and no-follow, but nothing in the suites actually competes with it. Proving the property needs a second process renaming components mid-run, which is inherently timing-dependent.

## Low

### Known limitations

- [ ] **`ROOT` is exempt from the protected list**: pointing cclean at `.git` scans it. Deliberate, in that naming a directory is explicit, but inconsistent with the same name being protected during a walk.

