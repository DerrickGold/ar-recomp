# Deprecated compatibility shims

These wrappers exist only so existing muscle memory and older automation keep
working. They are thin launchers that shell out to the Go drivers
(`cmd/snesbuild` and `cmd/v2regen`) and add no logic of their own. Prefer
calling the Go driver directly in any new script or CI step. They will be
removed once nothing invokes them by name.

**Keep for now** — some are still referenced from the docs (e.g. `tools/regen.sh`
is cited as a fallback in `README.md` and `DEBUG.md`), so do not delete them
until those references are updated.

## Python shims → `v2regen` (D9)

| Shim | Direct replacement |
|---|---|
| `tools/gen_metadata.py` | `v2regen metadata` |
| `tools/link_audit.py`   | `v2regen link-audit` |
| `tools/opcode_diff.py`  | `v2regen opcode-diff` |
| `tools/stub_census.py`  | `v2regen stub-census` |

Each is a ~15–24 line `os.execvp("go", ["go", "run", "./cmd/v2regen", <subcmd>, …])`
passthrough.

## Shell launchers → `snesbuild` (D10)

| Launcher | Direct replacement |
|---|---|
| `tools/regen.sh`       | `snesbuild regen` |
| `tools/build-macos.sh` | `snesbuild build` |

Both self-describe as "Compatibility launcher" and exec the cross-platform
`snesbuild` driver (or `go run ./cmd/snesbuild`).
