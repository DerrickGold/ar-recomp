# Widescreen Mode Survey and Implementation Record

> **Current status (2026-07-12):** the early Phase-2/Phase-3 text below is
> retained as an evidence trail, not the current design. Every action level in
> regions `$18=$01-$06` is directly confirmed fully playable with correct wide
> BG streaming, sprites, activation, and observed raster effects. The shared
> path is enabled for `$18=$01-$07`; Death Heim/`70X` currently reaches its
> first boss arena and crashes. The one
> known presentation gap for `$01-$06` is widescreen-aware camera/world-edge
> clamping so finite map edges cannot enter the margins.
> The current implementation is split between
> `src/actraiser_widescreen_bg.c` and the audited `$8C98/$8D68` HLE seams; it
> does not use the historical monolithic `src/actraiser_widescreen.c` strategy.
> See “Investigation restart” and “Remaining task queue” for authoritative
> status.

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
| 00 | 01-06 | Sim town views: Fillmore through Northwall | **Range enabled; Fillmore directly validated.** BG1 uses asymmetric margins capped to each 512px town world; BG2/dialogs stay center-clamped. Regenerated ADAD/AE6F ports render complete world enemies in the margins, and the horizontal-only `$01:B473` lifetime extension lets arrow record `$0B0A` traverse them. Bloodpool capture `runs/20260714-185817/` proved `$00:$02` and exposed the former Fillmore-only gate; `$02-$06` now share the policy but still require direct content passes. `AR_WS_SIM=0` restores pillarbox and `AR_WS_SIM_SPRITES=0` restores native sprite/projectile predicates. | snap_05 + `$01:B4C6/$B473` static proof + 2026-07-14 Fillmore BG/enemy/arrow tests + Bloodpool mode capture |
| 00 | 07 | Sky palace (angel hub) | **WIDE.** BG1 sky/clouds are clean; BG2 pillars are genuinely wide, but dialogue states expose the game's offscreen BG2 staging. Render-only ROM source-map margin decode implemented 2026-07-12, pending direct validation | snap_01, snap_03; failure evidence `runs/20260712-232230`, 11:31 and 11:36 PM captures |
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
margins as "extra text box edges" / black slabs. Sampling further out during
a dialogue state (margin-gap 48px) proved that state uses the offscreen area as
staging/scratch all the way; merely skipping more columns cannot reach clean
pillar continuation. Detection approaches that failed:
y-band from box-frame tiles (staging is tall side columns, not a band),
tile fingerprinting (box shares tiles 0x0ff/0x0ee with pillars), dispatcher
hook `$02:BF60` (fires sporadically; hle_func can't wrap; geometry varies).

**Observed Sky Palace map states:** dialogue states stage a second, taller box
in BG2's offscreen half — verified per row: offscreen columns hold box interior
fill `$011` (rows 5-22) and borders `$017/$018` (rows 24-27), not pillars. The
message-speed submenu instead exposes a completed BG2 map with clean pillar
continuation. This proves clean background data exists, but does **not** prove
that every dialogue composition has an observable background-only intermediate.
The submenu may rebuild the map or select a different completed page/state.

Static correction (2026-07-12): `$02:ADA8` is the shared **64-byte** VRAM DMA
helper, not one monolithic 64x32 compose upload. Entry/whole-map refreshes are
mega-bursts: `$02:B727` repeatedly calls the original `$02:B825` column decoder
and drains its records inline. `$02:BF60` writes the BG3/HUD compose buffer at
`$7F:B000`; it is not a proven direct BG2 staging writer. The exact submenu
transition (rebuild versus BG2SC/page selection) remains a targeted trace item.

**Historical policy (2026-07-10):** plain full-wide with transient staging
artifacts accepted after whole-layer clamp, dynamic clamp, margin-gap, and
scanline-band experiments all looked worse. The old
`ActRaiser_SkyPalaceWideColonnade` cache experiment was parked because normal
flows never reliably produced a clean frame from which to warm it.

**Failed render-scoped decoder reconstruction (tested 2026-07-12):** in
`runs/20260712-232230`, `[ws-sky]` reported 9/9 reconstructed strips, logical
width `$0200`, scroll `$0000`, and BG2SC `$73`; the captured margins still held
the staged boxes. BG2SC `$73` confirms the expected `$7000` base and 64x64 map,
so this was not an offset or destination-page error. It demonstrates that the
active `$B825` source/config has already switched to the UI-composed completed
state. Re-decoding from it cannot recover an earlier clean palace map. The
transaction was removed rather than retained as dead complexity.

**Renderer-only padding trials (2026-07-12):** reflecting only the first clean
16 raw BG2 pixels beside each authentic edge removed the staged boxes, proving
the renderer-side isolation strategy. The user capture at 11:31 PM also showed
why that source was too narrow: it contained only transparent/cap fragments,
which repeated as broken posts rather than complete columns.

The next isolated-center mirror also failed. The 11:36 PM capture corrected the
ownership model: `$02:BF60` supplies text through BG3, but the visible bounding
box remains on BG2. Reflecting the center therefore copied its side borders
into both margins.

**Direct source-map margin decode (implemented 2026-07-12; validated
2026-07-13):** static ROM evidence at `$02:B6F8-$B726` is a conditional 256-byte
copy from ROM `$07:D0A0` to WRAM `$7E:C200`. The source is a 16x16 metatile page
with the authentic palace beam, capitals, shafts, and floor. Its metatile rows
9-12 contain a box; the box-free scene behind it is reconstructed per column
class (established 2026-07-13 by prediction-vs-live-map diffs, `runs/20260713-*`,
after three staged fixes):

1. Quadrant order is row-major within the metatile (`((y&1)<<1)|(x&1)` =
   TL,TR,BL,BR); the initial x-major read transposed every 2x2 block and drew
   the split shaft metatiles as 8px checkerboards.
2. Rows 9-10 continue the shaft row 8; meta cols 0/15 keep rows 11-12 (the
   page-seam base halves `$42/$40`+`$4A/$48` that complete each center-edge
   half-base); the floor plane's top two rows sit under the box bottom, so
   row 12 maps to floor row 13 at plain columns (the page's own floor rows
   13-15 only cover the lower floor — true in BOTH scroll bands; a
   band-conditional first attempt left a one-row black band in menu state).
3. Pillar base flares exist only in the metatile table, never in a page row:
   `$41/$49` center (the `$41` top half is plain shaft, so the splice is
   seamless) flanked by `$40/$48` / `$42/$4A` skirts at the row-8 shaft
   neighbors — recovered by reverse-lookup of the words the boot-time
   colonnade left in the scratch columns.

The game stages menu UI by rewriting the WRAM `$C200` page copy (rows 2-6)
and re-decoding, so the pristine ROM page is the correct box-free source. The
64x64 map holds four quadrant canvases (2 x-pages x 2 y-bands) selected per
UI state via scroll; the mapping above applies to all of them. F2 VRAM dumps
are taken after the post-scanout restore, so margins must be validated from
rendered pixels or predictions, never from the dump.

For rendering, only BG2 tile columns sampled by the side margins are generated.
The source IDs are expanded through the live `$7E:2900` BG2 metatile table and
the same `$FDFF` mask / `$2000` attribute composition used by `$02:B90D`.
Columns belonging to the authentic center are explicitly excluded. The game
keeps its real center box and offscreen staging; the temporary VRAM map is
restored byte-for-byte after scanout. This remains presentation-only and needs
no generated-code change. `AR_WS_SKYPALACE_BG=0` restores raw-wide output.
**Validation:** the final margin decode is byte-identical to the game's own
boot-composed colonnade (scratch cols 56-63, rows 18-31), and the user
confirmed clean margins in both the dialogue and submenu states.

**Engine widescreen primitives built along the way** (all game-agnostic,
inert unless set, reset per frame by the extra-space setters):
- `PpuSetWidescreenLayerClamp(ppu, mask)` — whole-layer 256 clamp.
- `PpuSetWidescreenLayerClampBand(ppu, layer, y0, y1)` — clamp one layer only
  on a scanline band ("BG2.5" overlay; for wide-layer + bounded-UI-band).
- `PpuSetWidescreenLayerMarginGap(ppu, layer, l_px, r_px)` — margins skip the
  first N offscreen pixels (staging strip) and sample beyond (4bpp/2bpp paths;
  for games whose offscreen area DOES continue — check first!).

## Policy implemented after this survey

Historical survey policy was wide only for `$18==0 && $19∈{07,08,09}`.
Current `ActRaiser_ApplyWidescreenPolicy` grants action margins for the full
`$18=$01-$07` range. BG refresh, object drawing, component emission, and
activation all use that shared range. `AR_WS_SURVEY=1` still forces raw wide
everywhere for continued surveying.

## Phase 3: action-stage sprite pipeline (mapped + ported 2026-07-10)

> **Historical/superseded implementation.** This first monolithic port is the
> branch that failed to isolate sprite state from BG work. Do not use it as the
> template for new code; the controlled Stage A-D1 ladder later in this file
> records the corrected implementation.

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
- **NoSpriteLimits capability exists but ActRaiser wiring is missing.** The PPU
  stores `renderFlags` and can gate the 32-sprite/34-OBJ-tile per-scanline caps,
  but `src/main.c` currently calls `PpuBeginDrawing(..., 0)` regardless of the
  parsed `g_config.no_sprite_limits`. Therefore the config key has no effect and
  the completed direct action validation used authentic scanline limits. Wire
  it deliberately through the runtime-settings work before treating `0/1` as
  distinct test configurations.
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

The controlled ladder resolved the attribution: **background widening did not
break sprites.** The old `widescreen-bg` branch inherited a coupled HLE of the
entire `$8C98->$8D68` OAM path. That port both changed activation/emission at
once and failed to reproduce native exit state exactly: it represented
`PHP/PLP` as host locals and omitted the ROM's two `LSR`s for each unused
high-table slot, leaving the wrong A low byte on the normal exit. The isolated
Stage-B background transaction preserves CPU, all WRAM, math state, OAM, and
OBJ VRAM and has been directly validated. This is why future rendering work
must name the persistent state it is allowed to change and leave unrelated
native/HLE seams untouched.

### Controlled implementation ladder

1. **Stage A (passed direct testing 2026-07-12)**: raw wide action renderer only. `$8C98/$8D68`,
   `$B158/$B1AF`, OAM DMA, and all game WRAM behavior remain original
   recompiled code. Stale/wrapped BG margins are expected. `AR_WS_ACTION=0`
   returns to the pillarboxed baseline in the same binary. The headless
   `AR_WS_HEADLESS=1` geometry opt-in was restored as test infrastructure only;
   it does not alter the normal oracle/differential path. User report: sprites
   remained correct with none of the prior extra/partial boundary symptoms.
2. **Stage B (validated and landed on main 2026-07-12)**: true-content BG
   margin refresh only, while keeping the original OAM and streamer paths.
   `AR_WS_BGREFRESH=0` returns byte-for-byte to Stage A in the same binary.
   Direct user testing confirmed both the wide BG and sprites remain correct.
   The shared path is enabled for `$18=$01-$07`; regions `$01-$06` were directly
   validated at this stage. Death Heim `$07`, blocked by its first-boss crash at
   the time, was subsequently repaired and validated end-to-end on 2026-07-14.
3. **Stage C (validated 2026-07-12)**: widen only per-sprite emission for
   already-authentically-active objects. `$0400` activation remains authentic.
   A full direct-play pass of Fillmore act 1 with `AR_WS_SPRDBG=1` found no
   corrupt, partial, or boundary sprites. `runs/20260712-121751/` recorded
   2,035 intentional wide-only definitions from 30 objects across x=-59..298,
   exercising both 9-bit OAM sides without sanitizer/runtime findings.
4. **Stage D1 (validated 2026-07-12)**: fully margin-resident initialized
   objects draw, while `$0400` and gameplay activation retain the authentic
   boundary. Direct testing confirmed correct sprites and that enemies still
   activate only on entering the old screen space. Stage D2 subsequently
   isolated and widened activation without coupling it back to drawing; the
   final combined path is validated across regions `$01-$06`.

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

### Stage B: audited BG-only refresh

The useful strategy from `widescreen-bg` is retained: build 16px margin
columns with the game's exact `$02:B825->$02:B90D` level/metatile decoder and
copy the resulting column record into tilemap VRAM before scanline rendering.
The sprite-related and state-risking parts are removed:

- no `recomp/*.cfg` changes and no regeneration;
- no `$8C98/$8D68` OAM port;
- no `$B158/$B1AF` hle wrappers—the original generated streamers still run;
- fast vertical motion needs host margin rows at the native 16px cadence: the
  first cached refresher used `$24&$FF00`, producing 75-90px stale intervals in
  `runs/20260712-205507`. The source-only repair reuses `$B8A0` row records at
  `$24&$FFF0` while keeping the expensive `$B825` column window page-aligned;
- complete CPU + 128 KiB WRAM + SNES math-unit snapshot/restore around every
  frame's batch, instead of restoring only `$00-$17/$A5-$A6`;
- fixed-buffer validation (`$3900` BG1, `$3B04` BG2) and per-layer destination
  validation: BG1 writes cannot leave `$6000-$6FFF`, BG2 cannot leave
  `$7000-$7FFF`, so OBJ chars `$2000-$3FFF` and OAM are unreachable;
- authoritative BG1 camera/level bounds prevent the host refresher from reading
  beyond real map data, and steady unused framebuffer gaps are cleared every
  frame. This is a safety clamp on margin data, not yet a widened camera clamp:
  the native 256px camera endpoint can still let a finite background edge enter
  the wider viewport. That remaining presentation task is tracked below.

Deterministic gates using `saves/level1-action.rec` at gf=2500:

- Stage-B-off `runs/20260712-114134/shot.ppm` is byte-identical to the prior
  Stage-A `runs/20260712-112054/shot.ppm`.
- Stage B `runs/20260712-114226/shot.ppm` changes 8,653 margin pixels while
  the authentic center 256x224 has zero differing pixels.
- Stage-A and Stage-B exit WRAM and SRAM dumps are byte-identical (including
  OAM shadow/object state); both complete without ASan/UBSan findings.
- `AR_WS_BGDBG=1` run `runs/20260712-114712/` built 10-14 accepted strips per
  action frame with `(state restored)` and no rejected cursor or VRAM target.

The refreshed capture replaces the stale right-edge forest seam with a
continuous cliff/foliage margin and reconstructs the left-side stump/ground
continuation. Automated gates prove game/OAM-state identity, and subsequent
direct movement/boundary testing confirmed the implementation works without
the historical extra or partial sprite failures. Stage B was fast-forwarded
onto main after that confirmation.

### Stage C: isolated `$8D68`, preserved `$8C98` (validated)

The historical port replaced the whole `$8C98->$8D68` pipeline. Stage C instead
hle-replaces only `$8D68`, leaving the generated `$8C98` object scan unchanged:

- `$0400` activation, object logic, spawn timing, and sheet loading remain tied
  to the authentic 256px window;
- only definitions belonging to an already-accepted object can enter a margin;
- the emission test changes from biased `x < $110` to
  `(x+left) < $110+left+right`, covering screen-x
  `[-16-left, 256+right)`—exactly one maximum 16px OAM tile beyond the visible
  left edge, not the old experimental fixed-64 overreach;
- the ROM's y-$11 store, reject-after-write re-parking, packed x-bit-8/size
  high table, `$8F` bias clearing, OAM-full carry, and RTS ABI are preserved;
- `AR_WS_SPRITES=0` selects authentic bounds in the same regenerated binary;
  `AR_WS_SPRDBG=1` attributes every wide-only emitted definition.

`hle_func 8D68 ActRaiser_BuildObjectSprites` was regenerated and a complete
Fillmore act-1 direct-play pass validated the wide path. The subsequent
host-only activation probe supplied the candidate evidence used by Stage D1.

### Stage D1: widen drawing, not activation (validated)

The full `level1-action.rec` probe in `runs/20260712-122509/` observed 114
margin-candidate intervals across 40 object slots, balanced across both sides.
Thirty-five in-margin handler/definition changes show that many candidates are
already initialized and continue to animate while `$0400` is set.

Stage D1 therefore separates the decisions: authentic visibility continues to
set/clear `$0400`, while authentic-or-margin visibility decides whether `$8D68`
emits OAM. Status gates and vertical visibility remain original, and
`AR_WS_MARGIN_OBJECTS=0` restores authentic draw coverage for comparison.

This requires hle-replacing `$8C98`, but does not reuse the historical port.
That port modeled PHP/PLP only as host locals and skipped the ROM's two LSRs per
unused high-table slot, leaving the wrong low byte in `A` on normal exit. The
audited port keeps the live stack byte and preserves the distinct normal-flush
versus OAM-full exit state. That cfg change was regenerated before the direct
Stage-D1 test below.

After regeneration, direct action-stage testing (`runs/20260712-125747/`)
confirmed complete/correct sprites and the intended gameplay split: enemies
are visible in the margins but activate when they enter the authentic screen.
The run completed without sanitizer/runtime findings.

Two full `AR_WS_MARGIN_OBJECTS=0` replays (`runs/20260712-130051/` and
`runs/20260712-130342/`) completed cleanly with identical SRAM. Raw final-WRAM
identity is not claimed: the two nominally identical fidelity runs themselves
differ in six transient object-position bytes, so this recording is not a
stable whole-WRAM oracle at exit. Use focused OAM/object traces for the next
gate rather than treating a final object-table permutation as scan divergence.

### Stage D2 validated and default-on

Stage D2 widens only the `$0400` horizontal decision and remains independent of
`AR_WS_MARGIN_OBJECTS`. Vertical coverage, status gates, D1 drawing, OAM
emission, and exit machine state are unchanged. Direct Fillmore testing
confirmed correct sprites and activation, so wide activation is now default-on;
`AR_WS_MARGIN_ACTIVATION=0` restores the authentic boundary.
`AR_WS_ACTDBG=1` logs every `$0400` transition with authentic/draw/activation
results, handler, type, definition, span, and margins.

Cheat-matched deterministic runs through game-frame 3000 completed cleanly:
off `runs/20260712-135146/`, on `runs/20260712-135219/`. At gf=2500 the PPM
and SRAM are byte-identical; 15 transient WRAM bytes differ, expected from
earlier object state. Diagnostic run `runs/20260712-135329/` captured real
margin activation (for example gf=1802, slot 33, right span `[297,321]`,
`$0400 1→0` while `authentic=0`, `draw=1`, `active=1`). The user then directly
validated movement, combat, sprites, and boundary behavior with Stage D2 on.
After making Stage D2 default-on, cheat-matched replay
`runs/20260712-191359/` was byte-identical in PPM, SRAM, and WRAM to the prior
explicit-on run `runs/20260712-135219/` at the same game-frame.

## Action-region and asset conclusions (2026-07-12)

- `$00:9557` indexes `$00:95DD` with `$18`; the eight table pointers are
  `$96AF,$A8F6,$B449,$C11E,$CD9B,$D928,$E722,$F39A`. All action regions
  `$01-$07` converge on the same `$8C98->$8D68` OAM machinery. `$07` is Death
  Heim, a user-confirmed no-act boss-rush flow that teleports through the six
  act-2 boss arenas before the final boss. Its exact `$19` sequence still needs
  an instrumented runtime capture.
- `$02:BC9E` loads one common 8 KiB action OBJ atlas from `$07:8000` to VRAM
  `$2000-$2FFF` and sprite palettes `$07:D040-$D09F` to CGRAM `$C0-$EF`.
  It also selects a 256-byte magic overlay rooted at `$06:A400` for `$2D40`.
- `$00:96C3-$96F5` is the previously unknown descriptor-slot-0 armer. Object
  `$38` selects a bank-6 effect source rooted at `$06:A000`; the descriptor
  writes 128 bytes to `$2D80`. This is bounded magic/effect replacement, not
  a general per-enemy sheet allocator.
- Therefore Stage D2's main risk is changing gameplay timing/state for
  `$0400`-gated objects, not exhausting a region-specific sprite cache. Still
  trace `$D0-$D5` and effect selectors around magic/bosses because those are
  the one dynamic action OBJ path now known.

## Town simulation preparation (Phase 4 static map)

Town sprites use a separate bank-1 pipeline:

- `$01:B4C6` follows `$0AEE/$0AF0`, deriving camera `$22/$24` and clamping to
  `$0000-$0100` horizontally and `$0000-$011F` vertically. This proves a
  512×512-pixel town world. `$7F:9F65/$9F67` are transient shake offsets.
- Both `$B4C6` callers run before the behavior/OAM pass. Its faithful
  HLE keeps corrected-wide X in `[extra,$0100-extra]` (16:9:
  `[$002B,$00D5]`) and subjects horizontal shake to the same interval. That
  prevents the wide viewport from reaching the renderer's cleared map-edge
  gaps while keeping BG scroll, OAM, and projectile lifetime on one camera.
  Vertical behavior is unchanged; RAW wide and `AR_WS_SIM=0` remain native.
  The regenerated HLE was directly validated in simulation mode on 2026-07-14.
- `$01:ACD9` rebuilds OAM every frame. It first scans 48 fixed `$12`-byte
  records at `$06A0` with fixed-screen origins, then 44 world `$26`-byte
  records at `$0A00` with camera-relative origins. These are two record
  formats, not one universal object array.
- World render fields are `+08` frame pointer, `+0A/+0C` world X/Y, `+10`
  status, and `+25` delay/timer. The frame definition begins with a count byte
  followed by five-byte components. Record `+0E` is not the tile count.
- `$01:ADAD` and `$01:AE6F` are the leaf composition emitters. `AE6F` uses the
  same geometry but rewrites palette/attribute bits; `$7F:9752 & 2` selects it.
  There is no action-style `$0400` activation decision in `ACD9`: active world
  records are already submitted, and per-component horizontal clipping is the
  widescreen bottleneck.
- The implemented composition seam widens `ADAD/AE6F` only for world records at
  `$0A00+`, using live asymmetric margins. Fixed records must remain authentic
  or UI/overlay sprites may leak into margins. At town map edges cap margins to
  `left<=cameraX`, `right<=$0100-cameraX` and use the same cap for BG and OBJ.
- The BG half of that policy is implemented in `ActRaiser_ApplyWidescreenPolicy`:
  modes `$18=$00,$19=$01-$06` grant BG1
  `left=min(extra,$22)` / `right=min(extra,$0100-$22)`, clears any unrendered
  framebuffer gaps every frame, and clamps BG2. `AR_WS_SIM=0` selects the
  authentic pillarbox in the same binary. The regenerated `$B4C6` HLE prevents
  the native camera endpoints—and therefore the cleared finite-map gaps—from
  entering the corrected-wide viewport. Tilemap, behavior records, and fixed
  OAM remain otherwise unchanged.
- Direct testing on 2026-07-14 confirmed that this BG policy exposes no wrapped
  or stale tiles at town edges. The regenerated sprite stage HLEs ADAD/AE6F to a
  shared faithful port: only record bases `$0A00-$1087` receive the live town
  margins; fixed records and vertical clipping retain the ROM predicate.
  `AR_WS_SIM_SPRITES=0` and `AR_WS_SIM_SPRDBG=1` provide the fidelity/debug
  gates. Direct testing confirms complete enemy compositions in both margins.
- The next symptom was upstream lifetime rather than composition: angel arrow
  record `$0B0A` reaches ADAD, but movement `$B44B` calls single-use `$B473`,
  which releases it outside the authentic horizontal camera window. Its staged
  HLE extends that one interval to the live margins while preserving hard world
  and vertical bounds. Regenerated direct testing confirms both margins.
- A post-validation static pass found no second shared projectile boundary:
  `$B473` is called only by arrow movement, `$B810` only by arrow release, and
  `$B898` already ticks every active `$0A00-$1087` world record. Treat enemy
  shots, construction/lair effects, and rewards as ordinary world records until
  a direct margin symptom identifies a content-specific lifetime gate. Keep the
  separate `$06A0-$09FF` fixed/overlay array authentic unless such a capture
  proves that a world-like effect actually lives there.

For a future decompilation, keep spawn/type identity (`$01:E099/$01:E7D9`),
live behavior/update, frame composition (`ADAD/AE6F`), and graphics upload/VRAM
identity as separate modules. That structure permits replacing assets or the
renderer without silently changing simulation behavior.

## Remaining task queue (execute in order)

Each item is deliberately bounded so it can be implemented, rebuilt, and
direct-tested before the next begins. Regeneration is user-owned: stop and hand
off whenever a cfg/emitter change makes it necessary.

1. **Action Stage D2 — COMPLETE.** D1 drawing is unchanged; wide activation is
   default-on and `AR_WS_MARGIN_ACTIVATION=0` retains the same-binary fidelity
   gate. Direct Fillmore movement/combat and boundary behavior are clean.
2. **All-action policy — IMPLEMENTED and VALIDATED.** The runtime
   enables the shared BG/sprite paths for `$18=$01-$07`. Every ordinary action
   level plus the complete Death Heim boss rush/final boss/return transition is
   fully playable and renders correctly.
3. **General action camera/world-edge clamp.** Map the native camera limits and
   make them widescreen-aware so finite background edges do not scroll into
   view. Recheck representative edges after the camera change.
4. **Dynamic OBJ/effect validation.** Exercise all magic selectors and several
   boss/effect paths while tracing `$D0-$D5`, `$02AC`, object `$38`, OAM count,
   and VRAM `$2D40-$2DBF`. Catalogue selectors to named assets for decomp use.
5. **Town authentic baseline corpus — direct passes pending.** A 2026-07-14
   headless replay audit found that `simdev.rec` and `lairseal.rec` still exit
   cleanly but, against the current SRAM, cover only Sky Palace/world-map
   navigation—not a town viewport or its actors. Capture left/center/right
   town positions, dialogs, builders/people, lair sealing, rewards, and
   multi-actor cutscenes with `AR_WS_SIM=0`; resolve or precisely freeze the
   existing partial-actor symptom as a baseline.
6. **Town BG/map-edge widening — IMPLEMENTED and directly validated.**
   Modes `$00:$01-$06` now derive asymmetric margins from camera `$22` and the
   `$B4C6` 512px bounds, clamp BG2/dialogs, clear edge gaps, and provide
   `AR_WS_SIM=0`. Fillmore confirmed clean clamped edges without wrapped/stale
   tiles; Bloodpool identified and closed the former `$19==01` policy gate.
   The regenerated `$B4C6` corrected-wide camera clamp was directly confirmed
   on 2026-07-14; full `$02-$06` town/special-flow coverage remains in item 8.
7. **Town world sprites — enemy composition and arrow lifetime validated.**
   The regenerated ADAD/AE6F ports widen only `$0A00-$1087` horizontally;
   direct testing confirms complete margin enemies. `$06A0-$09FF` fixed records
   and vertical clipping remain authentic. The horizontal-only `$B473`
   lifetime extension for arrow record `$0B0A` is regenerated and directly
   validated in both margins.
8. **Town matrix, remaining actors/effects, and polish.** Test all six towns at
   left/center/right camera positions; arrows into both margins; enemy shots;
   construction people/effects; lair sealing, rewards, dialogs, pause/menus,
   and transitions. Then audit action HUD side panels, boss HDMA/window effects,
   iris wipes, and unsampled intro/name-entry/ending screens. Treat each as an
   explicit policy choice, not a side effect of widening another mode.

### Cross-region findings: first pass (2026-07-12)

**Milestone resolution:** subsequent direct play completed every action level in
regions `$01-$06`. All are fully playable and correctly render/activate sprites
and backgrounds in widescreen, including the repaired fast fall, previously
inert enemies/platforms, bosses, Aitos cloud bands, and Northwall cloud/snow
bands. The bullets below retain the chronological discovery evidence; their
intermediate “pending retest” states are superseded by this result. Death Heim
and camera/world-edge presentation remain open as described above; Death Heim
is broken at its first boss transition.

- Fillmore act 1: clean. Fillmore act 2: completed, but character movement
  slowed; the action→action F6 path is now the leading suspect rather than
  widescreen. That run entered `$00:B122_M0X0` garbage twice.
- Bloodpool act 1: clean despite its BG2 declaring width `$0100`.
- Bloodpool act 2: gf1915/gf2574 snapshots in
  `runs/20260712-192126/` show camera, object records, and OAM continuing to
  update (29 then 24 live entries). No full right-margin OAM entries exist;
  the frozen sprite-like graphics far into the margin are tilemap content.
  BG2 declares `$32=$0100`, so the host refresh correctly skipped it but policy
  incorrectly exposed its stale offscreen half. Action policy now clamps BG2
  whenever `$32<$0200`; the next run confirmed the clamp. The change is inert on Fillmore
  act 1: replay `runs/20260712-193101/` is byte-identical in PPM, WRAM, and
  SRAM to pre-change `runs/20260712-191359/` at gf2500.
- Bloodpool act 2 BG2 clamp was visually confirmed in `runs/20260712-193357/`.
  The same test exposed a separate recompilation-coverage failure: enemies were
  present and killable but inert, and the next room's platforms did not move
  although their chain tiles animated. Snapshot gf4073 (`$18=02,$19=03`) showed
  active object slots with the `$0400` cull bit clear, ruling out wide activation.
  `dump_dispatch_log.json` then showed live object-loop calls from `$00:8965` to
  `$B990/$B9BC/$BAF1/$BB84/$BCC1/$BCCF`, all `found:0`. `$B990/$B9BC` directly
  update platform Y and `$BAF1` is an enemy tick, so their non-execution exactly
  explains the symptom; the chain animation is an independent tile animation.
  The six roots expand to a 12-entry handler web in `bank00.cfg`; later
  regeneration and direct play confirmed the repair. This failure is independent of the warp and
  widescreen policies—the wider view merely opened the first test of this code.
- Narrow-BG2 presentation follow-up: instead of exposing invalid tilemap data,
  the renderer can reflect the already-decoded authentic BG2 into the margins.
  This is pixel-accurate across sub-tile scroll, preserves the layer's priority/
  transparency/color-math identity, and does not write VRAM. It is now the
  default for action `$32<$0200`; `AR_WS_BG2_MIRROR=0` restores the confirmed
  clamp for the Bloodpool handler retest and for visual A/B comparison.
- Aitos Act 1 (`0401`, continuing through raw maps `$19=02/$03`) also declares
  BG2 `$32=$0100`, but its upper cloud bands exhibit parallax/raster-like
  scrolling (the exact native HDMA/HBlank mechanism remains to be traced).
  `runs/20260712-220525/snapshots/snap_00_gf2960.ppm` proves reflection is the
  wrong presentation there: cloud slope and motion reverse at both 256px
  boundaries. The renderer now cyclically repeats the isolated authentic BG2
  scanline for those three maps instead. This preserves same-direction scroll.
  Bloodpool Act 2 retains its visually successful reflection policy, while
  Bloodpool Act 1 (`0201`) is explicitly hybrid: mirror its static mountain
  band above screen `y=136`, then cyclically repeat BG2 rows `136-223` so the
  animated water keeps the same apparent direction across both seams.
- Northwall Act 1 entry `0601` exhibits the same narrow-BG2 cloud technique.
  `runs/20260712-222626/` records BG1 width `$2E=$0A00`, BG2 width
  `$32=$0100`, and the live `$00:E7BC` -> `$02:945E` -> `$02:96B6` path
  configuring HDMA channel 2 for `$210F` (`BG2HOFS`). The mountains/clouds can
  therefore scroll in scanline bands while the 256px BG2 source wraps; the
  width declaration does not imply a stationary image. Raw map `0605` was
  later confirmed to need the same treatment as `0601`: disabling reflection
  removes its reversed-motion seam. The full observed range `$19=01-$05` now
  shares Aitos's cyclic-repeat policy so internal transitions cannot silently
  restore reflection. Boss map `0608` also uses a parallax-scrolling snow BG2
  and explicitly selects cyclic repeat. Maps `0606/0607` remain on the default
  policy; direct full-level testing found no equivalent seam there.
- Death Heim boss-warp room `0701` is a mixed-content BG2 case, not an ordinary
  level-edge clamp. Offline reconstruction of
  `runs/20260714-174654/snapshots/snap_00_gf1436` from its VRAM/CGRAM dumps
  shows BG1 is the central causeway; BG2 owns both the face statues and the
  animated divider/fog/water. Both layers report width `$0200`, but camera
  `$22=$0000` reduced the left side budget to zero. The selected policy opens
  symmetric margins, clamps BG1+BG2, then cyclically repeats only BG2 screen
  rows `144-223`. This keeps every face in the authentic frame while extending
  the animated lower field from its live rendered scroll phase. Direct testing
  on 2026-07-14 confirmed the centered scenery, clean full-width fog, and
  continuing animation.
- The post-final-boss return to `0701` cannot switch on `$0347=$07` alone.
  `runs/20260714-184728/snapshots/snap_01_gf14676` has `$0347=7/$0334=0` while
  the faces are still visible; `snap_02_gf15031` has `$0334=3` after the
  sky/cloud/water appears. Waiting for `$0334=3` was then observed to release
  the clamp too late in `runs/20260714-185817/`. Static tracing locates the
  exact transition: the `$00:F5C2-$F5E3` fade-to-black and `$F5E4-$F5EF`
  statue-removal wait are followed by BG1SC/BG2SC writes `$64/$74` at
  `$F5F0-$F619`; the fade-in starts at `$F625`, while `$0334=3` is delayed
  until `$F650`. The refined policy requires `$0347>=7` plus live BGSC page
  bases `$64/$74` (with `$0334>=3` as a fallback), clamps only BG1, and mirrors
  the complete live BG2. Reflection removes the hard cloud-edge seam seen with
  cyclic repeat. Direct testing on 2026-07-14 confirmed that the switch is
  hidden by the black frame and completes before the sky fade-in.
- Death Heim raw maps `0702-0707` select cyclic repeat for the moving
  mountain/parallax background.
  In
  `runs/20260714-173750/snapshots/snap_00_gf4875`, the active policy was
  `mirror=02` with BG2 `$32=$0100`; direct observation showed those reflected
  margins scrolling opposite the authentic center on maps `$19=04-$07`.
  `$19=02/$03` are provisionally classified with the same background family.
  The full `$02-$07` range now selects cyclic repeat; direct post-build
  validation remains pending for these maps, especially the provisional
  entries.
- Final-boss map `0708` is separate: both BG1 and BG2 declare `$0100` width and
  form stacked transparent star-road/star-field effects with scanline/sine
  motion (`runs/20260714-183142/snapshots/snap_00_gf12574` and
  `snap_01_gf12654`). Camera `$22=0` previously collapsed both margin budgets.
  Isolated repeat on both layers (`repeat=$03`) filled the margins but caused a
  major slowdown in `runs/20260714-184728/`. BG1SC/BG2SC are `$60/$70`, so both
  32x32 maps already wrap natively every 256px. The optimized policy opens
  symmetric margins and draws both raw (`repeat=$00`), retaining their live
  raster phases without temporary clear/merge passes. Direct testing on
  2026-07-14 confirmed correct visuals and normal performance.
- Full Bloodpool act 2 run `runs/20260712-200334/`: mirror fill is visually
  confirmed. The first handler batch restored early enemies/platforms. Later
  F2 captures found four more live `$12` roots (`$BD82`, `$BD36`, `$BBB4`,
  `$BE0B`), expanding to 12 entries; `$BE0B` is also `found:0` on all 204 boss
  frames in the exit ring. These cfg registrations were pending at this capture;
  they are included in the current generated build and were confirmed by the
  completed region pass.
- **Movement slowdown resolved to draw-side BG refresh cost:** the deliberately
  paired captures in `runs/20260712-202151/` separate the symptom cleanly. Normal
  F9 is host frame 3298/game frame 3318 in map `$19=03`; slow F9 is host 4303/game
  4323 in map `$19=05`. The constant +20 offset proves one game frame per host
  frame—no extra yield—and the player remains on `$9884`, with ordinary position
  deltas while input is held. Host cadence falls from 60/61 to a stable 47 fps,
  while measured `RtlRunFrame` remains only 4.6–5.0 ms. This rules out water,
  collision, warp timing, and player-state logic as the cause.
  Map `$03` has only wide BG1 (BG2 is `$0100×$0100`); map `$05` declares both BG1
  and BG2 as `$0700×$0400`. Its slow dump's block ring is consequently dominated
  by repeated `$02:B825/$B90D/$B914` BG2 record builds (`$3B04/$3B86`). The host
  margin refresher was rebuilding every required BG1+BG2 strip every scanout,
  after `AR_PERF`'s old `run-ms` timer. It now caches the complete aligned-camera/
  map descriptor and rebuilds only at the original streamer's 16px/map-page
  cadence; partial builds retry. `AR_PERF` also emits `[draw-perf]` so the next
  run measures this previously hidden phase directly. This source-only fix does
  not require regeneration.
- The same slow F9 dispatch ring discovered one additional live ungenerated
  object root, `$BD90`; its closure is `$BD90/$BD9F/$BDA5/$BDAE`. It is separate
  from the slowdown—missing dispatches return immediately—but was added to the
  handler batch and confirmed by the completed Bloodpool pass.
- **Bloodpool full retest after regeneration:** the boss and all previously inert later objects
  now work. One enemy remained drawable, collidable, and killable but did not tick. Snapshot
  `runs/20260712-205842/snapshots/snap_00_gf6378.wram.bin` identifies its exact handler as `$BB25`
  (record `$BB19`, `$B449` type `$21`). This is another recompilation-coverage issue, not a
  widescreen cull/activation/rendering fault: `$BB25` contains the object's movement, proximity,
  and yield logic, while the graceful missing-handler fallback leaves drawing/collision intact.
  The root was absent from the original bulk scan because `$B449` has zero holes `$19-$1D` before
  its live `$1E-$27` tail. The corrected static audit also queues Stage-3 table continuations
  `$C48A/$C653/$C90E` before that region's runtime pass, reducing regeneration rounds.
- Stage-3 act-1 capture `runs/20260712-211830/` adds exact runtime roots
  `$C7FA/$C7FF/$C804` and yield continuation `$C80A`. Six live objects hold those roots and the
  exit dispatch ring counts 94 misses per root. They are object-handler coverage, not evidence of
  a new widescreen rendering failure; the completed Kasandora pass later confirmed them.
- **Regions 4-7 static preflight:** both acts in a kingdom share one `$18` handler table, so a
  complete sparse-table walk covers every declared object type regardless of which act spawns it.
  Aitos, Marahna, and Northwall add no state/yield gaps beyond the table entries already queued.
  Death Heim's distinct `$F39A` flow adds 13 primary `$12` continuations, each immediately after
  `JSR $86FA`; all are included. With one analogous Stage-1 `$A66A` continuation, the generated
  batch is 43 unique handlers and all table/field-`$14`/literal-`$12`/yield scans report zero gaps.
  Runtime playthroughs subsequently validated regions `$04-$06`; Death Heim
  remains the only broken/unvalidated region table.
- **Fast vertical stale rows isolated and repaired:** in Stage 1 Act 2 run
  `runs/20260712-205507`, the opening fall advances about 5px/frame while the
  cached host refresh fires at gf1885, 1903, 1918, 1933, and 1949—15-18-frame
  (roughly 75-90px) gaps. The full `$B825` margin-column window must remain
  `$24&$FF00`; changing it to 16px cadence would redo 18 columns every few
  frames and risk restoring the draw slowdown. The source-only fix instead
  calls the already-emitted `$B8A0` row builder at `$24&$FFF0`. Its current
  page record is drained normally; if a margin lies outside that 512px band,
  the neighboring page is decoded but only the missing margin columns are
  copied, preserving the opposite half of the aliased 64-column ring.
- The console's lone `$8465` `4210-wedge` was a false diagnostic accumulated
  across 4,096 ordinary once-per-frame ACK reads, not a yield; the detector now
  requires adjacent block-ring reads.
