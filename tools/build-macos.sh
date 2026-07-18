#!/usr/bin/env bash
# Compatibility launcher for macOS. Cross-platform automation should invoke:
#   snesbuild build --root . --config Release
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG="Release"

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)
      echo "Usage: bash tools/build-macos.sh [--config Release|Debug | prod | debug]"
      exit 0
      ;;
    --config)
      shift
      CONFIG="${1:-Release}"
      ;;
    debug) CONFIG="Debug" ;;
    prod) CONFIG="Release" ;;
    *) echo "build-macos.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

DRIVER_ARGS=(build --root "$ROOT" --config "$CONFIG" --generator "Unix Makefiles")
if command -v brew >/dev/null 2>&1; then
  DRIVER_ARGS+=(--prefix-path "$(brew --prefix)")
fi

if [ -n "${SNESBUILD:-}" ]; then
  exec "$SNESBUILD" "${DRIVER_ARGS[@]}"
fi

HOST_DRIVER="$ROOT/snesrecomp-go/build/snesbuild"
if [ -x "$HOST_DRIVER" ]; then
  exec "$HOST_DRIVER" "${DRIVER_ARGS[@]}"
fi

GO_COMMAND="${GO:-$(command -v go || true)}"
if [ -z "$GO_COMMAND" ]; then
  echo "build-macos.sh: no snesbuild binary or Go toolchain found" >&2
  exit 1
fi

exec "$GO_COMMAND" -C "$ROOT/snesrecomp-go" run ./cmd/snesbuild "${DRIVER_ARGS[@]}"
