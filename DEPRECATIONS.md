# Deprecated compatibility launchers

Compatibility launchers add no build logic; they delegate to the Go drivers in
`cmd/snesbuild` and `cmd/v2regen`. New scripts and CI should call those drivers
directly.

## Removed Python shims (2026-08-12)

The repository no longer referenced these wrappers, so they were removed after
a repo-wide caller audit. Use the direct replacements below.

| Shim | Direct replacement |
|---|---|
| removed `tools/gen_metadata.py` | `v2regen metadata` |
| removed `tools/link_audit.py`   | `v2regen link-audit` |
| removed `tools/opcode_diff.py`  | `v2regen opcode-diff` |
| removed `tools/stub_census.py`  | `v2regen stub-census` |

## Retained shell launchers → `snesbuild` (D10)

Keep these while repository automation or user-facing documentation invokes
them by name.

| Launcher | Direct replacement |
|---|---|
| `tools/regen.sh`       | `snesbuild regen` |
| `tools/build-macos.sh` | `snesbuild build` |

Both self-describe as "Compatibility launcher" and exec the cross-platform
`snesbuild` driver (or `go run ./cmd/snesbuild`).
