# Game SDK evaluation

This is a design audit, not a claim that these games have been recompiled. It
checks whether a new project can describe its integration without importing
private runner layouts or adding title-specific policy to the SNES core.

## Current integration shape

A project supplies one immutable `RtlGameModule`. Identity and execution are
required; lifecycle, generated-state providers, and privileged audio policy are
capability-gated additions. The runner validates the module once, caches its
tables, owns provider installation/revocation, and exposes only an opaque runner
handle to lifecycle callbacks.

Game policy then uses two deliberately different surfaces:

- direct linked-game adapters for the few once-per-frame generated-code seams
  where repeated public-table validation measured poorly; and
- the versioned `SnesRunnerApi` for enhancement layers, inspection, derived PPU
  work, observations, and controlled mutations.

## Hypothetical project audit

| Need | Super Mario World-shaped project | Zelda 3-shaped project | Current result |
| --- | --- | --- | --- |
| ROM identity and initialization | Stable game ID plus verified ROM transforms | Same, potentially revision-specific transforms | Covered by identity and callback-lifetime initialization context |
| Generated execution | Main frame/NMI policy and optional dispatch recovery | Main frame/NMI/IRQ policy and optional dispatch recovery | Covered by the execution table; ROM-address policy stays in the project |
| Debug CPU/execution state | Generated register and block-history snapshots | Same | Covered without a concrete `Snes *`; provider context is module-owned |
| Horizontal scene extension | Scrolling tile backgrounds, status region, OAM extension | Multi-background overworld/dungeons, window/color effects, OAM extension | Covered by bounded begin/finalize frame policy plus generic providers, extents, captures, and OBJ metadata |
| Mode 7 | Specialized scenes | Map/transition-style scenes | Generic Mode-7 state, coordinate resolution, surfaces, and scanout are available |
| Scanline effects | IRQ/HDMA scroll and layer changes | IRQ/HDMA, window, mosaic, and color-math changes | Coherent PPU/DMA snapshots and scanout callbacks cover the mechanism |
| Enhanced audio | Driver-specific track/SFX classification and replacement | Different SPC driver and track catalogue | Safe-point audio mechanics are available; semantic track IDs remain game metadata |
| Save/load tooling | Existing in-process saves | Existing in-process saves | Gameplay works; caller-buffer versioned serialization is still an SDK gap |

## Zelda 3 re-evaluation

The third hypothetical project does not reveal a new core-hardware blocker.
Its expected background, sprite, Mode-7, DMA/HDMA, window, color-math, input,
and SPC needs map to game-neutral mechanisms already exposed by ABI v2. It also
fits the currently supported cartridge class and does not require title logic
inside the runner.

The first audit gap now has a concrete game-neutral contract:

1. **Frame policy has an explicit lifecycle.** `SrPpuFramePolicy` atomically
   begins a frame by replacing margin geometry, fill masks, row bands,
   vertical clipping, and HUD split state while clearing retained providers
   and extents. A conditional `FINALIZE` phase requires the same active margin
   budget and preserves newly published resources while applying decisions
   that depend on publication success. Requests are bounded, validated before
   mutation, synchronous, allocation-free, and retain no caller pointer.

One runner SDK gap remains, plus one deliberate game-project layer:

2. **Semantic audio is game-owned.** The runner intentionally stops at bounded
   SPC upload, DSP routing, mix, and extension safe points. Track names,
   replacement assets, loop metadata, and preview extraction belong in a
   game-owned catalogue/manifest above it. The integration workflow is now
   documented in `GAME_ENHANCEMENT_INTEGRATION.md`; these concepts should not
   become SNES-core ABI.
3. **Serialization remains the SDK gap.** A generic frontend or tool cannot yet
   request a versioned caller-buffer save image and queue a load through the
   public safe-point boundary.

ActRaiser now uses the same transaction for horizontal margins, layer fill and
motion, ordered row bands, provider-dependent finite-world correction,
vertical extension/clipping, capture padding, HUD split policy,
capture/composition, and developer diagnostics. The application has no
concrete-layout exceptions. A later hypothetical audit should use a HiROM or
coprocessor title, because another LoROM game does not challenge cartridge
mapping or enhancement-chip boundaries.

## Frame-policy acceptance result

- A fixture game can register only identity/execution and run without private
  binding calls.
- A complex fixture can add lifecycle, state, PPU-policy, and audio tables with
  exact capability/size validation and symmetric teardown.
- Frame policy has explicit begin/finalize/clear semantics and cannot retain
  callbacks or borrowed memory past its documented lifetime.
- No per-frame allocation or new full-memory/framebuffer copy is introduced.
- ActRaiser replay artifacts remain identical and the portable suite stays
  inside the established ABI performance gate.
- Public SDK headers compile from `include/snesrecomp` without the private
  implementation include root, and authored ActRaiser sources are fenced from
  private runner headers.
