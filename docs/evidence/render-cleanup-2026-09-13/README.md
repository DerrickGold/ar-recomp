# Rendering cleanup after the all-effects optimizations

This is a correctness/maintainability pass over the renderer at `482cc605`,
not a new performance benchmark or a claim that every remaining hotspot is
exhausted. Previously enabled optimizations, helper threads, graphics settings,
shader math and effect quality remain unchanged.

## Changes

- SIM's retained linear-model source builder now checks the **prepared device
  capability before constructing geometry**. A failed boot-time preparation
  selects the existing ordinary projected geometry directly; it does not keep
  constructing source data only to reject it every present. This is an
  acceleration decision, not a switch to authentic graphics or a new visual
  setting. The explicit CPU diagnostic override remains available.
- The capability query is read-only, owner-thread, device-specific and false
  before preparation/after reset. Normal video reset re-prepares the pipelines;
  town transitions do not reset them. Failed preparation remains latched until
  reset. No shader compilation or allocation is added to the query.
- Shadow batch compatibility compares named placement/sampling fields instead
  of byte slices spanning unrelated uniform members. Conservative exact-bit
  geometric comparisons and the existing three-tap shader layout are retained.
- Unused affine/hardware-clipped reference model contracts, shaders and code
  are compiled only into the focused GPU test and opt-in projection benchmark.
  The shipping game no longer exposes or prewarms these two reference variants.
  Radial and linear production models still use the shared color fragment
  shader. Named private pipeline indices replace magic numeric variants.

The public runner SDK/ABI is unchanged. The project-private preparation field
is now explicitly named `linear_models`; no SDL types cross into scene code.
Test-only declarations live under `tests/support`, enabled by target-local
`AR_SIM3D_DEPTH_TEST_REFERENCE=1`. A boundary check and negative tests reject
reference API dependencies in shipping scene/header code. Geometry ownership,
queue admission, effect budgets and reset contracts are unchanged.

## Validation

- Release game, GPU integration test, shader-blob test, render-preparation test
  and opt-in model-projection benchmark all build on macOS/Metal.
- All six selected CTests pass: runner-private boundary, render-backend boundary,
  boundary negative cases, render preparation, shader blobs and depth GPU.
- The complete depth GPU suite also passes with diagnostic shadow batching
  disabled on Metal, and with default batching on Steam Deck/Vulkan.
- New test-only fault injection verifies a rejected linear preparation stays
  rejected for eight repeated preparation/presentation attempts, ordinary
  geometry still submits, reset permits successful preparation, and null or
  unbound-device queries cannot disturb the prepared device.
- The preparation unit test confirms missing linear acceleration does not
  disable otherwise supported visual settings.
- Symbol inspection confirms the four reference model entry points and fault
  injector are absent from the shipping executable and present in the GPU test.
- Nine all-effects Metal replay screenshots (three each for SIM, navigation and
  Sky Palace) are **byte-identical** to the preceding default-enabled baseline.
  All three final WRAM hashes match. Every target-scene metrics window reports
  zero failure, fallback, rejection, opt-out and limit counters, and the normal
  retained GPU reuse remains active. Legitimate blank/mode-entry bridges still
  appear in the transition logs; zero counters are not a claim of no bridges.

The fault is injected in the focused adapter test, not the full game. Normal
full-game replays exercise successful preparation and the source-model path.
Windows/D3D12 runtime execution was not tested in this pass. No elapsed-time or
FPS conclusion is drawn from the image runs: they use headless one-tick/present
capture with clouds frozen for repeatable pixels.

## Evidence and reproduction

[Verification data](results.json) records binary/source hashes, image hashes,
game-state hashes and target-scene counter checks. The local ignored
`raw-evidence.zip` contains only test/replay logs and verification data, not
ROMs, saves, game-state dumps, executables or installed settings.

The isolated Release build uses enhanced text off and the prior renderer
baseline plus only this cleanup; unrelated dirty packaging/localization edits
are excluded. The two reference targets alone gain the test-support include
path and compile definition. The Deck run uploads only a standalone test
executable to the existing private test directory and uses Wayland/Vulkan.
Installed games, settings and power policies are untouched.

Run the selected local checks with:

```sh
ctest --test-dir BUILD --output-on-failure -R '^actraiser_(runner_private_boundary|render_backend_boundary|render_backend_boundary_negative|render_preparation|shader_blob|sim3d_depth_pass_gpu)$'
AR_SIM3D_SHADOW_BATCH=0 BUILD/actraiser_sim3d_depth_pass_gpu_test
```

For the untimed full-game images, reuse the preceding pinned input directory
with `docs/evidence/shadow-batch-2026-09-13/probe.py`:

```sh
python3 docs/evidence/shadow-batch-2026-09-13/probe.py FRESH_ROOT \
  --binary BUILD/ActRaiserRecomp --inputs PINNED_INPUTS \
  --visual --default --cohort images --cases palace navigation sim-held \
  --set AR_PIPELINE_PERF=1
```

Compare the nine `images` hashes and the three `final_wram_sha256` fields in
`images-0-default/results.json` against the preceding default baseline.
