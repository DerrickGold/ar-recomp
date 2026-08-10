# T2a — Split the sim3D / world-navigation renderer out of `present.c`

**Status: EXECUTED.** Applied on a machine with a ROM and a linkable game
target. `present.c` is 4,551 -> 1,262 lines; the renderer is `src/present_sim3d.c`
with `src/present_internal.h` as the boundary, and the world-map half was
subsequently split again into `src/present_world_nav.c`
(`src/present_sim3d_internal.h`). What the execution found that this spec did not
anticipate is recorded at the end, under
[Execution notes](#execution-notes-what-the-spec-did-not-anticipate).

The original analysis follows, kept for the reasoning behind the partition.

---

**Original status: SPEC ONLY — not executed.** This split was analysed in depth on a
ROM-less machine and deliberately **not** applied there, because `present.c`
is not compiled by the `AR_TESTS_ONLY` test tier and no ROM was available, so
the only local oracle is `cc -fsyntax-only`. A syntax check cannot detect the
errors that matter for this file (a mis-extracted function body or a static
wrongly shared between the two translation units compiles clean and renders
wrong). Three scripted attempts each produced a different structural mis-slice
that only surfaced as an undefined-symbol error — evidence that this extraction
must be executed where it can be **linked and run against a ROM**.

Execute this on a machine that can build the full game target and visually
confirm sim-mode 3D output is unchanged.

## Goal

`present.c` is ~4,551 lines; ~58% of it is the sim-mode 3D + world-navigation
renderer, interleaved with the flat/diorama present path. Move that renderer to
a new `src/present_sim3d.c`, leaving `present.c` with the flat/diorama path and
the public entry points. Introduce `src/present_internal.h` for the symbols the
two translation units share. **No behaviour change** — every moved definition
is moved verbatim; the rendered output must be byte-identical.

## The partition (verified by usage analysis)

Do NOT trust a hardcoded count — derive the set. The rule is: a definition
MOVES if, after the sim functions leave, it has zero remaining users in
`present.c`. Roughly ~65 functions + ~35 file-statics/types (order-of-100
definitions, ~58% of the file); the exact set falls out of the derivation
below. The three scripted attempts here disagreed on the total by a few
because the name filter alone is incomplete — hence the usage rule.

**Moves to `present_sim3d.c`:** every function, type, enum, and file-static
whose name matches `Sim` / `WorldNavigation` / `Underlay`, PLUS these that the
name filter misses but usage proves are sim-only (each has zero remaining users
in `present.c` after the sim functions leave):

- Types/enums: `SimCullFade`, the anonymous `kSimGround{Columns,Rows,VertexCount,IndexCount}`
  enum, `SimEffectParticleMotion`, `SimEffectStyle`, `kSimMaxParticlesPerEffect`
  enum, `SimBillboardPassKind`, `SimObjectTierFilter`, `SimBillboardPass`,
  `SimCloudLayer`, `SimDynamicCameraState`, `kSimShadowBlurTaps` enum, and the
  anonymous enums local to sim functions (verify each block's constants are
  referenced only by moved code before moving).
- Const data: `kEffectCircle32` (`static const float[32][2]`), `kSimCloudLayers`.
- Statics incl. multi-declarator ones: all `s_sim_*` and
  `s_world_navigation_*` including the paired `s_sim_shadow_w, s_sim_shadow_h`
  and `s_sim_rim_w, s_sim_rim_h`, and `g_sim_dyncam`, `kPi`, and the `kSim*`
  tuning constants.

**STAYS in `present.c` (public entry points — never move, even though they
touch sim statics):** `PresentCompositeScene`, `PresentUpload`,
`PresentRendererResources_Reset`, `Present_EffectRendererSupported`,
`Present_SimRimMaskSupported`, `ComputePresentationViewport`, `FrameSlot_Capture`.
The later terminal-order cleanup added the public `PresentFrame` orchestrator in
`present_frame.c`; it calls the internal scene compositor named above.

## The boundary — `present_internal.h`

Not a public API (that is `present.h`). Contents:

1. **Full definitions** of the two effect types both files need by-value/field
   access — move these OUT of `present.c` into the header:
   `EffectRenderState`, `EffectBatch`.
2. **Prototypes of 10 shared helpers** that stay defined in `present.c` and are
   called by the sim renderer — and must be **de-static'd** in `present.c`:
   `ToFRect`, `ApplyLogicalPresentation`, `PresentHudOverlayComposited`,
   `PresentSceneInspector`, `PresentCheatBadge`, `EffectRendererAvailable`,
   `DisableEffectAdd`, `BeginEffectAdd`, `EndEffectAdd`, `SubmitEffectBatch`.
   (`ComputePresentationViewport` is already public in `present.h` — do not
   redeclare.)
3. **Prototypes of the sim entry points** defined in `present_sim3d.c` and
   called back by `present.c`: `PresentSim3D`, `PresentWorldNavigation3D`,
   `UploadSimTownCanvas`, `UploadWorldNavigationComposition`, plus the new
   `PresentSim3D_ResetResources`.

`present_sim3d.c` also needs the `extern` block of presentation-resource
globals (`g_renderer`, `g_texture`, `g_hud_bg/obj_texture`, `g_pixels`,
`g_hud_bg/obj_pixels`, `g_diorama_textures[]`, `g_diorama_layer_pixels[]`,
`g_sim_obj_atlas_texture`, `g_sim3d_layer_textures[]`, `g_sim3d_flat_texture`)
— copy it verbatim from `present.c`'s top.

## The one function that must be SPLIT, not moved

`PresentRendererResources_Reset` (present.c:2513-2556) frees both HUD-composite
(stay) and sim (move) textures. Split it:

- **Keep in `present.c`:** lines 2514-2517 (`s_hud_composite_*`) and 2529-2530
  (`SDL_SetAtomicInt(&s_effect_add_supported/…_geometry_supported, 1)`), then
  add a call to `PresentSim3D_ResetResources();`.
- **Move to `present_sim3d.c`** as the body of a new
  `void PresentSim3D_ResetResources(void)`: lines 2518-2528 and 2531-2555 (all
  the `s_sim_*` / `s_world_navigation_*` frees). Place this function **after**
  the sim static definitions in the new file (C requires file-scope statics be
  declared before use — putting it at the top fails to compile).

⚠️ When implementing the splice, anchor on the *unique* `PresentRendererResources_Reset`
function, not a bare `if (s_sim_shadow_texture)` text match — that substring
also appears inside `EnsureSimShadowTexture`, and matching the wrong one
silently extracts the wrong body (this bug occurred in all three scripted
attempts).

## `present_sim3d.c` include block

Mirror `present.c`'s includes plus `present_internal.h`. Verify against the
compiler; the sim code uses SDL, math, the diorama headers (scroll/skybox),
`scene3d_math.h`, `render_capabilities.h`, and the `sim/*` headers.

## Verification (MUST run on the ROM machine)

1. `cc -fsyntax-only -Werror=implicit-function-declaration` on **both** TUs —
   necessary but NOT sufficient (this is all the origin machine could do).
2. Full game link: `cmake --build --preset play`. A missing shared-helper
   declaration or a mis-partitioned static surfaces here as an undefined or
   duplicate symbol.
3. **Run with a ROM and visually confirm render-identity** of: an action-stage
   frame (flat + diorama, to prove the stay-path is intact), a SIM town 3D
   scene (`PresentSim3D`), and a world-navigation scene
   (`PresentWorldNavigation3D`). Compare against a pre-split build. This is the
   only check that validates correctness; everything above only validates that
   it builds.
4. If a frame capture / golden-image path exists, diff sim-mode frames
   before/after for byte-identity.

## Why this is worth doing carefully, not fast

`present.c` carries the D6 no-live-globals invariant (it must never read
`g_ppu`/`g_settings` live; all state comes via `const FrameSlot *`). Both
resulting files must preserve that. The split does not widen the contract —
`present_internal.h` exposes only present.c internals to the sim TU, not live
game state — but a careless extraction that pulls in a live global would break
the invariant the whole file exists to enforce.

---

## Execution notes (what the spec did not anticipate)

Recorded after the fact, because each cost time and each generalizes.

1. **The derivation rule was right and sufficient.** Run as a usage closure over
   a lossless top-level chunking of the file, it independently found every
   sim-only definition the name filter misses — `kEffectCircle32`, `kPi`,
   `CloudHash`/`CloudSmooth`/`CloudNoise` — and independently kept all ten shared
   helpers plus `EffectRenderState`/`EffectBatch` on the `present.c` side. No
   hand-tuning of the partition was needed. **Do not seed the closure with a
   case-insensitive `WorldNavigation` match**: it does not match the snake_case
   statics (`s_world_navigation_*`), which silently pins them on the wrong side.

2. **The extern block is DECLARATIONS, not definitions.** `g_sim_obj_atlas_texture`,
   `g_sim3d_layer_textures[]` and `g_sim3d_flat_texture` are owned by `main.c`.
   They must be **duplicated** into both files, not moved — `PresentUpload` stays
   and still touches them. The spec's "copy it verbatim" was right for the new
   file but the originals must also stay put.

3. **`s_sim_rim_mask_supported` crosses in both directions.** It is written by the
   moved rim path and read by `Present_SimRimMaskSupported()`, which stays as a
   public entry point. It ended up owned by the sim TU and `extern`'d through the
   boundary header. Any static in that shape needs an explicit decision.

4. **`PresentRendererResources_Reset` splits once per split.** The same surgery
   was needed again when the world-map renderer came out of `present_sim3d.c`
   (`PresentWorldNav_ResetResources`). Expect a reset split every time this family
   divides; it is the one function that touches every subsystem's textures.

5. **`extern` arrays have no size.** `sizeof(kSimCloudLayers)` could not follow the
   code across a TU boundary; the owning file now publishes `kSimCloudLayerCount`
   beside the table. Any `sizeof`-over-a-shared-table has the same problem.

### How it was actually verified

The spec asked for a visual confirmation; the repo supports something stronger.
`tools/sim3d_demo.py` stages a per-checkpoint SRAM seed + settings fixture, so its
frames are real content — 20/20 validation-error sets and all 156 rendered PPM
frames came out byte-identical to the pre-split build. That was backed by 28
headless composited frames (SIM on a pinned seed, action stage flat, action stage
diorama) and a line-conservation check showing exactly 16 changed lines, each an
enumerated de-static.

**The trap worth repeating:** an unstaged replay sits on the title screen and
compares byte-equal, so an A/B built that way "passes" while testing nothing. One
was written and believed here until the frames were converted to PNG and looked
at. Always look at a frame. See `docs/code-style.md` for the rule.

**Still not covered:** the world-map navigation renderer. No checkpoint sets
`AR_SIM3D_WORLD_NAV` and no staged replay reaches world-map travel, so it rests on
the clean link plus a manual confirmation. It was given its own translation unit
so that gap is visible in the file listing.
