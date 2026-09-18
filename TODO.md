# TODO

## Critical

## High

## Medium

### Features

- [ ] **An installed CMake package config**: `make install` places `libcclean.a` and the headers under the prefix, but exports no `cclean-config.cmake`, so a consumer cannot `find_package(cclean)` and has to name the archive and `-pthread` itself. Needs an export set, a generated version file, and a decision about whether the static library is the supported distribution form.

- [ ] **.NET build output**: `bin/` and `obj/`, marked by a `*.csproj`, `*.fsproj` or `*.sln` file. Needs marker matching by glob rather than by exact name, which `defaults::artifacts` does not currently support. `bin` and `obj` are generic enough names that the marker is the only safeguard.

### Tests

- [ ] **Fuzzing**: the configuration parser, the glob compiler and the numeric filter parsers all take untrusted text and none has a fuzz target. The malformed-input cases in the suites are hand-written, so they cover the shapes that were already known to be wrong.

- [ ] **Deletion races are tested one thread deep**: `src/removal_hooks.hpp` injects a replacement into the walk and into the interval before the final unlink, which covers both points on the path to a reviewed target. Two gaps are left. The recursion that empties a matched directory re-resolves each child name within a descriptor it holds, and nothing stands a replacement in a child's place while it does. And no test runs two removals, or a removal and a build, against one tree at once: the hooks make one process deterministic and say nothing about what two of them interleave into.

## Low

### Known limitations

- [ ] **`ROOT` is exempt from the protected list**: pointing cclean at `.git` scans it. Deliberate, in that naming a directory is explicit, but inconsistent with the same name being protected during a walk.

