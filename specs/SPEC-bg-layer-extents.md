# Action background layer extents

Status: implemented and validated 2026-08-10

This work separates the size of the presentation canvas from the area each
action background layer is allowed to occupy. The playable layer may therefore
use a wider or taller canvas without forcing a finite decorative backdrop to
repeat unique art such as Bloodpool's moon.

The contract extends `ActionBgPlan`; it does not replace the background HLE
provider. The provider still selects authentic tile words. Live PPU state still
owns scroll and raster effects, VRAM/CGRAM, windows, transparency, priority,
mosaic and color math. Extents decide only where the already-correct layer may
contribute to the host presentation.

## 1. Coordinate spaces and ownership

There are four distinct bounds. They must not be collapsed into one scalar.

1. The **global canvas** is the area the host asks the PPU/compositor to
   produce. It is shared by backgrounds, objects and fixed-screen overlays.
2. The **authentic viewport** is the SNES 256x224 center. Extent policy never
   removes authentic pixels.
3. A **source bound** says what pixels exist: a finite decoded world, the live
   authentic viewport, or a native PPU tilemap. `ActionBgSourceKind` owns this.
4. A **presentation extent** limits how far a particular layer may contribute
   outside the authentic viewport. `ActionBgPlan` owns this independently for
   BG1 and BG2.

The plan also records each layer's semantic role. A unique `playfield` may own
finite-world horizontal canvas bounds; a special native `scene` may anchor
vertical presentation without pretending to be platform art; `backdrop` layers
never become canvas owners merely because they occupy BG1 or BG2. Invalid,
unclassified or ambiguous role sets fail closed. This keeps canvas policy from
reintroducing a hard-coded PPU layer-number convention.

The extent is expressed as extra presentation pixels on each side of the
authentic viewport. Horizontal values are left/right; vertical values are
top/bottom. They are caps, not requests to manufacture pixels. A 64-pixel cap
on a layer with only 20 pixels of live finite-world space still contributes 20.

Pixels inside the global canvas but outside a layer's resolved extent are
transparent for that layer. They do not paint opaque black and therefore do not
hide a wider playable layer. The normal backdrop/final compositor decides the
visible clear color after all layers have participated.

## 2. Edge strategy and extent are independent

`ActionBgSourceKind` answers which tile-word source is authoritative;
`ActionBgEdgeMode` answers how a pixel outside the authentic viewport is
presented. Those decisions are independent. A verified finite-world source may
therefore bind the HLE provider for authentic tile words while Mirror or Repeat
fills its synthetic span. The extent answers how far that fill may be used.

| Edge | Available extension before an explicit cap |
|---|---|
| `LiveWorld` | Finite world pixels available on that camera side |
| `Mirror` | Reflected authentic scanline to the canvas budget |
| `Repeat` | Cyclic authentic scanline to the canvas budget |
| `RawWrap` | Native PPU result in the live canvas margin |
| `Clamp` / `Transparent` | None |

An explicit fixed extent takes the minimum of the edge-available extension and
the configured cap. It cannot turn `Clamp` into padding. Changing a strategy
and changing its extent are deliberately separate edits in the debug tuner.

For the semantic playfield, the presentation-aware camera uses that same
resolved per-side cap. It never shifts by a larger global canvas budget when
the layer will expose only a smaller fixed span.

The behavior-neutral default is **available**: add no cap beyond the existing
edge/source/canvas rules. Every canonical map initially uses that default, so
introducing the metadata changes no pixels.

## 3. Row bands and precedence

Many SNES backgrounds mix content classes in one hardware layer. Bloodpool
`0201` has unique moon/cloud art above authentic row 136 and repeat-safe water
at rows 136..224. Death Heim `0701` has bounded face/statue art above row 144
and repeat-safe fog below it.

Each layer therefore owns:

- one default edge and horizontal extent;
- one default vertical extent;
- zero or more non-overlapping authentic-row bands, each of which overrides
  the edge and may inherit, remove or replace the default horizontal cap.

Bands use half-open authentic coordinates `[y0,y1)`, are sorted by `y0`, and
must not overlap. Malformed plans fail closed. A row not owned by a band uses
the layer default. Rows in the vertically extended area use the layer default;
bands describe authored authentic scanline families, not arbitrary capture
coordinates.

Horizontal extent modes are:

- **inherit**: band only; use the layer's horizontal extent;
- **available**: explicitly remove the layer cap for this band;
- **fixed**: cap left and right extension independently.

Layer horizontal and vertical extents may be **available** or **fixed**;
`inherit` is invalid there. This distinction lets Bloodpool cap the moon/cloud
rows while its water band explicitly remains full width.

## 4. One resolution seam

Row resolution must be a pure `ActionBgPlan` operation shared by the PPU,
FrameSlot/Diorama handoff, inspector and debug tuner. Consumers must not each
reimplement band precedence or infer policy from PPU masks.

The producer builds the canonical plan, optionally applies an explicit debug
draft, validates it, and publishes that immutable resolved plan with the frame.
The PPU consumes it while rendering. `FrameSlot` copies the same value. Diorama
uses the captured plan and never reads live globals during presentation.

Global presentation overrides such as 4:3 and Wide Raw may replace executed
edge policy. They must also restore behavior-neutral available extents unless
the override explicitly owns an extent; stale per-room caps must never survive
a raw/native projection.

## 5. Debug authoring contract

The developer-only `Layers` section gains a `Diorama` / `BG Extents` authoring
view. The extent view is keyed to the live action `($18,$19)` room even when
Diorama mode is off. It displays canonical and resolved source state and edits
an opt-in draft layered over `ActionBgPlan`.

The view supports BG1/BG2, edge mode, left/right and top/bottom caps, read-only
canonical band intervals with independently editable horizontal caps, visible
guides, canonical/draft A/B, reset, and a normalized plan dump to the log.
Each BG also has independent **ignore side bounds** and **ignore vertical
bounds** A/B shortcuts. The first resolves that plane's default and every
canonical row-band horizontal extent to `available`; the second resolves its
top/bottom extent to `available`. Neither overwrites stored cap edits, enlarges
the shared canvas, or overrides source/edge/finite-world policy. This lets a
playfield fill the expanded Diorama capture while a unique landmark backdrop
remains inside tuned bounds, and switching either shortcut off restores the
exact saved draft values.
Source and semantic role remain read-only. Drafts are not ordinary player
settings and do not share `diorama-layers.ini`. They are disabled by default;
shipped canonical policy remains the only default runtime source of truth.

## 6. Baseline and acceptance

The pre-change baseline is clean `main` at `115c28e` with ROM SHA-256
`b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0`.
The following fresh matrices are local evidence fixtures:

- `runs/bg-layer-extents-baseline-full.json`: Bloodpool `0201`.
- `runs/bg-layer-extents-baseline-0701-full.json`: native-provider control for
  the short Death Heim hub.
- `runs/bg-layer-extents-baseline-0702-0707-full.json`: all provider-backed
  Death Heim rematches at their early 410/420 capture window.
- `runs/bg-layer-extents-baseline-0708-full.json`: native-provider control for
  the final raster arena.
- `runs/bg-layer-extents-baseline-0201-diorama32.json` plus the corresponding
  `0701`, `0704` and `0708` Diorama-32 manifests: compositor handoff fixtures.

Every accepted run reports zero comparator mismatch. Provider-backed runs also
report zero preflight mismatch/outside result. Hub and final-arena controls are
deliberately provider-disabled because those native 32x32 tilemaps have no
eligible finite-world layer.

Behavior-neutral phases must reproduce these baselines. The first intentional
visual policy is Bloodpool `0201`:

1. BG1 can occupy the complete expanded playable canvas.
2. BG2 moon/cloud rows form one bounded contiguous region and never re-enter
   through mirror or repeat.
3. BG2 water rows may continue across the complete canvas.
4. The authentic 256x224 center remains exact.
5. Native PPU effects and layer ordering remain exact.

The first acceptance spelling was a fixed `0/0` horizontal extent on the unique
moon/cloud family plus an explicitly available `136..224` water band. Bloodpool
`0202` uses the same confirmed row family. A zero cap means "no synthetic
extension beyond the authentic viewport," not "hide the layer"; authentic
pixels remain inviolable and the independent playfield/global canvas remains
wide.

Later promoted room policies extend the same catalogue: `0206` uses a 68/68
BG2 cap, `0207` uses 92/92, and boss room `0208` uses Mirror/fill on both
layers with world-backed BG1 capped to 16/16 and viewport BG2 capped to 0/0.
`0208` is the confirming case that finite-world provider ownership does not
imply a `LiveWorld` synthetic edge.

Death Heim acceptance keeps four state classes distinct: pre-ending `0701`
clamp/repeat bands, the ending-sky page handoff, cyclic `0702-0707` rematch
parallax, and the native raw-wrap `0708` raster arena.

## 7. Final validation

The canonical Wide Full twelve-entry matrix accepts all 204 artifacts against
the pre-policy census under the authentic-center contract. Only Bloodpool
`0201/0202` changes: 4,074 framebuffer pixels, all in synthetic side margins;
every authentic center, state dump, PPU snapshot and provider-owned VRAM word
remains exact. Both Bloodpool entries also pass the complete 4:3 and Wide Raw
controls. The complete ordinary-entry matrix passes in 4:3, Wide Raw and
Diorama-32 with zero provider mismatch.

Death Heim `0702-0707` passes in Wide Full and Diorama-32; the Wide Full set
is 102/102 artifacts byte-exact to the frozen baseline. The pre-ending `0701`
controls remain 17/17 exact in Wide Full and Diorama-32. Native `0708` remains
17/17 exact in both modes after its special-room classifier explicitly resets
both raster planes to available extents. That reset is load-bearing: allowing
the earlier generic narrow-BG2 cap to survive removed the starfield from the
side margins even though its final edge strategy was correctly `RawWrap`.

The release build and all 44 tests pass (the display-backed shader test was run
with a real display outside the headless sandbox). Focused action-plan, tuner,
PPU pipeline, settings-overlay and Diorama tests also pass under ASan/UBSan. A
`-Wall -Wextra -Wpedantic -Wshadow` audit reports no warning in the changed
production files.

A pinned `$0347=7` shortcut reaches the real `0701` `$64/$74` BGSC handoff but
stays black, so it is rejected as a visual ending fixture. The natural ending
tail remains useful archival/manual evidence; it is not represented as an
automated pixel-parity gate here.
