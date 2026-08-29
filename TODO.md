# TODO

## Features

- [ ] **`--dependencies`**: A separate flag from `--build-artifacts` for downloaded dependency trees: `node_modules` beside `package.json`, `deps` beside `mix.exs`, `vendor` beside `go.mod` or `composer.json`. Kept separate because the two carry different risk. Everything `--build-artifacts` removes is rebuilt offline by a local toolchain from source already on disk; a dependency tree needs a network, a reachable registry, and credentials for any private packages, and without a committed lockfile may not restore the same tree. Native builds via node-gyp and `patch-package` postinstall state make a reinstall slower and more fragile still. Folding these into `--build-artifacts` would hide that difference behind one flag.

- [ ] **Config file**: A `.cclean.toml` or similar, so a pattern set and its excludes can live in a repository instead of being retyped. rclean searches upward from the working directory, then falls back to a global path.

- [ ] **`--format json`**: Machine-readable output for scripting. Write it to stdout with nothing else on that stream; rclean 0.3.0 shipped this with a
  log line ahead of the JSON, which made the documented `| jq` pipeline fail.

- [ ] **.NET build output**: `bin/` and `obj/`, marked by a `*.csproj`, `*.fsproj` or `*.sln` file. Needs marker matching by glob rather than by exact name, which `defaults::artifacts` does not currently support. `bin` and `obj` are generic enough names that the marker is the only safeguard.

## Known limitations

- [ ] **Monorepos are not detected by `--build-artifacts`**: the `.git` requirement is literal, so a crate at `repo/rust-app/` whose repository `.git` sits one level up does not match. Fixing it means searching upward for `.git` instead of requiring it as a sibling, which has to be reconciled with the rule that an artifact must be top-level in the outermost project: an upward search would find the monorepo root for a nested crate, but must not do so for a submodule.

- [ ] **Unreadable directories are skipped silently**: `skip_permission_denied` suppresses the error, so neither the walk nor an unreadable `ROOT` reports anything. An unreadable `ROOT` exits 0 with "No matching targets found" rather than failing.

- [ ] **`ROOT` is exempt from the protected list**: pointing cclean at `.git` scans it. Deliberate, in that naming a directory is explicit, but inconsistent with the same name being protected during a walk.

## Tests

- [ ] **Terminal confirmation is untested**: the branch that puts the terminal in raw mode for a single keypress and restores it afterwards needs a pseudo-terminal. `script` can provide one, but its arguments differ between macOS and Linux. The piped branch is covered.
