# TODO

## Features

- [x] **`--dependencies`**: Marker-guarded dependency trees are handled separately from build artifacts because restoration may require a network, registry access, credentials, or post-install state.

- [x] **Config file**: `.cclean.toml` stores `patterns`, `excludes`, `defaults`, `build_artifacts`, `dependencies`, `skip_protected`, `dependency_markers`, `project_roots`, `older_than` and `larger_than`. `skip_protected` mirrors `--no-skip`, disabling the protected list wholesale; the list itself cannot be extended or replaced from configuration. It is discovered upward from `ROOT`, and CLI values take precedence.

- [x] **`--format json`**: Machine-readable output for scripting, including target reasons and per-reason statistics. Diagnostics remain on stderr.

- [x] **`--yes`**: Explicit unattended deletion without changing the default confirmation behavior.

- [x] **Age and size filters**: `--older-than` and `--larger-than` restrict candidates before confirmation; both are available in configuration.

- [ ] **.NET build output**: `bin/` and `obj/`, marked by a `*.csproj`, `*.fsproj` or `*.sln` file. Needs marker matching by glob rather than by exact name, which `defaults::artifacts` does not currently support. `bin` and `obj` are generic enough names that the marker is the only safeguard.

## Known limitations

- [x] **Monorepo project roots**: `.cclean.toml` can list explicit `project_roots` relative to `ROOT`, allowing marker-guarded artifact detection without weakening nested-repository protection.

- [x] **Unreadable directories are reported**: permission failures become warnings and return status 1.

- [x] **Pre-delete validation**: targets are rechecked before removal so a changed or replaced path is rejected.

- [ ] **`ROOT` is exempt from the protected list**: pointing cclean at `.git` scans it. Deliberate, in that naming a directory is explicit, but inconsistent with the same name being protected during a walk.

## Tests

- [ ] **Terminal confirmation is untested**: the branch that puts the terminal in raw mode for a single keypress and restores it afterwards needs a pseudo-terminal. `script` can provide one, but its arguments differ between macOS and Linux. The piped branch is covered.
