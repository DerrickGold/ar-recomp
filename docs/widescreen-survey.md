# Widescreen Mode Survey (Phase 2)

> **2026-07-09 correction (user-reported):** the first pass of this survey
> mis-verified several modes as "clean wide" — those screenshots were in fact
> pillarboxed (dark content next to pure-black margins misread visually; only
> the action stage truly rendered margins). Root cause: ActRaiser's sim/menu
> engine keeps a full-width hardware window enabled, and the SNES 8-bit window
> coordinates can't express the margins → everything outside [0,255] hit the
> composite's clip-to-black path. Fixed in the engine (ppu.c
> PpuWindows_Calc): window edges pinned at 0/255 are read as "to the screen
> border" and extend into the active margins. After the fix the verdicts below
> hold for real (re-verified pixel-numerically via simdev.rec replay, and
> always verify margins NUMERICALLY, not by eyeballing dark PNGs).

Catalogue of per-mode behavior with raw symmetric margins (`AR_WS_SURVEY=1`,
16:9 / 43 extra columns per side). Source: user play session
`runs/20260709-045204/snapshots/` (F2 snapshots incl. WRAM/VRAM/OAM sidecars).
Read each snapshot's `$18`/`$19` from its `.wram.bin` (offsets 0x18/0x19).

## Mode table ($18 = main mode, $19 = sub-mode)

| $18 | $19 | Screen | Wide verdict | Evidence |
|-----|-----|--------|--------------|----------|
| 00 | 00 | Title / menu | clean (black BG) but **pillarbox** — $19=00 likely covers other screens (intro etc.) we haven't sampled | snap_00 |
| 00 | 01 | Sim mode, town view | world tilemap fills margins cleanly in static shot; **pillarbox until Phase 3** — needs scroll/map-edge clamp + people-sprite cull/OAM widening | snap_05 |
| 00 | 07 | Sky palace (angel hub) | **WIDE — validated.** Columns + cloud plane extend naturally; menus/dialog centered fine | snap_01, snap_03 |
| 00 | 08 | Temple interior / dialog scenes | **WIDE — validated.** Bounded scene on black backdrop, margins show backdrop | snap_04 |
| 00 | 09 | Mode 7 world map | **WIDE — validated.** M7 plane fills full width; snap_02 shows a few black columns at extreme left at some scroll positions (map edge = backdrop) — acceptable | snap_02, snap_06 |
| 01-07 | * | Action stage (act = $19) | world layers (BG1/BG2 parallax) extend beautifully; **pillarbox until Phase 3**. Artifacts: (a) at level edge (cam_x=0) left margin shows WRAPPED BG1 columns from the far end of the 512-wide tilemap → needs level-bound clamp via PpuSetExtraSideSpace; (b) BG3 HUD rows wrap in margins (repeated left-HUD tiles, the "«««" junk under SCORE) → PpuSetWidescreenBg3Widen(hud_height) or HudSplit; (c) sprite draw/cull windows still authentic (no pop-in observed in static shot; expected when enemies cross ±0/256 while moving) | snap_07 (Bloodpool act 1, $18=02 $19=01) |
| ≥20 | * | Transitions | unsampled — pillarbox | — |

Unsampled yet: intro cinematic, name entry, act-select spin (which $19?),
boss fights, professional mode select, sim menus over town view, ending.
Default policy for anything unsampled: pillarbox (safe).

## Artifact classes → plan phase

1. Level-edge wrapped BG columns (action, presumably sim map edges too) → Phase 3.5 camera/level-bound clamp.
2. BG3 HUD wrap in margins (action; sim HUD rows looked clean in snap_05 — its
   HUD band is backed by... verify while implementing) → Phase 4 HUD policy;
   cheap interim: Bg3Widen(from_y=16).
3. Mid-level tilemap streaming staleness: NOT yet observed (need a walking
   capture mid-stage; Bloodpool act 1 start shows none) → Phase 3.4.
4. Sprite pop-in at old window edges: not observable in static shots → Phase 3.1-3.3.

## Sky palace ($19=07) layer analysis (2026-07-09)

Isolated each BG (AR_WS_ONLYBG=N) + tilemap dumps at dialog/no-dialog frames:
- **BG1** (32-wide): sky + clouds. Repeating pattern, genuinely wide + clean.
- **BG2** (64-wide): pillars AND the dialog/menu box, same layer. Pillars are
  genuinely wide; the box's full-width border rows bleed into the margins.
  PROVEN dialog-only: BG2 margins are 0 bleed cells with no dialog up, 84 bleed
  cells the instant a box appears (gf372 clean vs gf400/560 bleed).
- **BG3** (32-wide): HUD (engine-default clamped).
- Tile-index classification REJECTED: box shares fill tile 0x0ff + 0x0ee/0x11
  with pillars, so fingerprinting misfires.

**KEY DISCOVERY (user-diagnosed): offscreen tilemap staging.** ActRaiser's
UI engine uses BG2's offscreen tilemap columns as a construction/staging area
for dialog and menu boxes — redrawn per UI state (box frames, fills parked in
the columns adjacent to the visible screen; content changes when opening e.g.
the message-speed screen). Invisible on hardware; exposed by widescreen
margins as "extra text box edges" / black slabs. Sampling further out
(margin-gap 48px) proved the offscreen area is staging/scratch ALL the way —
there is NO pillar-continuation content. Detection approaches that failed:
y-band from box-frame tiles (staging is tall side columns, not a band),
tile fingerprinting (box shares tiles 0x0ff/0x0ee with pillars), dispatcher
hook `$02:BF60` (fires sporadically; hle_func can't wrap; geometry varies).

**Sky palace mechanism (traced):** the game double-buffers UI in BG2's
offscreen tilemap half. `$02:ADA8` is the SOLE BG2 tilemap writer — a whole
64x32 map upload (4096 words) from a WRAM compose buffer (`[$76]`, bank $7E),
re-run per UI state. Box-free states (message-speed submenu) leave the pillar
continuation in the offscreen columns; dialog states STAGE A SECOND (taller)
BOX there instead — verified per-row at a dialog frame: offscreen cols hold
box interior fill $011 (rows 5-22) + borders $017/$018 (rows 24-27), not
pillars. So the offscreen pillar tiles DO exist (submenu draws them), they're
just clobbered by box-staging in dialog states. (Correcting an earlier wrong
claim that "no pillar source exists".)

**Sky palace verdict (FINAL, user decision 2026-07-10): plain full-wide,
staging artifacts accepted.** The wide-colonnade first impression outweighs
transient box-staging tiles in the margins during dialogs; every mitigation
tried was worse (whole-layer clamp kills the pillars; dynamic clamps trigger
too often since submenus keep other boxes staged; 48px margin-gap samples more
staging; 32px margin-limit + bottom band rejected). The cache+substitute
approach (`ActRaiser_SkyPalaceWideColonnade`, parked/commented in
src/actraiser_rtl.c) is the most promising future fix but its cache never
warms in normal flows — no fully box-free frame exists; the correct warm
source is the compose buffer at `[$76]` BEFORE box-draw (layout NOT
row-major-64 — quadrant/interleaved, unresolved), revisit in a
game-code-modification phase.

**Engine widescreen primitives built along the way** (all game-agnostic,
inert unless set, reset per frame by the extra-space setters):
- `PpuSetWidescreenLayerClamp(ppu, mask)` — whole-layer 256 clamp.
- `PpuSetWidescreenLayerClampBand(ppu, layer, y0, y1)` — clamp one layer only
  on a scanline band ("BG2.5" overlay; for wide-layer + bounded-UI-band).
- `PpuSetWidescreenLayerMarginGap(ppu, layer, l_px, r_px)` — margins skip the
  first N offscreen pixels (staging strip) and sample beyond (4bpp/2bpp paths;
  for games whose offscreen area DOES continue — check first!).

## Policy implemented after this survey

`ActRaiser_ApplyWidescreenPolicy` (src/actraiser_rtl.c): wide for
$18==0 && $19 ∈ {07, 08, 09}; pillarbox everything else until Phase 3/4.
`AR_WS_SURVEY=1` still forces raw wide everywhere for continued surveying.

## Phase 3: action-stage sprite pipeline (mapped + ported 2026-07-10)

Full chain: `$00:8C98` per-frame cull → `$00:8D68` per-object sprite builder →
`$00:923A` leading entries (from the `$06:A800` bank-6 table) → `$02:ACA6`
OAM DMA (544 bytes: `$0380-$057F` entries + `$0580-$059F` high table).
Ported to C in `src/actraiser_widescreen.c` via `hle_func 8C98
ActRaiser_ObjectVisibilityScan` (bank00.cfg); `8D68` is a static helper inside
the port. Wide branches key off the LIVE PPU margins (`extraLeftCur/RightCur`)
so they are inert (bit-exact) whenever margins are 0 — no separate mode wiring.

**$0380 shadow bit-8 answer** (the plan's "fiddliest detail"): the high table
packs 2 bits/sprite (bit0 = x bit 8, bit1 = size) via `XBA; LSR; ROR $00`
after the 16-bit screen-x store — i.e. bit 8 is taken from the true 16-bit x,
which is ALREADY correct 9-bit OAM encoding for margin sprites (negative x →
$1C0-$1FF region, right-margin x stays in $100-$12A). Composes with the
engine's moved wrap threshold with no extra work.

Port subtleties (bit-exactness traps, all replicated):
- The y store runs `SBC #$0010` with carry CLEAR → stores y-0x11 (x path's
  `SBC #$000F` → x-0x10). Asymmetric on purpose (OAM y is displayed +1).
- x-reject AFTER the y bytes were written must restore `$E080` into the slot
  WITHOUT advancing the cursor.
- The 16-bit y store scribbles the tile byte, which the following tile/attr
  word store overwrites — order matters.
- Builder full (cursor hits $0200) returns carry SET → the cull exits early,
  SKIPPING the final partial high-table flush ($8D5A epilogue).
- `$8F` attr bias: `TSB #$0E00` per object with `$30 & $2008`, `TRB` at every
  builder exit; other `$8F` bits persist across objects.
- `$923A` is invoked from C with the generated paired-call protocol (push
  $8CDD return frame, `host_return_valid=1`, NLR propagation, `S` restore).

Cull windows (wide): x accept becomes `(x0+extL) < 0x100+extL+extR` (same
two-edge unsigned structure as the ROM); builder draw window
`(x+extL) < 0x110+extL+extR`. Y windows untouched.

Still open for Phase 3: tilemap column-streaming margin (locate streamer via
AR_TRACE vram while walking a stage), level-edge clamp (camera $22 bounds →
PpuSetExtraSideSpace), and the action-mode wide policy switch itself
(action stays pillarboxed until those land; the hle wide branches follow the
margins automatically once the policy widens).

## Phase 3.4/3.5 landed (2026-07-10): action-stage margin policy

Measured with saves/level1-action.rec (AR_TRACE vram $6000-67FF, writer =
$02:ADA8 strip DMAs):
- Column streamer uploads a 2-col strip at camera+257..273 every 16px of
  scroll -> the leading margin is only guaranteed ~16px past the authentic
  edge; beyond that = stale/wrapped columns (the visible "streaming" the
  user reported). Trailing content stays resident a full 512px wrap.
- Strips carry FILLER in their top rows (BG1 tile $04E — most rows in sparse
  sections; BG2 tile $18A rows 0-5), hidden behind the opaque HUD on hardware.
- The "«««" row under SCORE is AUTHENTIC HUD art (present in the faithful
  256-wide frame) — only its margin continuation was an artifact.

Policy (ActRaiser_ApplyWidescreenPolicy action branch, $18=01-07):
- Camera-range margin clamp: per-side visible margin = distance to the
  camera-range boundary seen this scroll segment (+16px stream lead once that
  side has led); >64px camera jump resets the segment. Fixes streaming pop AND
  level-start/end wrapped columns with NO per-level bounds data. Margins ramp
  4px/frame (easing, not popping). Applied via PpuSetExtraSideSpace.
- HUD band: PpuSetWidescreenLayerClampBand(BG1/BG2, 0, 48) — filler rows
  clamped, HUD gets black side panels, world wide below y=48.

**White-player "corruption" = PRE-EXISTING cheat interplay, NOT widescreen**:
player always uses sprite palette 7; the hit flash WHITENS CGRAM row 7 and
restores it when invuln ends; AR_NO_KNOCKBACK pins the invuln flag ($08D0
bit $2000) so after the first real hit the restore never fires -> player
solid white ($77BD row) from then on. Proven: old pre-port build wrote the
SAME OAM attrs (pal7); old "red" snapshot predates any hit; faithful headless
run shows the white player too. Fix idea (cheat-side, later): drop the pin
for 1 frame periodically so the game's own restore runs, or pin the invuln
timer instead of the flag.

## Phase 3.4 revision (2026-07-11): the streaming is TWO-TIER

Correction to the earlier note ("the cam+0..46 bursts are tile animation,
not a problem" — WRONG). The action BG streaming has two tiers:

1. **Strip prefetch** — `$02:B158` (now hle'd: `ActRaiser_StreamStripH`,
   src/actraiser_widescreen.c): 2-column strip at camera+256 (right) /
   camera+0 (left), 16px-aligned, fired on each 16px camera crossing;
   marshals into `JSR $BED3` (multiply) + `JSR $B825` (build/upload via the
   `[$76]` record chain). Strip TOP ROWS are filler (BG1 `$04E`, BG2 `$18A`
   rows 0-5 — hidden behind the HUD on hardware). The port widens both
   offsets by ±64px while widescreen is active in an action stage, so strips
   land beyond the 43px margin (invisible arrival).
2. **Visible-window refresh bursts** — 8-column groups sweeping camera+0..62
   over ~3 frames, periodically. These are the AUTHORITATIVE pass: they
   redraw full detail INCLUDING the filler top rows (animated canopy etc.).
   The initiator routine is NOT yet identified (open item — trace the burst
   frames' func channel). Consequence: the LEADING margin only ever holds
   tier-1 strip content -> missing canopy tops / detail tiles there (seen as
   "holes in the spooky tree" in the user's gf4547 snapshot). The TRAILING
   margin is complete because every trailing column was burst-refreshed
   while on-screen.

Also fixed along the way:
- **Level-entry hole**: the initial map draw covers only ~entry+272; widened
  strips start at crossing+320, so [entry+272, entry+336) is never streamed
  going right. The policy grants the leading margin only after 96px of
  progress into the segment (`lead_r/l` gate in ActRaiser_ApplyWidescreenPolicy).
- **Stale-gap ghost strips**: the margin gap blackout is now an unconditional
  per-frame clear of the unrendered edge strips — change-detection was
  insufficient (steady clamps never repainted; ghosts of previous modes
  lingered at the framebuffer edges).
- **NoSpriteLimits wired for real**: the config knob existed but was never
  honored (PpuBeginDrawing ignored render_flags). Now plumbed
  (ppu->renderFlags; gates the 32-sprites and 34-tiles per-scanline caps).
  config.ini defaults it to 0 (hardware-authentic + keeps the faithful pixel
  gate exact); dev-config.ini keeps 1 for enhanced play — wide lines carry
  more sprites and hit the authentic caps earlier.
- **OAM budget ruled out** for the tree-hole corruption: the snapshot at the
  corrupted frame shows 40/128 shadow entries used (check recipe: count
  entries at $0380 with y byte != $E0).

Census closures: `$00:9258` = HUD sprite builder ($06:A800 table, FIXED
screen positions, no camera math, no cull window) — widescreen-safe, stays
centered. `$00:B1BB` = fixed map-ZONE trigger ($05F0/$0220 constants), not
screen culling — leave authentic.

## Phase 3 knowledge collection COMPLETE (2026-07-11)

Per the "collect everything upfront, design once" directive, the whole
rendering engine was mapped in one pass (static disasm of the bank-02 video
core + whole-level faithful traces + user-snapshot forensics). **The
authoritative reference is now [rendering-engine.md](rendering-engine.md)** —
including §13, the design-constraints list the next implementation must
satisfy. Highlights that supersede parts of this doc's earlier notes:

- The "tier-2 burst" initiator IS `$02:B1AF` (row strips, walk-bob-fired);
  there is no third mechanism. All tilemap words flow through four
  one-record buffers (capacity: 1 record/buffer/frame in gameplay).
- The V-strip widened band start was REVERTED ($B8A0 page-keyed decode
  requires 256-aligned spans). Margin fix = host-side record patching or a
  C metatile rebuild for out-of-span columns (§13.2).
- Camera bounds are authoritative in WRAM (`$2E/$30` = level dims; camera
  clamped to `[0, $2E-$100]`): replaces the heuristic 96px camera-range
  margin gate, and correctly explains the level-start black margins.
- Snapshot artifact catalog (runs/20260711-092516) fully explained:
  black staircase = BG1 filler `$04E` from a stale vertical window;
  missing-trunk holes = row-strip span re-wrap on bad camera phases;
  "17-glyphs" = BG2 filler `$17F` (BG2 row strips never fire in act 1);
  those captures did not show OAM exhaustion (<=60/128), while some apparent
  corruption was BG gaps or the known red/white hit-flash player from the
  AR_NO_KNOCKBACK pin. This did **not** close the later sprite-boundary bugs;
  see the 2026-07-12 investigation restart below.

## Investigation restart (2026-07-12)

### Clean baseline: `runs/20260712-100958/` on `main`

- Action mode stays pillarboxed inside the 342x224 framebuffer. The gf=3254
  snapshot shows complete multi-tile enemies and no extra boundary sprites.
- WRAM `$0380-$059F` has 37 live OAM entries, exactly contiguous in slots
  0-36; all 91 remaining entries are parked at x=$80/y=$E0. Two visible enemy
  groups occupy complete six-entry runs (slots 24-29 and 30-35). The separate
  `.oam.bin` capture is only 512 bytes and omits the 32-byte high table, so use
  the WRAM snapshot when checking x-bit-8/size bits.
- Both anomaly captures diagnose only the known benign paired-resume misses
  (`$82ED`, then `$80B4/$8078/$82ED`): zero m/x leaks and zero garbage variants.
  They are the same class seen in the earlier wide runs and provide no sprite-
  corruption signal.
- The console does reveal a real renderer bug: UBSan reports index 5 into the
  four-entry `wsClampY0/wsClampY1` arrays. `PpuWindows_Calc` uses logical layer
  5 for the color-math window, but `PpuLayerExtra` assumed every caller was
  BG1-BG4. Pillarbox usually masks the bad read; wide composition may not. The
  investigation branch now guards BG-only clamp metadata with `layer < 4`.

### Why the earlier branches did not isolate backgrounds

`widescreen-bg` is based on `widescreen-sprites-wip` and still hle-replaces
the whole `$00:8C98/$00:8D68` OAM build, even after reverting its acceptance
windows to authentic bounds. Therefore a sprite symptom on `widescreen-bg`
cannot yet be assigned to BG streaming, priority, or the OAM port. It also
changes `NoSpriteLimits` and cheat defaults, adding avoidable test variables.

The historical source suggests two plausible but not yet capture-proven sprite
mechanisms:

1. Widening the object cull changes `$0400` activation state, so a margin
   object can begin logic or sheet-loading earlier and can toggle at the old
   boundary. This fits extra/junk sprites correlated with clipping boundaries.
2. The per-sprite definition window independently decides which members of a
   multi-tile object reach OAM. An authentic window inside a wide framebuffer
   can legitimately emit only the definitions whose origins cross the old
   boundary. This fits partial groups better than BG priority does. Priority
   can hide pixels, but it cannot remove entries from the OAM shadow.

At the next symptom capture, first inspect the owning object's expected OAM
group. Missing entries means cull/builder/limits; complete entries with missing
pixels means renderer priority/windowing or OBJ character data. Also compare
VRAM `$2000-$3FFF` only after establishing the OAM group result.

### Controlled implementation ladder

1. **Stage A (current)**: raw wide action renderer only. `$8C98/$8D68`,
   `$B158/$B1AF`, OAM DMA, and all game WRAM behavior remain original
   recompiled code. Stale/wrapped BG margins are expected. `AR_WS_ACTION=0`
   returns to the pillarboxed baseline in the same binary. The headless
   `AR_WS_HEADLESS=1` geometry opt-in was restored as test infrastructure only;
   it does not alter the normal oracle/differential path.
2. **Stage B**: restore true-content BG margin refresh only, while keeping the
   original OAM path. Give the refresh its own same-binary off switch.
3. **Stage C**: widen only per-sprite emission for already-authentically-active
   objects. Do not widen `$0400` object activation yet.
4. **Stage D**: widen object activation/cull last, with object/sheet-transition
   logging around the old 256px boundaries.

Stop after each stage for a direct user run. No stage requires a recomp regen
unless a `recomp/*.cfg` change is deliberately introduced; regeneration remains
user-owned.

### Stage A automated gate

Deterministic `saves/level1-action.rec` runs at gf=2500:

- `runs/20260712-111910/shot.ppm`: 256x224 headless baseline (wide headless
  geometry not opted in for that run).
- `runs/20260712-112054/shot.ppm`: 342x224, margins=43/43, stage A raw wide.

Cropping columns 43-298 from the wide frame produces a byte-identical copy of
the complete baseline PPM raster: zero differing bytes/pixels, including every
sprite pixel. The wide frame shows the expected stale/wrapped BG seam outside
the authentic viewport. The run completed without the prior layer-5 UBSan
error; its two action-entry trace captures still contain only the known benign
paired-resume misses. The exit OAM shadow is contiguous (12 live slots 0-11,
116 parked) with no margin-origin entries, as expected from the untouched ROM
builder. This gate cannot reproduce motion-correlated boundary symptoms, so
manual stage-A play plus an F2 snapshot at the exact symptom remains required.
