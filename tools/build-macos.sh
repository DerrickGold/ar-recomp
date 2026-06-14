#!/usr/bin/env bash
# Build ActRaiser Recompiled on macOS.
#
# Prerequisites:
#   brew install cmake sdl2 ninja python3
#   ln -s ../snesrecomp snesrecomp  (or clone as sibling)
#   bash tools/regen.sh --no-tests  (needs ar.sfc at repo root)
#
# Usage:
#   bash tools/build-macos.sh [--config prod|debug]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

CONFIG="Release"
for arg in "$@"; do
  case "$arg" in
    --config)  shift; CONFIG="${1:-Release}" ;;
    debug)     CONFIG="Debug" ;;
    prod)      CONFIG="Release" ;;
  esac
done

if [ ! -d "snesrecomp" ]; then
  echo "Error: snesrecomp/ not found. Create a symlink:"
  echo "  ln -s third_party/snesrecomp snesrecomp"
  exit 1
fi

if [ ! -d "src/gen" ] || [ -z "$(ls -A src/gen/ 2>/dev/null)" ]; then
  echo "Error: src/gen/ is empty. Run regen first:"
  echo "  bash tools/regen.sh --no-tests"
  exit 1
fi

echo "=== Building ActRaiser Recompiled ($CONFIG) ==="
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE="$CONFIG" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix 2>/dev/null || echo /usr/local)"

cmake --build build

echo
echo "=== Build complete ==="
echo "Run: ./build/ActRaiserRecomp"
