#!/usr/bin/env bash
# Compatibility launcher for the cross-platform Go project driver.
#
# New automation should invoke a downloaded snesbuild binary directly:
#   snesbuild regen --root . --rom ar.sfc
#
# This wrapper keeps the historical developer command working. It runs the Go
# tests by default; --no-tests disables them. SNESBUILD may name a prebuilt
# driver, and SNESRECOMP_JOBS remains a supported worker-count override.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DRIVER_ARGS=(regen --root "$ROOT" --rom ar.sfc --run-tests)
if [ -n "${SNESRECOMP_JOBS:-}" ]; then
  DRIVER_ARGS+=(--jobs "$SNESRECOMP_JOBS")
fi
DRIVER_ARGS+=("$@")

if [ -n "${SNESBUILD:-}" ]; then
  exec "$SNESBUILD" "${DRIVER_ARGS[@]}"
fi

HOST_DRIVER="$ROOT/snesrecomp-go/build/snesbuild"
if [ -x "$HOST_DRIVER" ]; then
  exec "$HOST_DRIVER" "${DRIVER_ARGS[@]}"
fi

GO_COMMAND="${GO:-$(command -v go || true)}"
if [ -z "$GO_COMMAND" ]; then
  echo "regen.sh: no snesbuild binary or Go toolchain found" >&2
  echo "Download snesbuild for this platform or set SNESBUILD=/path/to/snesbuild." >&2
  exit 1
fi

exec "$GO_COMMAND" -C "$ROOT/snesrecomp-go" run ./cmd/snesbuild "${DRIVER_ARGS[@]}"
