#!/usr/bin/env bash
# canary.sh — the ROM-free per-commit gate.
#
# It configures, builds, and runs the tests that link no generated ROM banks.
# The save-system fixture test is the sole default exclusion; set CANARY_ALL=1
# on a ROM machine with the fixture installed to include it. This script never
# builds the game binary or pushes commits.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build/canary"
cd "${ROOT}"

# Configure the test-only path explicitly so an empty src/gen is valid.
echo "[canary] configure ROM-free test tier"
cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
      -DAR_TESTS_ONLY=ON >/tmp/canary_cfg.log 2>&1 \
  || { echo "[canary] CONFIGURE FAILED — see /tmp/canary_cfg.log"; tail -15 /tmp/canary_cfg.log; exit 1; }

# Discover targets from Ninja so a newly registered test cannot be omitted from
# the gate by a stale hand-maintained list.
TEST_TARGETS=()
while IFS= read -r target; do
  TEST_TARGETS+=("${target}")
done < <(
  ninja -C "${BUILD}" -t targets all |
    sed -n 's/^\(actraiser_[A-Za-z0-9_]*_test\):.*/\1/p' |
    sort -u
)
if [ "${#TEST_TARGETS[@]}" -eq 0 ]; then
  echo "[canary] no ROM-free test targets found in ${BUILD}"
  exit 1
fi
echo "[canary] build the ${#TEST_TARGETS[@]} ROM-free test targets (not the game binary)"
cmake --build "${BUILD}" --target "${TEST_TARGETS[@]}" >/tmp/canary_build.log 2>&1 \
  || { echo "[canary] BUILD FAILED — see /tmp/canary_build.log"; tail -25 /tmp/canary_build.log; exit 1; }

# actraiser_save_system needs the gitignored, ROM-derived
# save.sim-blank.bak.srm fixture. Override the exclusion with CANARY_ALL=1.
EXCLUDE_REGEX="${CANARY_ALL:+}"
[ -z "${CANARY_ALL:-}" ] && EXCLUDE_REGEX="actraiser_save_system"

echo "[canary] ctest (software renderer; dummy video/audio drivers)"
( cd "${BUILD}" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ctest --output-on-failure ${EXCLUDE_REGEX:+--exclude-regex "${EXCLUDE_REGEX}"} ) \
  || { echo "[canary] TESTS FAILED"; exit 1; }
[ -n "${EXCLUDE_REGEX}" ] && echo "[canary] (skipped ${EXCLUDE_REGEX} — needs a ROM-derived fixture)"

echo "[canary] GREEN — ROM-free gate passed."
