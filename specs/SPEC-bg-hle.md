# SPEC-bg-hle — action background world provider and scene plan

**Status: In progress (BH2 differential validation).** The action-level decode
is mapped, the offline oracle exists, and the pure production `ActionBgWorld`
decoder, ROM-free contract suite, and default-off runtime differential observer
are implemented. The complete BH1 census, virtual tilemap provider, and unified
scene plan remain open; no production pixel path consumes the decoder yet.

This spec replaces the narrower original BH1 proposal. It keeps that proposal's
measured decoder evidence, but expands the target from "decode more margin
tiles" to a complete action-background HLE boundary:

- own full world-space BG1/BG2 tilemap data where the level really has it;
- express decorative, bounded, repeated, mirrored, and native-wrapping layers
  as first-class per-layer/per-band policy;
- retain the existing PPU for tile pixels, palette, HBlank/HDMA, priority,
  windows, transparency, mosaic, brightness, and color math;
- fail closed to the authentic SNES tilemap path per layer and per frame;
- delete the transactional ring-repair machinery only after exhaustive parity.

Companion references:

- `docs/rendering-engine.md` §3-§7, §12a/§12b, §13 and §13i;
- `docs/SEAMS.md`, Graphics/PPU rows;
- `docs/bug-ledger.md` §37 and §39;
- `docs/code-style.md` for module boundaries and render verification.

---

## 1. Decision summary

The HLE replaces the **tilemap source and edge-resolution policy**, not the PPU
renderer.

```text
native game logic, loaders, cameras and PPU register writes
                          |
                          v
              ActionBgWorld + ActionBgPlan
       full world tile words / live layer edge policy
                          |
                          v
             generic PPU virtual-tilemap seam
                          |
                          v
       existing scanline decode and priority compositor
     live VRAM + CGRAM + HBlank/HDMA + windows + color math
                          |
                          v
       authentic framebuffer and captured priority planes
```

This is intentionally a hybrid design. A finished, pre-rendered level texture
would be simpler only by discarding the effects that make the original scenes
correct. A virtual tilemap lets the PPU ask for the right tile word at arbitrary
signed world coordinates while everything downstream remains live.

The authentic 64x64-tile VRAM ring and original streamers remain available as:

1. the reference implementation;
2. the fallback for unrecognised or invalid states;
3. the differential oracle for later HLE changes;
4. the path for layers deliberately described by their live tilemap page.

They are not cleanup targets. The cleanup target is the duplicated host-side
repair machinery built around the ring.

---

## 2. Scope and non-goals

### 2.1 In scope

- Mode-1 action stages, map groups `$01-$07`.
- BG1 and BG2 tilemap sources, full signed world bounds and independent cameras.
- Horizontal widescreen margins and the diorama top extension.
- Existing narrow/decorative layer behavior: clamp, reflection, cyclic repeat,
  raw native wrap, and per-row policy bands.
- Death Heim boss hub, ending transition, boss-rush rooms and final arena.
- Immutable per-frame presentation metadata consumed by diorama mode.
- Runtime/offline differential comparison with the original decoder/ring.
- Retirement of superseded action-background repair code after acceptance.

### 2.2 Out of scope

- Gameplay collision, camera movement, object activation or spawn behavior.
- Creating new background art outside the bounds the game actually supplies.
- Replacing the PPU's tile pixel decoder, priority resolver or color math.
- BG3 HUD composition and promoted HUD OBJ behavior.
- Simulation towns, Sky Palace and Mode-7 world navigation. Their sources and
  ownership differ; the architecture may support them later, but this track
  does not migrate them.
- Removing the original recompiled background streamers.

The HLE is presentation-only. It must never make synthetic margin content
available to gameplay code or mutate CPU, WRAM, VRAM, OAM, CGRAM, stack, direct
page, or math-unit state.

---

## 3. Evidence: the level map is already decompressed in WRAM

The premise this track started from — "we would have to reverse the bank-$0A
compressed level stream to decode backgrounds" — is wrong, and in our favour.

ROM-to-WRAM decompression happens once at level entry. Per-frame "streaming" in
`rendering-engine.md` means feeding the 64x64 VRAM tilemap ring from data that is
already resident in WRAM. The completed HLE replaces only that WRAM-to-ring
indirection. The loader remains native.

`$02:B8A0` does not read ROM. It reads the level map from WRAM bank `$7E`, where
the map is a flat array of 256-byte pages. One page contains 16x16 metatile IDs,
covering 256x256 world pixels:

```text
page_index = (world_y >> 8) * (width >> 8)
           + (world_x >> 8)
           + (State46 >> 8)

metatile_address = (page_index << 8)
                 | (world_y & $F0)
                 | ((world_x & $F0) >> 4)

metatile_id = $7E:metatile_address
```

The original row builder requires `world_x` to be 256-aligned because it passes
one page origin at a time. The underlying data has no such display constraint:
an HLE lookup can select the page for each requested coordinate directly.

Metatile expansion is equally small. `State52` points to an eight-byte table per
metatile, containing four 16-bit tilemap words in TL, TR, BL, BR order:

```text
word_k = ($7E:[State52 + id * 8 + k * 2] & State54)
       | (State6B << 8)
```

`State54` is the word mask and `State6B` contributes the common attribute byte.
`$02:BED3` is only the SNES hardware multiply operation.

### 3.1 Per-layer source state

| State, X=`0` BG1 / `4` BG2 | Meaning |
| --- | --- |
| `$22/$24` | full camera X/Y |
| `$2E/$30` | layer pixel width/height |
| `$46` | high byte selects the first WRAM map page |
| `$48` | native tilemap VRAM base (`$6000/$7000`) |
| `$52` | metatile definition table pointer in bank `$7E` |
| `$54` | word mask applied to the four definitions |
| `$6B` | common attribute byte OR'd into the high byte |

### 3.2 Residency evidence and limits

Fillmore captures `runs/20260806-224602` and `runs/20260806-231345` establish:

- rendering every page from WRAM produces coherent geometry across the complete
  4096px BG1 while the camera is stationary;
- 36/48 BG1 pages and 18/18 BG2 pages contain content simultaneously;
- two runs with cameras 955px apart have byte-identical maps: 12288 BG1 bytes
  plus 4608 BG2 bytes, zero differences;
- the two maps occupy about 16.5 KiB of the 128 KiB WRAM mirror.

This proves full residency for Fillmore acts 1 and 2. BH1 must census every
action map and every mid-level transition before generalising it. The addressing
has no sliding-window term, so a nonresident map would require additional loader
behavior not represented in the decoder state; that makes full residency likely,
not guaranteed.

### 3.3 Existing offline oracle

The following tools already exist:

- `tools/bg_hle.py` decodes every displayed tile from snapshot WRAM and compares
  it to the live VRAM ring;
- `tools/bg_render_level.py` renders a complete layer from WRAM, VRAM character
  data and CGRAM to establish residency and visual coherence.

Measured comparisons:

| snapshot | BG1 | BG2 |
| --- | --- | --- |
| `20260806-224602/snap_00_gf3427` | 1789/1792 (99.83%) | **1792/1792** |
| `20260806-224602/snap_01_gf3545` | 1789/1792 (99.83%) | **1792/1792** |
| `20260806-231345/snap_00_gf2363` | 1784/1792 (99.55%) | **1792/1792** |
| `20260806-231345/snap_01_gf2726` | 1784/1792 (99.55%) | **1792/1792** |

Every BG1 disagreement is a stale margin-ring cell from ledger §37. The pure
decoder independently returned the stable world value. That is useful evidence,
but it is not permission to classify every future mismatch as a native bug: the
runtime comparator must report both values and fail the HLE gate until the cause
is established.

---

## 4. Why the current ring repair should be replaced

The native ring is 64x64 tiles, or 512x512 pixels per layer. The original game
only needs a 256x224 window, one row/column record per layer per game frame, and
10-bit PPU scroll values. Extended presentation exposed properties that were
never observable on hardware:

- fixed 256-aligned row-decode spans leave phase-dependent side holes;
- column decodes write filler outside their current 512px vertical page;
- neighboring world bands alias the same physical ring cells;
- one fixed record per layer can be overwritten if extra work is queued;
- repair cadence and the maximum live width are coupled to the remaining ring
  slack;
- negative synthetic rows can wrap to the opposite edge of a bounded layer;
- the current repair invokes recompiled builders while snapshotting/restoring
  CPU, 128 KiB WRAM and SNES math state.

`src/actraiser/actraiser_widescreen_bg.c` makes those constraints safe, but the
amount of code is evidence that the repair is operating below the right seam.
The world map itself is simpler than its ring maintenance.

---

## 5. Architecture and contracts

### 5.1 Pure per-frame scene state

The ActRaiser adapter captures live game state into a plain value before scanout.
The pure planner must not read `g_ram`, `g_ppu`, `g_settings`, environment
variables or renderer globals.

Sketch, not frozen ABI:

```c
typedef struct ActionBgLayerState {
  uint16_t camera_x, camera_y;
  uint16_t width, height;
  uint16_t map_page, tilemap_base;
  uint16_t metatile_table, word_mask;
  uint8_t attributes;
  uint8_t bgsc;
} ActionBgLayerState;

typedef struct ActionBgFrameState {
  uint8_t map_group, map_number;
  uint8_t death_heim_progress, death_heim_ending_state;
  bool decorative_padding_enabled;
  ActionBgLayerState layer[2];
} ActionBgFrameState;
```

Only fields that affect background ownership belong here. If a future rule
needs another value, adding it to this record and its capture site makes the
dependency compiler-visible.

### 5.2 Pure scene plan

```c
typedef enum ActionBgSourceKind {
  kActionBgSource_NativeTilemap,
  kActionBgSource_WorldMap,
  kActionBgSource_AuthenticViewport,
} ActionBgSourceKind;

typedef enum ActionBgEdgeMode {
  kActionBgEdge_Transparent,
  kActionBgEdge_LiveWorld,
  kActionBgEdge_Clamp,
  kActionBgEdge_Mirror,
  kActionBgEdge_Repeat,
  kActionBgEdge_RawWrap,
} ActionBgEdgeMode;

typedef struct ActionBgBand {
  uint16_t y0, y1;                 /* half-open authentic screen rows */
  ActionBgEdgeMode edge;
} ActionBgBand;

typedef struct ActionBgLayerPlan {
  bool valid;
  ActionBgSourceKind source;
  ActionBgEdgeMode default_edge;
  uint16_t world_width, world_height;
  uint8_t band_count;
  ActionBgBand bands[kActionBgMaxBands];
} ActionBgLayerPlan;

typedef struct ActionBgPlan {
  bool valid;
  ActionBgLayerPlan layer[2];
} ActionBgPlan;
```

`ActionBgPlan_Build(state, out)` is total and fail-closed: it zeroes `out`
first, returns false for an unclassified or internally inconsistent state, and
never leaves a partially valid plan.

The maximum band count is fixed and `_Static_assert`-checked. Existing action
scenes need at most one override band per layer; a small capacity such as four
leaves room without allocating during scanout.

### 5.3 World ownership and cache

`ActionBgWorld` owns expanded 8x8 tilemap words, not finished pixels. It is built
from explicit WRAM source spans and validated decoder state:

```c
bool ActionBgWorld_Update(ActionBgWorld *world,
                          const ActionBgDecodeInput *input);

bool ActionBgWorld_Lookup(const ActionBgWorld *world,
                          unsigned layer,
                          int tile_x, int tile_y,
                          uint16_t *entry);
```

Requirements:

- validate all page/table arithmetic against the supplied WRAM size before
  publishing a new world;
- build into scratch storage and publish atomically only after complete success;
- cache exact source bytes or a collision-safe change signature so level/room
  transitions and metatile mutations invalidate the result;
- give each successful publication a nonzero serial;
- distinguish **outside world** from **provider failure**. Outside world is a
  valid transparent result; provider failure disables the whole layer for that
  frame. Never mix HLE and native tile words because one individual lookup failed;
- reset ownership on ROM unload, map transition, reset and savestate load.

The initial implementation should expand the full tilemap because it is simple
to diff and cheap at known sizes. BH1 must census the largest action dimensions
before choosing fixed storage versus bounded allocation. No unchecked level
dimension may control an allocation or index.

### 5.4 Generic PPU virtual-tilemap seam

The SNES runtime must not know ActRaiser map IDs or scene policy. It receives a
generic, frame-scoped provider binding for a BG layer. The exact API is a BH3
design deliverable, but it must satisfy these invariants:

- binding is all-or-nothing per layer for the frame;
- the provider supplies tilemap words only;
- the PPU continues to fetch character pixels from live VRAM;
- a bounded out-of-world lookup yields transparent layer pixels without
  interpreting wrapped VRAM;
- provider state is render-only and excluded from savestates;
- `ppu_reset`/the per-frame policy reset clears every binding;
- zero bindings produce the pre-HLE code path without an extra conditional in
  inner pixel work beyond the provider branch at tile fetch.

#### Full camera anchoring

The PPU scroll registers expose only their hardware-width phase, while the HLE
world uses full 16-bit cameras. For each layer, the binding must carry:

1. the full camera anchor from `$22/$24` or `$26/$28`;
2. the corresponding hardware scroll phase at the frame anchor;
3. the current per-scanline scroll after HBlank/HDMA.

The PPU resolves a signed phase delta from the current hardware scroll to the
anchor and adds that delta to the full camera. This preserves per-line HDMA
while avoiding a false wrap every 1024 pixels. BH3 must pin the signed-wrap
formula with synthetic tests at `0/$3FF` and with an HDMA action capture before
using the provider outside margins.

### 5.5 Edge operators belong at two different stages

The planner owns every decision, but not every decision executes during tile
lookup.

| Edge policy | Execution stage | Reason |
| --- | --- | --- |
| Live finite world | virtual tilemap lookup | real world coordinates exist |
| Transparent world bound | virtual tilemap lookup | prevents opposite-edge wrap |
| Clamp | layer extent selection | no margin pixels should exist |
| Mirror | isolated rendered scanline | preserves windows/HDMA/transparency/priority |
| Repeat | isolated rendered scanline | repeats the line after its current raster displacement |
| Raw wrap | native tilemap path | the hardware wrap is the authored effect |

Mirror/repeat must not be implemented by fabricating repeated tilemap pages.
The existing `PpuMergePaddedBackground` behavior is at the correct stage: it
copies isolated z/color words after tile decode/windowing and before final
priority/color resolution.

The plan should eventually replace the collection of independent clamp/mirror/
repeat masks as the source of truth. During migration, a small adapter may
compile the plan into those existing setters to prove policy parity before the
provider changes rendering.

### 5.6 Immutable presentation handoff

Diorama/present code may not inspect live `g_ppu` or reclassify policy masks.
The resolved plan, or a bounded presentation-only projection of it, is copied
into `FrameSlot` after scanout.

It must express validity per layer and per band. The current
`DioramaBg2MarginSource` scalar cannot represent Death Heim's banded BG2 and is
therefore a migration target. Consumers should ask the frame-owned plan which
texture rows/columns are live, padded or absent.

---

## 6. PPU-effect preservation contract

The following remain wholly owned by the existing PPU:

| Effect/state | Source in the completed HLE |
| --- | --- |
| Character graphics and animation | live VRAM |
| Palette swaps, fades and cycling | live CGRAM |
| Tile palette, flips and priority | HLE tilemap word, identical format |
| Transparent color zero | existing bitplane decoder |
| HBlank/HDMA scroll changes | live per-scanline PPU registers plus full-camera anchor |
| VBlank DMA and BG page changes | native game/NMI path |
| Main/sub-screen designation | live `$212C-$212D` state |
| Windows | existing PPU window evaluation |
| Add/subtract/half color math | existing PPU resolve |
| Mosaic and master brightness | existing PPU scanline path |
| BG/OBJ priority order | existing z/priority buffers |

An implementation that rasterizes a complete static RGBA level texture and
skips any of these stages does not satisfy this spec.

For authentic rows, a provider-enabled frame must produce byte-identical final
pixels **and priority-plane captures**, not merely a visually similar composite.

Page-changing scenes are conservative. If a live BGSC page identifies authored
tilemap state not represented by the world source, that layer uses
`kActionBgSource_NativeTilemap` until the alternate source is explicitly mapped.

---

## 7. Existing action policy mapped into the plan

This table is the minimum shipped behavior the completed HLE must preserve. It
describes the policy result, not necessarily a permanent hardcoded table; exact
live BGSC and transition state remain authoritative where already measured.

| Scene/layer | Source and edge policy |
| --- | --- |
| Ordinary wide BG1/BG2 | `WorldMap`; live inside independent bounds, transparent outside |
| Narrow decorative BG2 with padding disabled | `AuthenticViewport`; clamp |
| Bloodpool decorative BG2 | authentic rendered line; mirror by default |
| Bloodpool act 1 water rows `136..224` | repeat band overriding mirrored upper rows |
| Aitos maps `$01-$03` BG2 | authentic rendered line; cyclic repeat |
| Northwall maps `$01-$05/$08` BG2 | authentic rendered line; cyclic repeat |
| Death Heim maps `$02-$07` narrow BG2 | authentic rendered line; cyclic repeat |
| Death Heim hub before ending sky | BG1/BG2 clamped; BG2 repeat band `144..224` |
| Death Heim hub after BGSC `$64/$74` handoff | BG1 clamp, live BG2 line mirror |
| Death Heim final arena `$0708` | native tilemaps with authored raw 256px wrap |
| Unclassified map, dimensions or transition | native fallback for the whole affected layer |

Vertical extension uses the same independent world bounds. A BG1 camera with
rows above it may extend while BG2 at camera Y zero returns transparent. There
is no separate opposite-edge clip heuristic in the completed HLE path.

---

## 8. Code layout and house-style constraints

### 8.1 New hand-written modules

| File | One concern |
| --- | --- |
| `src/action/action_bg_plan.c/.h` | pure frame state -> scene plan classification |
| `src/action/action_bg_world.c/.h` | validated world decode, cache and tile lookup |
| `src/actraiser/actraiser_action_bg.c/.h` | capture live state, own lifecycle, bind plan/provider |
| `tests/action_bg_plan_test.c` | exhaustive rule/transition matrix |
| `tests/action_bg_world_test.c` | synthetic decoder, bounds, invalidation and capacity tests |

The generic provider contract belongs in `snesrecomp-go/runtime/src/snes/ppu.h`
and its execution in `ppu.c`. It must not include an `action/` or `actraiser/`
header.

Add new game sources to `snesbuild.ini`. ROM-free test targets list their
individual sources in `CMakeLists.txt`, following existing test isolation.

### 8.2 Structural rules

- Functions target the project's roughly 150-logic-line budget. Split decode,
  validation, cache publication and policy classification rather than growing
  another monolith.
- No data definitions in headers.
- Static rule rows use designated initializers so source/edge/band intent is
  compiler-checked at the declaration.
- Keep programmer-only rendering semantics in C, not an INI/JSON registry.
- Use `_Static_assert` for two-layer assumptions, band capacity, enum mappings
  copied into `FrameSlot`, and provider coordinate/storage limits.
- Comments cite the measured map/run/ROM state and preserve traps; they do not
  narrate loops.
- `actraiser_rtl.c` receives a short capture/apply call. New map-specific policy
  does not go into `ActRaiser_ApplyWidescreenPolicy`.
- Present/diorama TUs receive immutable slot data and have no declaration for
  live game globals.

The desired result is a net reduction of `actraiser_rtl.c`, which is already a
named debt item in `docs/code-style.md`.

---

## 9. Implementation phases and hard gates

Do not combine phases merely because adjacent code is convenient. Each phase
must leave a working fallback and a measurement that proves its own behavior.

### BH1 — evidence census and pinned baselines

**Work**

- Run `tools/bg_hle.py` and `tools/bg_render_level.py` across representative
  snapshots for every action map group, room transition and BGSC handoff.
- Record maximum dimensions, map/table ranges, CHR-base assumptions, residency,
  mutation behavior and any provider-ineligible layers.
- Pin deterministic replay frames for ordinary world BG, narrow decorative
  BG2, Bloodpool mixed padding, an HDMA cloud/snow room, Death Heim hub/ending,
  final arena and Fillmore act-2 vertical extension.
- Capture final framebuffer plus individual priority planes.

**Gate**

- Every in-scope world layer is either proven resident/decodable or explicitly
  classified native-only with evidence.
- Every replay is visually inspected and has a positive control that differs.

No production rendering changes.

### BH2 — pure C world decoder and differential oracle

**Implementation status (2026-08-09): decoder and differential observer
implemented, gate not yet claimed.** `src/action/action_bg_world.c` validates the complete WRAM
map/table spans, snapshots the exact source bytes, expands the finite world into
scratch tile-word storage, and atomically publishes only after success. Its
128-KiB WRAM bound derives a maximum of 512 pages / 524,288 expanded tile words
without accepting an unchecked level dimension. Lookup distinguishes a valid
out-of-world coordinate from provider failure; reset and malformed input fail
closed. `tests/action_bg_world_test.c` covers page crossings, all metatile
quadrants, mask/attribute/flip/priority preservation, negative and finite
bounds, exact-byte cache invalidation, failed-publication retention, reset, and
the maximum storage case. `src/actraiser/actraiser_action_bg.c` captures the two
live low-WRAM layer records, validates their 64x64 PPU ring ownership, and
compares every authentic 256x224 tile fetch against resident VRAM. It resets on
room/load discontinuities, classifies every fail-closed fallback, and never
binds a virtual source or writes CPU, WRAM, VRAM, CGRAM, OAM, or PPU state.
`tests/actraiser_action_bg_test.c` covers capture bounds, ring quadrant
addressing, fractional-camera coverage, finite world edges, a positive mismatch
control, and provider-ineligible layouts.

The first deterministic Fillmore act-2 replay compared 6,729,804 tile words
over 7,276 layer-frames with zero mismatches and zero unexpected world-edge
lookups. A disabled/enabled A/B produced byte-identical final WRAM, SRAM,
dispatch log, and state dump. This is one checkpoint, not the complete BH1
matrix; that matrix remains required before BH2's zero-mismatch gate can pass.

**Work**

- Implement `ActionBgWorld` with explicit input, bounds checking, atomic
  publication and serial/invalidation behavior.
- Add synthetic ROM-free unit tests for page selection, all metatile quadrants,
  mask/attribute merge, flips/priority bits, negative/out-of-world coordinates,
  maximum dimensions and malformed pointers.
- Add `AR_ACTION_BG_HLE_COMPARE=1`: the pure C result is compared against the
  original `$B825/$B8A0` transaction or resident VRAM, but is not displayed.

**Gate**

- Zero unexplained tile-word mismatches across the BH1 replay matrix.
- Comparator failure cannot mutate game-visible state.
- ROM-free suite and full release suite pass.

### BH3 — pure scene plan with current-policy parity

**Work**

- Implement `ActionBgPlan_Build` and the complete map/transition matrix.
- Initially compile the resulting plan into the existing PPU clamp/mirror/
  repeat setters; the tile source remains native.
- Publish the resolved plan to diagnostics and unit-test every rule, including
  unknown-state fallback.

**Gate**

- Current flat and diorama output is byte-identical across all BH1 checkpoints.
- A policy census shows the new plan and old classification agree on every
  rendered action frame.

This phase proves ownership and classification independently of decoding.

### BH4 — virtual provider for synthetic margins only

**Work**

- Add the generic PPU provider seam and full-camera/scroll anchoring.
- Use it only for pixels outside the authentic 256x224 viewport. The center
  continues sampling the native ring.
- Resolve independent top/side bounds through the provider; retain native,
  post-raster mirror/repeat execution from the plan.

**Gate**

- Authentic center and priority planes remain byte-identical.
- HLE margin tiles are stable by world coordinate with zero contradictions.
- Fillmore act-2 BG2 contributes no pixels above its world boundary while BG1
  continues normally.
- Synthetic tests prove palette changes, transparency, priority, windows,
  color math and HBlank scroll still affect provider tiles.

### BH5 — provider owns the authentic world layer

**Work**

- Extend provider sampling to the authentic center for eligible world layers.
- Compare HLE and native tile words/pixels/priority planes on every displayed
  authentic coordinate.
- Keep the feature default-off behind `AR_ACTION_BG_HLE=1` and expose counters
  for eligible layers, native fallbacks and mismatches.

**Gate**

- Zero unexplained authentic-center tile, pixel and priority mismatches.
- WRAM, CPU and gameplay behavior remain byte-identical across deterministic
  replays.
- HDMA/parallax rooms retain their measured per-line motion.
- Performance is measured at the largest known world and maximum display span;
  no regression is accepted without an explicit budget decision.

### BH6 — unified decorative/mixed policy and exact diorama handoff

**Work**

- Make the plan the only source of map-specific action background decisions.
- Carry exact per-layer/per-band presentation metadata through `FrameSlot`.
- Replace the scalar `DioramaBg2MarginSource` classification.
- Exercise Bloodpool, Aitos, Northwall and every Death Heim state through the
  same plan, using native isolated-scanline mirror/repeat where required.

**Gate**

- All current decorative and Death Heim behavior is preserved without a second
  map-specific branch in `actraiser_rtl.c`, PPU or present code.
- Diorama no longer conservatively collapses a banded layer into one span.
- No present-side live-state reads are introduced.

### BH7 — default-on soak and release acceptance

**Work**

- Make HLE the default for eligible action world layers while retaining native
  fallback and `AR_ACTION_BG_HLE=0` A/B.
- Run complete action playthroughs and transition sweeps in 4:3, wide raw, wide
  full and diorama configurations, including vertical extension 0 and 32.
- Exercise pause/redraw, restart, warp, savestate load, screen resize and room
  transitions.

**Gate**

- Zero unexpected fallback or mismatch counters in the complete coverage set.
- Every unsupported/native-only case selects its fallback deliberately and is
  named by diagnostics.
- Positive controls prove the HLE is active in every eligible checkpoint.
- Final images and priority planes pass automated comparison and human review.

### BH8 — cleanup and documentation

Cleanup is a separate behavior-neutral change after BH7, not part of the default
flip.

**Remove or retire, subject to a final consumer census**

- action-world portions of `ActRaiser_WidescreenMarginRefresh`;
- `WsRefreshKey`, row/column builder trampolines, partial drains and the 128 KiB
  WRAM/CPU/math snapshot transaction;
- `ws_build_visible_row`, `ws_build_band_rows` and `AR_VEXT_BANDFIX`;
- `AR_WS_BGREFRESH`, `ws_bgrefresh` and their production setting plumbing;
- duplicated map-specific background classification from
  `ActRaiser_ApplyWidescreenPolicy`;
- `DioramaBg2MarginSource` and its scalar reverse-classification;
- old PPU policy masks/setters only where the new plan has no remaining runtime
  or cross-project consumer;
- `PpuSetVerticalMarginLayerClip` if provider bounds completely subsume it and a
  repository-wide census finds no other user.

**Retain**

- native action streamers, record drain and VRAM ring;
- native PPU tilemap path and all PPU effects;
- generic isolated-layer mirror/repeat implementation if still executed by the
  plan;
- bounded opt-in differential oracle;
- behavioral regression tests, migrated to the new contract;
- historical bug-ledger entries and evidence runs.

If removing a persisted setting makes old configuration files noisy, keep a
hidden load-only compatibility alias for one migration window; it must not
remain a second runtime source of truth.

**Gate**

- Same BH7 outputs, counters and performance before and after deletion.
- `git diff --check`, ROM-free suite, full release suite and deterministic replay
  matrix pass.
- `docs/rendering-engine.md`, `docs/SEAMS.md`, `docs/progress.md`, `DEBUG.md`,
  `docs/settings-system.md`, `docs/research-symbol-map.md` and this spec describe
  the final ownership accurately.

---

## 10. Verification matrix

At minimum, the staged harness must cross these dimensions:

| Dimension | Required cases |
| --- | --- |
| Display | authentic 4:3, wide raw, wide full |
| Presentation | flat, diorama; vertical extension 0 and 32 |
| Source | wide world BG1, wide world BG2, narrow decorative BG2, native raw wrap |
| Edge | left/right world start, middle, end; top boundary and deep vertical camera |
| Raster | no HDMA, HDMA horizontal bands, animated character data, BGSC page handoff |
| Policy | transparent, clamp, mirror, repeat, mixed repeat band, native fallback |
| Lifecycle | act entry, room transition, boss transition, pause, redraw, savestate load, restart |

For each applicable case, compare:

1. resolved `ActionBgPlan`;
2. HLE versus native tile word at every authentic fetch;
3. final authentic framebuffer;
4. BG1/BG2 low/high priority planes;
5. WRAM and CPU/gameplay state;
6. fallback/mismatch/overflow counters;
7. performance and cache rebuild count.

The render verification rules in `docs/code-style.md` are mandatory: inspect a
frame, run a positive control, and compare against a freshly rebuilt baseline.

---

## 11. Failure and fallback behavior

Fail closed at the largest coherent boundary.

- Invalid dimensions, source pointers, table bounds, capacity, cache state or
  unknown scene policy disable HLE for the complete affected layer that frame.
- Out-of-world coordinates on a valid bounded world are transparent; they are
  not failures.
- Do not fall back per tile after scanout has begun. A mixed HLE/native layer can
  hide provider bugs as plausible ring fragments.
- Log each new fallback reason once and keep cumulative counters available to
  the replay harness.
- Forced blank, reset and map-load transitional frames may use native rendering
  without being counted as defects when the plan explicitly classifies them.
- Savestate load invalidates every cache and requires a fresh successful build
  before rebinding.

---

## 12. Open questions BH1 must close

1. Are the complete BG1/BG2 maps resident for every action map and transition?
2. What are the maximum validated dimensions and expanded-tile storage cost?
3. Do any action events mutate metatile IDs or definition tables after load?
4. Which BGSC page changes describe alternate world sources versus authored
   native tilemaps that should remain fallback-only?
5. What is the exact signed full-camera/10-bit-scroll reconciliation formula
   across HDMA lines and phase wrap?
6. Are there any non-Mode-1 action sections where provider sampling must fail
   closed?
7. Does any action tile animation modify tilemap words rather than only live
   character VRAM?
8. Which generic PPU clamp/mirror/repeat APIs have consumers outside this project
   and therefore cannot be removed during BH8?
9. Can the old action margin setting be removed cleanly from persisted configs,
   or does it need a temporary hidden load-only alias?

None of these questions requires replacing the PPU or reverse-engineering a ROM
compression stream. They determine eligibility, cache invalidation, rollout and
cleanup scope.

---

## 13. Definition of done

This background-HLE track is complete only when all of the following are true:

- every existing action background use case is represented by one resolved
  per-layer/per-band plan or one explicitly named native fallback;
- eligible world layers render through a pure bounded world provider with no
  ring-size, record-capacity, refresh-cadence or opposite-edge-wrap dependency;
- authentic pixels and priority planes are byte-identical across the complete
  staged matrix;
- live VRAM animation, CGRAM palette changes, HBlank/HDMA, VBlank transitions,
  windows, transparency, priority and color math remain PPU-owned and verified;
- game-visible CPU/WRAM/PPU state is unchanged by the HLE;
- diorama receives exact immutable policy instead of reverse-classifying masks;
- unexpected fallback and mismatch counts are zero in complete action sweeps;
- obsolete action margin-repair and duplicate policy code is removed in BH8;
- the authentic native renderer remains as fallback and diagnostic oracle;
- reference docs and symbols describe the new ownership and the cleanup.
