#!/usr/bin/env bash
# Compatibility launcher for the cross-platform Go project driver.
#
# New game automation should prepare project-owned content, then invoke the
# generic driver:
#   actraiser-builder native-source --root . --rom ar.sfc
#   snesbuild regen --root . --rom ar.sfc
#
# This wrapper keeps the historical developer command working. It runs the Go
# tests by default; --no-tests disables them. SNESBUILD may name a prebuilt
# driver, and SNESRECOMP_JOBS remains a supported worker-count override.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GO_COMMAND="${GO:-$(command -v go || true)}"

if [ -n "${ACTRAISER_BUILDER:-}" ]; then
  "$ACTRAISER_BUILDER" native-source --root "$ROOT" --rom "$ROOT/ar.sfc"
elif [ -x "$ROOT/installer/build/actraiser-builder" ]; then
  "$ROOT/installer/build/actraiser-builder" native-source --root "$ROOT" --rom "$ROOT/ar.sfc"
elif [ -n "$GO_COMMAND" ]; then
  "$GO_COMMAND" -C "$ROOT/installer" run ./cmd/actraiser-builder \
    native-source --root "$ROOT" --rom "$ROOT/ar.sfc"
else
  echo "regen.sh: no actraiser-builder binary or Go toolchain found" >&2
  echo "Set ACTRAISER_BUILDER=/path/to/actraiser-builder." >&2
  exit 1
fi

DRIVER_ARGS=(regen --root "$ROOT" --rom ar.sfc --run-tests)
if [ -n "${SNESRECOMP_JOBS:-}" ]; then
  DRIVER_ARGS+=(--jobs "$SNESRECOMP_JOBS")
fi
DRIVER_ARGS+=("$@")

if [ -n "${SNESBUILD:-}" ]; then
  exec "$SNESBUILD" "${DRIVER_ARGS[@]}"
fi

# This is a developer/source-checkout compatibility wrapper, so prefer the
# checked-out Go implementation when it is available. A bundled host driver
# can legitimately lag local source edits and would otherwise appear to
# regenerate successfully with yesterday's generator. Release automation that
# intentionally selects a downloaded binary invokes it directly (or sets
# SNESBUILD above).
if [ -n "$GO_COMMAND" ]; then
  exec "$GO_COMMAND" -C "$ROOT/snesrecomp-go" run ./cmd/snesbuild "${DRIVER_ARGS[@]}"
fi

HOST_DRIVER="$ROOT/snesrecomp-go/build/snesbuild"
if [ -x "$HOST_DRIVER" ]; then
  exec "$HOST_DRIVER" "${DRIVER_ARGS[@]}"
fi

echo "regen.sh: no snesbuild binary or Go toolchain found" >&2
echo "Download snesbuild for this platform or set SNESBUILD=/path/to/snesbuild." >&2
exit 1
