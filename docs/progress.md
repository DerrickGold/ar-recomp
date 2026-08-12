# ActRaiser Recomp — Progress Tracker

Tracks playability by action stage and simulation town, plus cross-cutting
subsystems. This is the authoritative project-status document.

Mark a row ✅ or 🟡 only after a play-test, not because the code should work.
Use ⬜ for untested paths and record the date and scope of each verification.

**Legend:** ✅ Confirmed working · 🟡 Playable with known issues (see note)
· 🔴 Broken/blocking · ⬜ Untested

## Action-widescreen milestone — 2026-07-12

Every action level in regions `$01-$06` has now been played through and is
confirmed fully playable with widescreen enabled. Across those passes:

- BG streaming, fast vertical traversal, sprites, old-edge activation, enemy and
  platform behavior, bosses, room transitions, and the observed HDMA/parallax
  effects render and behave correctly;
- the historical extra/partial sprite corruption did not recur after the BG
  refresher was isolated from emulated WRAM/OAM state;
- decorative 256px BG2 layers use stage-appropriate presentation policy:
  cyclic repeat for Aitos and Northwall cloud/snow raster effects, explicit
  clamp/raw exceptions, and independent extents that keep Bloodpool's unique
  upper art bounded (`0201` tuned to 76/100, `0202` and unbanded `0206` to
  68/68, and unbanded `0207` to 92/92); boss room `0208` mirrors BG1 within a
  16/16 playfield cap and holds BG2 to 0/0; `0201` water remains available while
  `0202` water inherits its backdrop span;
- the handler-coverage batch restored the inert objects discovered by the wider
  playthroughs. Those were pre-existing recompilation gaps, not widescreen
  regressions.

2026-08-12 entry-balance correction: Stage D1 drawing and Stage D2 activation
remain independent. During player arrival handlers `$97A6/$97C9/$97E4`, only
the additional horizontal `$0400` activation range is held to the native camera
reconstructed from `$02:B030` semantics; wide object drawing, fitted camera and
BG presentation stay enabled. `$97E4` installs input-reading `$9832` and wide
activation resumes on that scan. The first attempt instead put a zero-margin
256px window at fitted `$22=120`, shifting Fillmore's native `[0,256)` object
set to `[120,376)` and causing an intro fade regression. Corrected replay
`runs/20260812-122927` preserves `$97A6→$97C9→$97E4→$9832`, has no second
Fillmore music stop/restart, and the suite passes 45/45; live visual acceptance
is pending. See bug-ledger.md §58.

Death Heim (`$07`/`70X`), the distinct boss-rush/final-boss flow, is done:
user-verified end-to-end on 2026-07-14, playing through every boss to the end
(see the region table). Finite action-world side margins now clamp from the
role-selected playfield camera/dimensions. The BH7-accepted background HLE is
now default-on and supplies finite BG1/BG2 tile words across eligible authentic
world layers and side/top synthetic margins after phase, source, bounds, and
live-ring preflight. Exact ring words permit full-layer ownership; an in-world
contradiction retains the native authentic centre plus finite provider margins.
Decorative layers remain native mirror/repeat/clamp sources. BH6 now carries
that same per-layer/per-band
plan through the post-scanout `FrameSlot` handoff: Bloodpool and Death Heim
bands reach diorama exactly, with no scalar PPU-mask reclassification. Five
presentation matrices, long Full/Raw/diorama replays, a natural Death Heim
transition soak, lifecycle gates and a real compositor A/B pass. BH8's
behavior-neutral repair and dead-policy cleanup is now complete. The 2026-08-10
role/extent catalogue separates global playfield growth from per-backdrop
bounds and is validated across 4:3, Wide Raw, Wide Full and Diorama-32.

No generated ROM-derived source is committed; reproducible builds materialize
the registered handlers from `recomp/*.cfg`.

## Diorama play-testing milestone — 2026-08-11

The following routes have now been user-played to completion with Diorama
effects enabled, including their action-stage transitions and town presentation
where listed:

- Fillmore Acts 1 and 2 plus the complete simulation round;
- Bloodpool Acts 1 and 2 plus the complete simulation round;
- Kasandora Acts 1 and 2 plus the complete simulation round;
- Marahna Acts 1 and 2 plus the complete simulation round.

Aitos Act 1 and its transition into simulation mode are also user-confirmed;
the rest of its simulation round and Act 2 remain pending. This is a play-test
statement, not an inference from shared code. Northwall and Death Heim
Diorama-specific completion remain unverified unless their rows below say
otherwise.

## Action stages — observed status

ActRaiser's 6 kingdoms each have two action (side-scrolling) stages, played
between rounds of that kingdom's sim-mode town. Region/act numbering below
follows the game's internal order (`$18`/`$19` in WRAM — see `docs/SEAMS.md`
"Game-state anchors"), region name order from the town-dispatch table at
`$03:F5ED`.

| Region | Act 1 | Act 2 |
|---|---|---|
| 1 — Fillmore | ✅ Full widescreen playthrough; sprites and activation clean (2026-07-12); complete playthrough with Diorama effects (2026-08-11) | ✅ Full widescreen playthrough; fast vertical fall/row streaming confirmed repaired (2026-07-12); complete playthrough with Diorama effects (2026-08-11) |
| 2 — Bloodpool | ✅ Full widescreen playthrough; narrow BG2 policy clean (2026-07-12); 2026-08-10 moon/cloud bound + wide water/playfield matrix accepted; complete playthrough with Diorama effects (2026-08-11) | ✅ Full widescreen playthrough; enemies, moving platforms, and boss confirmed (2026-07-12); 2026-08-10 shares the accepted moon/cloud/water extent split; complete playthrough with Diorama effects (2026-08-11) |
| 3 — Kasandora | ✅ Full widescreen playthrough and generated handlers confirmed (2026-07-12); `0301/0302` world-anchored Mirror-cloud/Repeat-dune policy accepted at `128/128`; complete playthrough with Diorama effects (2026-08-11) | ✅ Full widescreen playthrough (2026-07-12); complete playthrough with Diorama effects (2026-08-11) |
| 4 — Aitos | ✅ Full widescreen playthrough; cyclic BG2 cloud padding removes the parallax seam (2026-07-12); Act 1 and the transition into simulation mode confirmed with Diorama effects (2026-08-11) | ✅ Full widescreen playthrough (2026-07-12); Diorama-specific completion pending |
| 5 — Marahna | ✅ Full widescreen playthrough (2026-07-12); complete playthrough with Diorama effects, including main/subscreen colour mixing and cyclic BG2 across `0501/0502` (2026-08-11); `0505` BG2 promoted to Repeat/fill with a fixed `128/128` backdrop extent while BG1 remains available | ✅ Full widescreen playthrough (2026-07-12); complete playthrough with Diorama effects (2026-08-11) |
| 6 — Northwall | ✅ Full widescreen playthrough; cyclic BG2 cloud/snow padding confirmed across the affected maps (2026-07-12) | ✅ Full widescreen playthrough and boss completion (2026-07-12) |
| 7 — Death Heim | ✅ Full boss-rush playthrough to the end — entry, every boss fight, victory teleport-outs, hub warps, and the final boss all user-verified (2026-07-14) | — No Act 2 |

Static code confirms `$18=$07` selects its own handler table at `$00:F39A`.
The game structure was confirmed on 2026-07-12: Death Heim has no ordinary acts;
it teleports through all six act-2 bosses, then transitions to the final boss.
The 2026-07-14 repair (DEBUG.md §7.20 / docs/bug-ledger.md) fixed one crash and
one silent soft-lock, both unregistered yield-helper continuations; the `$19`
flow, derived from code and confirmed through the boss warps, is: `$19=1` hub →
bosses at `$19 = $0347+2` (progress counter `$0347 = beaten map - 1`, written by
`$00:FEEC`, which also stages the hub warp via `LDA #$0701; STA $1A`) → `$19=8`
final boss (sets `$0334=1`). The all-six-regions-complete path
(`$00:A343` checking `$7F:6B18`) is the separate post-rush exit.

### Residual action-mode completion gate

The broad region `$01-$06` matrix is complete. Residual action work is:

1. ~~Map the native camera clamps and add widescreen-aware left/right limits so
   finite background edges never enter the visible margins.~~ Done 2026-08-10
   at the canonical `$02:B091` seam, with per-axis native fallback. Wide
   streamed `0202`, its 256px BG2, vertical Diorama-32, 4:3/Raw controls, and
   undersized `0703` all pass with zero background-HLE mismatches. The shared
   plan/provider gate also keeps authored hub `0701` and the explicit
   provider-off control on native camera behavior.
2. ~~Diagnose the `0701` crash after its first boss teleport~~ — done 2026-07-14
   (yield-helper continuations; docs/bug-ledger.md #20), and the full rush
   through the final boss (`$19=8`) is user-verified. There is no ordinary
   `0702`. Optional residue: the all-six-regions exit variant (`$00:A343` over
   `$7F:6B18`) fires only on a save with every kingdom's act 2 complete —
   exercise it during a full-game playthrough.
3. Representative corrected/4:3/Raw and undersized-room camera gates pass after
   the camera change. A lifted-limit A/B remains optional only after
   `NoSpriteLimits` is actually forwarded to the PPU render flags.

For Death Heim or any future anomaly:

- play from start through boss/transition, exercising both old 256px edges;
- take F2 snapshots at a dense encounter, a room/route transition, and the boss;
- exit/F9 while any object is inert or behavior is suspect so the 1,024-event
  dispatch ring retains the onset; use `AR_DISPMISSALL=1` only for a focused
  follow-up because ordinary `$896F` loop returns are benign noise;
- feed snapshots to `tools/find_handler_chain.py --snapshot ...`, and compare
  non-benign `found:0` roots against generated `bank_00_*` entries;
- record visual BG/sprite/activation results separately from handler coverage.
  A drawable or killable but inert object is usually a missing behavior handler,
  not proof of a widescreen rendering fault;
- current ActRaiser builds always use authentic scanline limits because the
  parsed `NoSpriteLimits` field is not forwarded to `PpuBeginDrawing`; wire that
  setting before attempting a lifted-limit A/B comparison;
- if a symptom might be caused by the action→action warp, reproduce with
  `AR_WS_ACTION=0` or natural progression before classifying it.

Testing caveats: recorded input needs the same cheat state used when it was
captured, and `AR_NO_KNOCKBACK=1` suppresses authentic water drag.

## Simulation mode — town verification matrix

| Town | Status |
|---|---|
| Fillmore | ✅ Clean full round (act1 → sim → act2, 2026-07-07): development cycles, story events (rock zap/fire), lair sealing with all cutscene actors, reward grants (scroll persists), offerings; complete simulation replay with Diorama effects (2026-08-11) |
| Bloodpool | ✅ Complete simulation round with Diorama effects, including development, events, lairs, and transition coverage (2026-08-11) |
| Kasandora | ✅ Complete simulation round with Diorama effects, including development, events, lairs, and transition coverage (2026-08-11) |
| Aitos | 🟡 Act 1 → simulation entry confirmed with Diorama effects (2026-08-11); complete development/event/lair pass and transition to Act 2 remain pending |
| Marahna | ✅ Complete simulation round with Diorama effects, including development, events, lairs, and both action-stage transitions (2026-08-11) |
| Northwall | ⬜ Full authentic baseline needed before any widescreen town work |

The sim-mode *engine* itself (town dispatch, spawn/behavior systems — see
`docs/SEAMS.md`'s "Sim-mode town architecture" section) is shared code across
all 6 towns, so a fix verified in one town likely applies everywhere — but
"likely applies" isn't "confirmed." Aitos remains partially verified and
Northwall remains ⬜ until someone actually plays it.

For **each** incomplete town, the baseline pass must cover:

1. act-1→sim entry and initial population/state;
2. camera at left, center, and right map edges;
3. builders, people, monsters, lairs, and ordinary development cycles;
4. town-specific disaster/story events and their multi-actor cutscenes;
5. offerings/rewards and persistence of granted items/stat upgrades;
6. lair sealing, dialogs/temple, Sky Palace staging (margin decoder directly
   validated 2026-07-13 — byte-identical to the boot colonnade; re-check per
   town only if a palace state looks off), and sim→act-2 transition;
7. an exit dispatch ring plus F2 snapshots around any partial actor, silent
   event, frozen builder, or missing reward.

Run these authentic-geometry baselines before changing town rendering. The
existing partial-town-actor symptom must be captured as a baseline rather than
silently attributed to future widescreen code.

## Remaining validation roadmap

The settings overlay and save editor are implemented through Phase 6. Automated
codec and backend tests pass; the remaining settings gate is a representative
Apply and save → Restart Game → Continue matrix across the save-editor pages.
See [settings-system.md](settings-system.md) for the implementation record.

The remaining project-wide validation work is:

1. **Complete simulation-town baselines.** Finish Aitos from simulation entry
   through Act 2, then capture Northwall's authentic baseline. Use
   `AR_WS_SIM=0` for the authentic-geometry comparison.
2. **Accept recent action effects visually.** Recheck Bloodpool, the
   materializing sword beam, Marahna, and Aitos against their captured evidence.
3. **Polish shared presentation.** Audit HUD side panels, dialog staging, boss
   effects, intro/name-entry/ending screens, and framebuffer-gap clearing.
4. **Run the release matrix.** Recheck every action stage, every town, and the
   major non-action screens with enhanced presentation enabled and disabled.

Action-background HLE, simulation background widening, and simulation world
sprites are implemented. Their detailed acceptance evidence lives in
[bg-hle-census.md](bg-hle-census.md), [widescreen-survey.md](widescreen-survey.md),
and the status tables below rather than being repeated here.

## Major functionality

| System | Status | Notes |
|---|---|---|
| Boot / title screen | ✅ | |
| Save / load (in-game state) | ✅ | Checksum-gated continue path confirmed (`AR_SAVECHECK`) |
| Action-stage combat | ✅ | Every ordinary action level plus the complete Death Heim boss rush, final boss, and return transition is fully playable. Open `$00:B8AB` spell-projectile garbage variant (`DEBUG.md` #19) remains a separate unconverted-code edge case. |
| Magic casting | 🟡 | WORKS as of 2026-07-07 (was dead — blocked by our own knockback cheat, `DEBUG.md` #18). 2026-08-02: all four spells' controller/slot lifecycles, animation banks, timing, transforms, and composition geometry are statically mapped in `effects-hook-investigation.md`. 2026-08-03: host-polish lighting/particles land, initially Magical Fire only — and only after a 16-bit read of the 8-bit animation-bank field was found to have rejected every spell since the feature was written (bug-ledger.md §32). 2026-08-05: generalised to a DATA-DRIVEN rule table (controller kind -> slots/roles/phases) covering all four spells, with per-part stage styling, overlap clustering, heading-oriented bodies, and three ember modes (rise/trail/burst). Fire and Stardust are MEASURED against live WRAM and confirmed rendering; Aura and Light are still TRANSCRIBED from the static map and unproven — an unrecognised cohort slot is reported to `[action-fx census]` rather than rendered on a guess, so one cast of each corrects the table. Test harness is Cheats > Cycle magic spell (default `M`). Known and accepted: Stardust's right-edge launch is born one pixel outside the authentic window and as low as the player's line, which widescreen makes visible (bug-ledger.md §33). |
| Action scene lighting / particles | 🟡 | 2026-08-10: Bloodpool BG1 torch pair `$47/$4F`, enemy fireballs, vertical lightning traps, the map-$08 boss lightning attack, and the global player sword beam publish a separate read-only scene-effect frame and render in both flat and Diorama action presentation. Actor matching uses measured map/handler/resume/source/state plus exact live visual/composition pairs; run `20260810-180202` and a complete `$7E:5000` decode identify six boss strikes (vertical/diagonal × long/medium/short), visual `$20` as their blank half-cycle, and a separate state-`$09` floor impact. Runs `20260810-175403` and `20260810-184935` identify both sword-beam cycles and correct signed-origin geometry to four explicit state/direction OAM rectangles. Run `20260810-190012` shows the aligned five-glint revision was too faint, while `20260810-190729` shows the denser centreline remained too narrow. The current pass derives full-height haze from the crescent and fixes forty-eight stars at sixteen top/centre/bottom cross-sections from 4px to 88px behind it; independent 18-tick alpha/scale phases make them materialize instead of streaming. Portable SDL additive geometry follows BG1, BG2, or OBJ projection and needs no backend-specific shader. Follow-up fixes carry the Diorama OBJ-apron texture origin, accelerate synchronized torch accents, cover the full 176px trap shaft, and make Bloodpool boss filaments follow exact authored OAM row centroids. Runs `20260811-151353`, `20260811-221433`, `20260811-225534`, and `20260811-232640` add and refine Marahna `$05/$04-$08`: camera-windowed single-`$43` torches including ten in the boss room, the complete `$E047` left/idle/right orb cycle and four cardinal children, parent-validated `$DE96` snake fireballs, validated horizontal/vertical endpoint-linked lightning, and the complete `$E483` boss hand-charge, central-orb, diagonal-bolt, and three-frame ground-charge lighting cycle. The later runs correct `$34/$4BE5` to moving platforms and `$E0BA` to a non-fire reaper orb; both fail closed. Aitos `$04/$01` now covers the complete `$DC/$DD+/$DE` rim over `$DF/$E7`, exact `$CF9E/$CFCD` fireballs across states `$22-$24`, and separate `$CEEC/$CF16` launched molten rocks without decorating stationary mouths. The full bubbly volume retains its glow, while `20260812-105106/snap_00_gf3728` corrects spark births to a narrow band one quarter-height above its geometric midpoint, matching the isometric surface. Run `20260812-000613` adds exact three-row waterfall-platform identification in `$04/$02-$03`, BG1 spray/drips, one restrained BG2 veil with 48 slow flow streaks, paired bottom foam/mist covering the finite-backdrop gap, and the `$04/$03` dragon's two priority-2 diagonal sword crescents through an exact inactive-controller/live-root lifecycle. Map decorations have a separate 16-record budget, proven against the measured 14 splashes plus one veil and one mist while retaining all 16 actor slots. The veil composes at BG2 depth—after BG2-low in Diorama and through a flat PPU BG2-winner mask—so BG1 platforms and actors remain in front; the bottom atmosphere submits unmasked from Diorama’s after-BG2 callback while retaining BG2 projection and leaving later BG1/OBJ planes in front; flat mode has no vertical extension gap. One-time success logs expose both veil routes and the atmosphere submission. Drawable-plane publication and drawing share one eligibility contract with paired UV/shape metadata. Explicit geometry budgets cover five Marahna connector links, one boss bolt, the measured three simultaneous sword streams, one waterfall veil, and one paired mist bank. Production flat/Diorama regressions verify independent BG1, BG2, and OBJ projection remain aligned; fresh visual acceptance remains for Bloodpool, the materializing sword beam, Marahna, and Aitos. |
| Sim-mode town simulation | 🟡 | Fillmore, Bloodpool, Kasandora, and Marahna ✅ end-to-end with Diorama effects (2026-08-11); Aitos Act 1 → sim entry is confirmed with the remainder pending; Northwall still needs its baseline. Reward web and multi-actor cutscenes fixed 2026-07-07 (`DEBUG.md` #18b/§7.17). |
| Scroll/MP persistence | ✅ | `$0295` persistent / `$21` working-copy model mapped + grant verified across modes (2026-07-07) |
| Audio (music/SPC) | 🟡 | SPC upload handshake and boss-music playback fixed; a narrower "voice/SFX key-on" gap was reported and its current status isn't confirmed — verify before marking ✅ |
| Music replacement (OGG streaming) | 🟡 | 2026-07-16: manifest-driven OGG streaming live (`[music:]` in `game-assets/manifest.ini`, all 17 song-table entries enumerated): port-0 play/halt protocol decoded, srcn>=0x0C DSP voice gate keeps SFX authentic, sample-accurate loops (LOOPSTART tags), `when =` variant gates. 2026-07-25: fixed the intermittent boot race that cleared the song-instrument base after the common upload and shifted music into unmuted srcn 00-0B; 20/20 fast-boot stress runs reached the common upload only after SPC bootstrap idle, and 10/10 paced runs keyed title srcn 0C. Pending: in-game listening pass, per-src identification of the 16 unnamed songs, driver fade capture. |
| Mode 7 (world navigation) | 🟡 | 2026-07-27: native camera/focus contract, act-entry spin/zoom, Palace/UI OAM composition, location selection, developed-map builder, and four-frame water upload are mapped. Optional `AR_SIM3D_WORLD_NAV=1` owns the full world as a forced-top-down 3D scene and remains enhanced across the complete INIDISP fade. Pending: complete manual movement/destination and action-entry acceptance sweep. Other Mode-7 screens remain separate work. |
| Input | ✅ | Every joypad button is re-bindable for keyboard and gamepad independently (Settings → Controls; `bind_key_*`/`bind_pad_*` in `settings.ini`, keyboard stored by SDL scancode so layout changes follow the keys). Gamepad support includes multi-pad selection with hotplug, `gamecontrollerdb.txt`, left-stick-as-D-pad with deadzone, right-stick/trigger camera control, and six pad-bindable host actions so the menu (including rebinding) is reachable with no keyboard. Steam Deck works via Steam Input and via SDL's HIDAPI Steam driver from desktop mode. Reference: `docs/manual.md` "Controls". Consumer side fully mapped (SEAMS "Input" + "Magic system") |
| Runtime settings overlay | ✅ | Phase 5 complete: global Escape/F1 access, hierarchical category/direct-action navigation, independently scaled three-panel native dialog theme, ROM-decoded font/frame atlases, frozen-game input capture, validated editing/actions, and atomic `settings.ini` saves. Phase 6 includes the guarded Save editor category and codec actions. A native game-menu entry is optional; gamepad support has since landed (the pad drives the menu using the player's own bindings — see the Input row). 2026-07-21: new Graphics category added for the diorama GPU-shader effects below. |
| Diorama 3D presentation mode | 🟡 | `ar-recomp-threading-impl.md`'s full plan shipped 2026-07-20/21: action-stage layers render as a tilted 3D shadowbox (interactive camera, per-layer toggles), a fixed 60.0988Hz game-tick loop, and optional GPU shader effects (rim lighting + depth-of-field/edge-AA, live-verified) reachable via Settings → Graphics. Scroll interpolation remains off by default pending in-game acceptance of its IJ1 unit fix; it no longer reads HDMA-polluted scroll state. Soft shadow blur also remains off because it can bleed onto transparent gaps in the layer behind it. Per-room presentation is still being authored: `diorama-layers.ini` carries the depth/alpha/rake overrides for each area, and rooms without a tuned entry fall back to defaults that can read flatter than intended — an ongoing visual-tuning pass, not a defect. 2026-07-23: the act-title card and pause text (BG3 rows below the HUD split) now ride the composed flat HUD overlay instead of being buried behind the tilted scene planes — see rendering-engine.md §13.1. Simulation presentation uses independent profiles below rather than the action-stage camera. 2026-08-05 (branch `vertical-extend`): a **vertical band** (`diorama_vertical_extend`, initially 0-32 scanlines, **default 0**) renders real world above the authentic viewport. 2026-08-10 makes it symmetric and raises the per-side ceiling to 64: top/bottom resolve independently against the primary finite world, every BG receives its own top/bottom clip, exact signed OBJ positions allow actors on either side, and FrameSlot/Diorama carry the complete `top+224+bottom` capture. Repro `runs/20260810-112529` proved camera/player Y both moved 48px while the old bottom remained zero, culling a still-resident lower platform. The Layers tuner now has independent side-bound and vertical-bound bypasses per BG. The default remains 0 pending a complete manual action-stage sweep. 2026-08-09 corrected the Fillmore act-2 red-band diagnosis: it was not spatially off-screen art but bottom-of-tilemap BG2 data wrapping into negative synthetic rows while BG1 legitimately extended. `PpuSetVerticalMarginLayerClip` bounds BG1/BG2 independently; the gf-2200 replay removes BG2-high with byte-identical WRAM. Design and diagnostics are rendering-engine.md §13i. 2026-08-06: the horizontal transpose landed as the **OBJ apron** — captured OBJ planes carry 64 columns of RESOLVE headroom per side, so a sprite straddling the display edge is rasterized whole instead of being abandoned mid-write. Those columns are deliberately NOT displayed: the scanline buffer is now 512px with a 120px/side live cap, while the 64px apron remains a separate resolve-only surface band. Clipping at the shown edge therefore remains correct; the benefit is that the DOF/edge-AA/rim shaders stop sampling past the content, and it is the machinery the sim synthetic part channel needs. Real OAM is never widened — out-of-window parts stay parked and ride a host part channel carrying exact positions. `kPpuObjApron = 0` is the A/B lever. See rendering-engine.md §13j and ledger §35/§36. 2026-08-11 Marahna established the missing main/subscreen contract: visual capture uses `TM | TS`; measured disjoint full-add state `TM=$06`, `TS=$11`, `CGWSEL=$02`, `CGADSUB=$03` is reproduced with PPU-resolved sparse TS planes and an immutable additive-plane handoff. Its 512px BG2 is independently classified as a decoded horizontal cycle whenever a wider BG1 shares camera X, covering `0501/0502` without a subsection allowlist. |
| Simulation / world-navigation 3D | 🟡 | Town 3D provides the projected ground, full-town canvas, developed-world underlay, semantic billboards, height/shadow/rim-light stages, cloud shroud, haze, and atmospheric backdrop. 2026-08-03: frame-owned effect emitters now cover the Lightning miracle, Blue Dragon lightning, the world-tier pair of new-town creation strikes, palette-1 Red Demon fire, both palette-1 red / palette-2 blue variants of the shared ground-fire animation, and the separate scripted burning-house `$0A01/$01:A838/$DD2D-$DD39` family with portable batched additive lighting and deterministic burst/rising particles. Run `20260803-130945` frame 19,950 proves three concurrent house fires and their `(8,16)` ground contact; a signature-checked visual-data patch slows their one-tick source frames to four ticks without putting mutations in renderer or metadata callbacks. Run `20260803-133014` corrects the new-town hook: the `$01:A8BB` actors are world process `$000E` records retaining `$A8BB` in raw `+$06`, with `$E9CC/$EA27/$EA82/$EAEC` visible phases and `$E527` preserving lifecycle through blank gaps; exact slots `$0E02/$0E28` and positions are independently unit-covered, with post-fix visual acceptance still pending. The reusable contract separates lifecycle/phase/geometry/colour metadata from renderer style, captures raw source identity and emitted OBJ palette rather than inferring either from shared runtime composition addresses, validates applied blend support, and fails closed on ambiguous palettes or metadata overflow. `D6a`-`D6c` strictly cover the 240-tick miracle, 33-tick dragon strike, and 1,123 blue-fire samples; house fire and new-town lightning now need post-change live visual acceptance, while Red Demon still needs its live visual capture. 2026-07-27 adds independent off-by-default world navigation: a forced-top-down 1024x1024 developed plane using the game's live focus/matrix/zoom, location-aware haze (full-map dim outside all borders), seamless whole-world clouds and directional shadows, zoom-relative cloud-deck visibility, animated water, Palace/UI overlays, and continuous fade ownership. A 2048x2048 high-fidelity reconstruction from all six town tilesets remains research. |
| Battery save codec/editor | 🟡 | 2026-07-16: exact 8 KiB native codec, checksum validation/repair, version-1 lossless INI, deterministic active backend, atomic writes, timestamped editor backup, auto-persist/shutdown shadow re-sync, five paged edit groups (town/Death Heim/Professional progress, player/Angel status, magic, items, and BCD scores), import/export/session/persistent actions, `tools/srm.py`, and transactional tests are implemented. All 9 repository saves validate and `.srm → .ini → .srm` is byte-identical. Pending final gate: manual Apply and save → Restart Game → Continue acceptance matrix in the game. |
| Cheats | 🟡 | Named cheat kit 2026-07-07: `AR_ALL_MAGIC`/`AR_RANGED_SWORD`/`AR_INF_MP`/`AR_INF_SP`/`AR_ANGEL_HP` + magic-safe `AR_NO_KNOCKBACK` + generic `AR_PIN`; real 8x turbo on `t`. `AR_FREEZE_TIMER` auto-backoff added, still unverified. `AR_NO_KNOCKBACK` is not physics-neutral: its pinned invulnerability suppresses water drag (confirmed 2026-07-12). |
| Bridge structure-cap fix (sim) | 🟡 | 2026-07-17: structure-record system fully mapped + SRAM-validated (SEAMS town §7, save-format §3.4: 128 × 4-byte records per town, allocator `$03:9D9F`, census `$03:C07F`, miracle damage `$03:B274`, bridge immunity row `$A435`; record format confirmed against real saves incl. both bridge orientation variants). v1 slot-reuse/lightning designs were withdrawn after they erased bridges on reconstruction. v2 uses a validated/deduplicated completed-bridge sidecar: `$9D9F` migrates, `$C07E` restores support, `$9CFB` restores `$E1/$E2` marks, and `$89F0` decodes the native rebuild program to restore the visible metatile after `$9D4D`. Sidecar-only checksum changes are shadowed until a normal ROM save transaction, with a persistence regression test. Marks-only visual capture correctly failed (black bridge), establishing the second render seam; generated build + replacement screenshot are the remaining acceptance gate. |
| Build / platform targets | 🟡 | macOS (arm64, primary development platform) and Steam Deck are built and played on regularly — both confirmed working end to end from the distribution bundle. All seven bundles (macOS arm64/x86_64, Linux x86_64/arm64, Windows x86_64/arm64, steam-deck) cross-build from one machine because the Go module is CGO-free, but **Windows and generic Linux have not been run end to end by this project** — no CI, no `.vcxproj`, no verified launch. Treat those bundles as untested. See `docs/BUILD_TOOLING.md` for the packaging design and the open signing/notarisation gaps. |
| Debug tooling | ✅ | 2026-07-07 toolkit: `dis65`/`romxref`/`wram`/`resolve_miss`/`cycle.sh` — anomaly capture → auto-triage → proposed cfg patch loop (`DEBUG.md` §1) |
| Action widescreen BG/sprites | 🟢 | All ordinary stages and Death Heim are fully playable and visually validated: wide streaming, finite camera edges, sprites, activation, narrow-BG2 edge policies, HDMA/parallax scenes, bosses, and post-final-boss transitions behave correctly. 2026-08-09 `SPEC-bg-hle.md`: bounded `ActionBgWorld`, the 49-map `ActionBgPlan`, full-world provider, and exact diorama handoff are implemented. The provider is default-on with exact `AR_ACTION_BG_HLE=0` native fallback. After phase/source/bounds preflight, an exact authentic ring gives the provider the whole layer; an in-world ring contradiction retains a native authentic centre while the provider remains active for finite margins. Live VRAM/CGRAM, priority, windows, transparency, mosaic, color math, and scroll effects remain native PPU stages. Five paired 12-entry presentation matrices, long Fillmore Full/Raw/diorama-32 runs, a natural Death Heim transition soak, lifecycle/rebind/savestate/geometry gates, and a real compositor A/B pass; every authentic center and state artifact is exact. Wide Full `0301` intentionally corrects 30 synthetic-margin pixels at BG2's finite edge. Maximum-span cost remains 0.067 ms/frame, below the accepted 0.10 ms budget. BH8 removed the duplicate ring repair and unused clamp-band/margin-source-gap PPU prototypes; three final release matrices are 612/612 byte-exact. 2026-08-10 adds semantic playfield/scene/backdrop roles plus independent per-layer and row-band extents. Bloodpool's unique moon/cloud art is independently bounded (`0201` live-tuned to 76/100, `0202` and unbanded `0206` to 68/68, unbanded `0207` to 92/92); `0201` water remains wide while `0202` water inherits its backdrop extent. Boss room `0208` retains its world-backed BG1 but mirrors only 16/16 pixels beyond the authentic view, keeps viewport BG2 at 0/0, and caps the fitted camera to that actual playfield span. Source ownership and synthetic edge fill are now independent, so world+Mirror binds without a false Clamp fallback. The initial all-0/0 policy accepted all 204 Wide Full artifacts with only 4,074 side-margin pixels changed, and complete 4:3/Raw/Diorama-32 plus Death Heim rematch/final-arena gates pass. Run `20260810-172649` then exposed two boundary regressions: eight stale words on a newly exposed BG1 row triggered an atomic fallback, and a blocked `$7C=-120` request drove zero-input walking at the new left clamp. Snapshot comparison confirmed an 8px visibility versus 16px strip-publication cadence seam, not a transition-time ROM writer. Margin-only provider recovery and effective camera-delta reconciliation are implemented and regression-tested; a direct replay remains the visual acceptance gate. Native streamers/ring and live decorative paths remain as fallback/oracle infrastructure. |

## Codebase metrics (objective, automated — refreshed 2026-08-12)

These metrics measure structural and reference-vector coverage, not
playability. Re-run the listed commands when refreshing this section.

| Metric | Value | How to reproduce |
|---|---|---|
| Hand-authored recompiler directives (`recomp/*.cfg`) | 2,912 lines | `wc -l recomp/*.cfg` |
| → generated C output (`src/gen/*.c`) | 2,227,186 lines | `wc -l src/gen/*.c` (after `snesbuild regen`) |
| Hand-written game runtime (`src/`, excluding `src/gen` and shader blobs) | 59,265 lines | `find src \( -path src/gen -o -path src/shaders \) -prune -o -type f \( -name '*.c' -o -name '*.h' \) -print0 \| xargs -0 wc -l` |
| Bank coverage | 29 of 32 possible SNES banks | `ls recomp/bank*.cfg \| wc -l` |
| Recompiled functions (unique ROM addresses) | 2,483 | `grep -c "^    { 0x" src/gen/dispatch_v2.c` |
| Recompiled functions (× m/x width variants) | 4,657 | `go -C snesrecomp-go run ./cmd/v2regen link-audit --gen-dir ../src/gen --src-dir ../src --runtime-dir runtime/src` |
| Static reachability | 4,657/4,657 (100%) — 0 orphans, 0 unreferenced variants | same Go link-audit command |
| Unresolved trap sites | 74 logical sites / 165 variant emissions: 20 goto sites (53 variants) + 54 indirect-oob sites (112 variants) | `go -C snesrecomp-go run ./cmd/v2regen stub-census --gen-dir ../src/gen` |
| Opcode correctness vs. Tom Harte 65816 reference vectors | 227/227 opcodes clean, 14,528/14,528 vectors passed | `go -C snesrecomp-go run ./cmd/v2regen opcode-diff --cache-dir ../tools/oracle/harte_cache --runtime-dir runtime/src --all` |
| Go recompiler unit/regression suite | all packages passing | `go -C snesrecomp-go test ./...` |
| Generated-output layout | 83 generated C files in the current local build; generated files and comparison archives are ROM-derived and are not distributed | `find src/gen -maxdepth 1 -name '*.c' -type f \| wc -l` |

## What "done" looks like

At minimum, every action stage and simulation town must be played through and
marked ✅ or 🟡 with a specific known issue. Update this document with each
verification instead of batching status changes later.
