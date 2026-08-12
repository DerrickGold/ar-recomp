#!/usr/bin/env bash
# canary.sh — the ROM-free per-commit gate (spec §0.7 rule 6).
#
# Run after EVERY `git am <patch>` on the ROM machine, and locally after each commit
# while authoring. It rebuilds and runs the ROM-FREE test tier (the tests/ suite compiles
# individual src/*.c + SDL + PPU — it never links the generated ROM banks in src/gen/).
#
#   - Clean apply + GREEN canary  -> proceed to the next patch.
#   - Clean apply + RED canary    -> case (B) SEMANTIC breakage: a patch applied but is now
#                                    wrong (a depends_on parent changed shape). Re-verify the
#                                    parent finding; do NOT keep applying.
#   - `git am` HALTED with .rej    -> case (A) TEXTUAL conflict: resolve the .rej, `git add`,
#                                    `git am --continue`; if one file keeps conflicting,
#                                    reorder that file's commit block (spec §0.7 rule 3).
#
# NEVER runs `git push` (no push credentials on the authoring machine — spec §0.5).
#
# Prereq: AR0 (the tests-only configure path) must be in place so CMake configures without
# a ROM / without src/gen/*.c. If AR0 is not yet applied, this script stubs src/gen so the
# existing configure guard passes; that stub compiles nothing into the tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build/canary"
cd "${ROOT}"

# --- ROM-free configure guard workaround -------------------------------------------------
# The top-level CMakeLists FATAL_ERRORs when src/gen/*.c is empty. The tests never use those
# banks, so drop a single empty stub TU to satisfy the glob until AR0 lands a real
# -DAR_TESTS_ONLY path. (Harmless: it contributes no symbols.)
STUB="${ROOT}/src/gen/_canary_stub.c"
STUB_CREATED=0
if ! ls "${ROOT}"/src/gen/*.c >/dev/null 2>&1; then
  mkdir -p "${ROOT}/src/gen"
  printf '/* canary stub: satisfies the src/gen glob for ROM-free test builds; no symbols. */\n' > "${STUB}"
  STUB_CREATED=1
  echo "[canary] src/gen empty — wrote temporary stub ${STUB#${ROOT}/}"
fi
cleanup() {
  if [ "${STUB_CREATED}" = "1" ]; then
    rm -f "${STUB}"
  fi
}
trap cleanup EXIT

# --- configure + build + test (ROM-free tier only) ---------------------------------------
echo "[canary] configure (BUILD_TESTING=ON, SDL_VIDEODRIVER=dummy for render-harness)"
cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON >/tmp/canary_cfg.log 2>&1 \
  || { echo "[canary] CONFIGURE FAILED — see /tmp/canary_cfg.log"; tail -15 /tmp/canary_cfg.log; exit 1; }

# Build ONLY test executables—not `all`/ActRaiserRecomp, which needs generated
# ROM banks. Discover the targets from the configured Ninja graph so adding a
# CTest cannot silently leave its executable out of this gate.
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

# actraiser_save_system needs a gitignored, ROM-derived fixture
# (${ACTRAISER_SOURCE_DIR}/save.sim-blank.bak.srm) that is absent on a ROM-free machine, so
# it cannot pass here — exclude it from the ROM-free gate. It is part of the Wave-3 on-device
# checklist instead. (Override with CANARY_ALL=1 once the fixture is present.)
EXCLUDE_REGEX="${CANARY_ALL:+}"
[ -z "${CANARY_ALL:-}" ] && EXCLUDE_REGEX="actraiser_save_system"

echo "[canary] ctest (software renderer; dummy video/audio drivers)"
( cd "${BUILD}" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ctest --output-on-failure ${EXCLUDE_REGEX:+--exclude-regex "${EXCLUDE_REGEX}"} ) \
  || { echo "[canary] TESTS FAILED — a clean git am + red here = case (B) semantic breakage; re-verify the depends_on parent (spec §0.7 rule 6)"; exit 1; }
[ -n "${EXCLUDE_REGEX}" ] && echo "[canary] (skipped ${EXCLUDE_REGEX} — needs a ROM-derived fixture; it is a Wave-3 on-device check)"

echo "[canary] GREEN — safe to proceed to the next patch."
