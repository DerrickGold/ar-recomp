# ActRaiser Recomp — Follow-up Improvements

**Date:** 2026-07-21
**Base commit:** `cc0b042` ("present thread, diorama extraction, and graphics settings")
**Predecessor doc:** `ar-recomp-threading-impl.md` (M0–M8 present-thread + diorama plan; implemented through the present-thread/diorama/settings commit above)
**Status:** planning — supersedes the "next steps" scattered across the audit of `cc0b042`.
**Revision note (same day):** corrected/hardened after a code-verified audit of every
claim against the `cc0b042` source: A3 rewritten (seeding already shipped), A1/A4
fix steps completed, B1 gained the OBJ/BG3 source mapping + wrap-fix note and a
corrected §5.8 citation, B3's cost accounting fixed, B4 gained dt-corrected damping
and the camera state-ownership split.

This doc rolls every outstanding change and improvement into one place, split into
two phases plus a parallel menu track:

- **Phase A — Baseline fixing.** Bugs/gaps in the *shipped* `cc0b042` code that
  should be corrected regardless of new features.
- **Phase B — Next steps / improvements.** New capability: >60fps interpolation,
  enhanced video settings, and porting select diorama effects into the flat 2D view.
- **Track M — Menu organization & visual polish.** Regrouping the sprawling
  graphics menus and making rows more readable (icons, value-vs-key text colors).

Scope discipline carried over from the predecessor doc: this is a pragmatic solo-dev
C codebase (direct globals, env-flag debug toggles, plain static functions, no DI/
frameworks). Every proposal is the smallest change that achieves the goal.

### Handover conventions (read before implementing — these recur throughout)
This doc is written to be implemented by someone who **cannot ask questions**. Two
rules recur; internalize them once:
1. **Present-thread code reads ONLY the `FrameSlot`, never live globals.** Any new
   value the present side needs (a setting, a WRAM read, a PPU field) must be
   snapshotted into `FrameSlot` by `FrameSlot_Capture` (game thread, `main.c` ~1774)
   and read from the slot. `present.c` must not `extern` `g_settings`/`g_ppu`/
   `g_snes_width` (predecessor §D6 — it's enforced by present.c not declaring them).
2. **There is NO `ENUM_SETTING` (or float) macro.** Bool/int settings use the
   `BOOL_SETTING`/`INT_SETTING` macros (settings.c:512/516); **enum-valued settings
   are RAW STRUCT LITERALS** (`kSettingType_Enum` + a labels array + count — copy the
   `extended_aspect` row, settings.c:581-587). Camera angles/distances are scaled
   ints (mrad, x100), never floats.
**"Author-blocked" / "tune later" markers:** where the doc says an item is
author-blocked, SKIP it — do not guess. Where it gives a **provisional value**,
implement that exact literal; the author may retune later, but a provisional value
is a build instruction, not a suggestion.
3. **Every lettered item ends with a "Demo / acceptance test:" line** — the smallest
   thing you can SEE or TEST once that item lands. Items tagged *(no eyeball demo)*
   change nothing visible on their own (race fixes, comment retense, dead-code
   removal, plumbing); they carry a non-visual check (regression diff,
   ThreadSanitizer, `AR_PERF` counters, instrumentation) and MUST be batched with a
   visible sibling. Items tagged **REGRESSION CHECKPOINT** are large invisible
   refactors that GATE a later visible payoff — their acceptance test is "output is
   pixel-identical / unchanged, but the new plumbing exists and is exercised";
   verify that BEFORE stacking features on top. The suggested-sequencing appendix
   ends with the full demo-checkpoint order.

---

## Provenance: where these items came from

The `cc0b042` audit (5 lenses, adversarially verified; the build lens was aborted —
no ROM on this machine, so the full game cannot link, and it shouldn't) produced
**0 high, 4 medium, 7 low** confirmed findings. Notably it also confirmed, as a side
effect, that `present.c` / `diorama.c` / `settings.c` **compile cleanly** (the build
reached 82% and the ROM-free settings test binaries linked; only the generated
`actraiser_00.c`/`funcs.h` wall stopped it, as expected).
**A7 (widescreen HUD anchoring skipped in diorama mode) was added later** — surfaced by
the author from in-game observation, then confirmed against `present.c` (the diorama
branch returns before `PresentHudOverlay`). It is a shipped regression, not from the
original audit sweep.

**Standing caveat — this doc is STATIC-REASONED; the ROM is the only real judge.** No
item here has been built or run: there is no ROM on this machine (see
[[rts-dispatch-dump-build-differs]]), so every "you can see X" and every code citation is
verified by reading source, not by execution. That is exactly how the two prior audits
(the 5-lens `cc0b042` sweep and the 8-agent demo-review) BOTH missed A7 — reasoning about
code in the abstract, while "the HUD looks wrong in diorama" only came from *playing*.
Treat the demo-checkpoint acceptance tests as the real gate: they must be run on hardware,
in the specific stage/mode each names. Where a fix has a single load-bearing assumption
that can't be checked statically (e.g. A7's `g_hud_bg_pixels`-populated question), the
item carries an explicit **Validation procedure** to run first. Line numbers drift as the
tree changes — treat them as "near here," re-grep the named symbol before editing.

Findings were triaged into "fix in place" (Phase A) vs "carry to next doc" (Phase B/
Track M). One audit finding — **sprite-upright** — was CLOSED, not carried: the
author tried the effect in-game and it didn't land (small, densely-animated,
near-side-on action sprites don't have the vertical extent for a "standing up"
read), so it was deliberately disabled. It is not a feature to finish; see A4 for
the cleanup of its now-inert setting.

---

## Phase A — Baseline fixing

### A1. F9 display-mode cycle bypasses the present-thread quiesce (the one real race)
**Severity:** medium (latent data race in shipped code).
**Where:** `src/main.c` F9 handler (~2743-2752) → `ApplyDisplayPresentation`
(~1273-1301) mutates the renderer (`SDL_SetRenderLogicalPresentation`,
`SDL_SetWindowSize`) on the game thread. `Settings_CycleDisplayMode`
(`settings.c` ~1607-1616) calls `Settings_SetDisplayMode` directly and does NOT go
through the settings observer `OnRuntimeSettingChanged`, so it does not inherit
that path's quiesce.
**Why it matters:** every OTHER renderer/geometry mutation quiesces the present
thread first (§2.9(a)/§D8 of the predecessor doc); F9 is the odd one out. With the
present thread live, F9 mutates renderer state concurrently with `PresentComposite`.
**Fix:** wrap the F9 geometry mutation in the same `PresentThread_Quiesce()` /
`Resume()` bracket the observer path uses. Smallest correct change: have
`Settings_CycleDisplayMode`'s F9 call site quiesce, OR route it through
`OnRuntimeSettingChanged` so it inherits the existing quiesce. Prefer the latter for
consistency (one quiesce owner). Mechanically: `Settings_CycleDisplayMode` computes
`next` then calls `Settings_SetLong` on the `display_mode` descriptor instead of
`Settings_SetDisplayMode` directly — `FinishChange` fires `DisplayModeChanged` (ws
flags) then the observer (quiesce + `ApplyDisplayPresentation` + redraw-pending);
no recursion, since `Settings_SetDisplayMode` writes fields directly. **The F9 call
site must then DROP its own direct `ApplyDisplayPresentation()` and
`g_paused_redraw_pending` lines** — keep them and the unquiesced mutation (and the
race) survives its own fix.
**Demo / acceptance test:** *(no eyeball demo)* F9 still visibly cycles 4:3 → wide
RAW → wide FULL exactly as before (regression); the fix removes only a
nondeterministic race, so verify it under ThreadSanitizer OR a rapid-F9 stress loop
with the present thread live at 16:9 (crash/glitch-free). Do this item FIRST — it's
independent of everything and it's the one real bug.

### A2. Stale "M5.3 pending" comments no longer match shipped code
**Severity:** low (doc/comment drift).
**Where:** `src/present.c:11-12` and nearby — comments still use anticipatory
phrasing ("after the present-thread handshake lands (M5.3)") for a handshake that IS
landed in `cc0b042` (real `SDL_CreateThread`, condition-var protocol, quiesce,
synchronous headless fallback). The ownership rules the comment states are correct —
only the tense is stale.
**Fix:** retense the comments to describe the shipped state. No code change.
**Demo / acceptance test:** *(no eyeball demo)* comment-only; verify by diff + clean
build. Batch with A4 (the same menu-cleanup commit).

### A3. Redundant C-literal camera defaults (§D13 residue — the seeding is already shipped)
**Severity:** low (maintenance hazard, not a live bug).
**Where:** `src/diorama.c` (~443-445) `static DioramaCamera g_diorama_cam = { .tilt_x
= 0.0f, .tilt_y = -0.18f, ... }`.
**Status check (corrects the original audit framing of this finding):** the §D13
seeding IS implemented in `cc0b042` — `Diorama_SeedCameraFromSettings` exists
(diorama.c ~460) and runs at boot after the settings load (main.c ~2521), on menu
edits of the camera rows (main.c ~1392), and inside `Diorama_ResetCamera`
(diorama.c ~502). The literal and the descriptor defaults also currently AGREE
(`-0.18f` ↔ `-180` mrad, 0 ↔ 0, 0 ↔ 0), and the boot seed overwrites the literal
before first render — so boot and "Reset Camera" cannot diverge today.
**What's actually left:** the literal is dead weight a future editor could mistake
for load-bearing (edit one side, silently disagree with the other until the next
seed call). Drop it — zero-init and let the boot seed populate it, or leave a
comment pointing at the descriptors as the single source of truth. Separately,
reconcile the shipped defaults against the predecessor §D13 example values (shipped:
tilt_y=-180 / tilt_x=0 / distance=0-auto; §D13's example: 150/0/500) — pick one and
the descriptor owns it.
**Demo / acceptance test:** *(no eyeball demo for the literal drop)* removing the
dead literal is verified by "Reset Camera" producing a byte-identical pose
(screenshot diff) — the boot seed already overwrites it. ⚠️ **Split out the secondary
default-reconcile sub-task:** IF you change a descriptor default value (e.g. adopt
§D13's 150/0/500), that IS visible — the boot/Reset diorama pose changes — so treat
it as its own visible checkpoint, not part of the invisible literal cleanup.

### A4. Inert sprite-upright setting (feature closed, tidy the phantom knob)
**Severity:** low (UX: a slider that does nothing).
**Context:** the sprite-upright effect was tried and rejected (see Provenance).
`diorama.c` has `#define USE_UPRIGHT_OBJ` commented out (~line 8) and
`disable_fig_adjust = 1` hardcoded (~745), so the upright logic is dead code — but
`diorama_sprite_upright` remains a LIVE `INT_SETTING` (default 60, range 0-100,
`settings.c` ~629-631). A player can move a slider with no effect.
**Fix (pick one):**
- **Remove** (preferred, since the effect is a decided "no"). Full checklist, not
  just the descriptor: the descriptor (`settings.c` ~629-631), the `g_settings`
  field (`settings.h`), the unconditional read at `diorama.c` ~742 (a compile error
  surfaces this one), the `"diorama_sprite_upright"` entry in
  `Diorama_ResetCamera`'s `kResetKeys` (`diorama.c` ~490 — this one fails SILENTLY
  via the `if (row)` guard, so grep for it), and the `#ifdef USE_UPRIGHT_OBJ` block
  + commented `#define` (`diorama.c` ~8, ~747-755).
- **Or park:** keep the code but hide the setting (availability hook → false, or
  drop the descriptor) so it's not a phantom knob, if there's appetite to revisit
  the effect with a different approach later.
**Demo / acceptance test:** open the diorama settings section — the "Sprite upright"
row is GONE (or greyed/hidden if parked). Directly eyeball-verifiable in the menu; no
ROM/action-stage needed. This is the visible sibling to batch A2/A3 with.

### A5. HUD-flat decision (§5.7.1) not implemented
**Severity:** medium (spec deviation; HUD readability).
**Where:** `actraiser_rtl.c` (~1169-1174) captures BG3 as a full diorama layer with
RemoveFromGame unconditionally; `diorama.c` (~561-562) projects BG3 as a tilted
plane at z=0.95. There is no `diorama_hud_flat` field/setting anywhere (confirmed by
grep).
**Why it matters:** BG3 is the status bar (ACT/TIME/SCORE, HP, boss health) —
informational text that keystones under tilt, exactly the readability problem
§5.7.1 designed the flag to prevent.
**Depends on / distinct from A7.** A5 is about the HUD's *tilt* (flat vs keystoned);
**A7** is the separate, more fundamental bug that the widescreen *anchoring* logic
(`BuildHudPresentationChunks`) never runs in diorama mode at all. A5's "flat" branch
routes through the same `PresentHudOverlay` call A7 restores — so A7 lands the
anchoring unconditionally and A5 then chooses whether that anchored HUD is drawn flat
(default) or as a tilted plane. **Read A7 first**; A5's implementation below assumes
A7's `PresentHudOverlay`-in-diorama-branch plumbing exists.
**Decision — implement with a CONCRETE default; tuning is a later pass.**
`diorama_hud_flat` selects between two ACHIEVABLE looks (see the constraint below —
the naive "tilted + anchored" third option is NOT one of them):
- **true (default): flat anchored HUD** — BG3 kept out of the diorama capture and drawn
  via the `PresentHudOverlay` path A7 restores. This is the widescreen-spread, readable
  HUD. Ship default = true; a readable HUD is the safe out-of-box choice.
- **false: raw tilted BG3 plane (unanchored)** — capture BG3 as a diorama layer and
  project it tilted at z=0.95. This is the *current* diorama look, and it is
  **inherently unanchored** (see constraint) — it's kept only as an A/B curiosity /
  "authentic centered strip in 3D" option, not as an anchored alternative.

**Load-bearing constraint (this is concern #1 — do NOT design around it).** The
anchoring in `BuildHudPresentationChunks` is *screen-space 2D*: it emits plain
`SDL_RenderTexture` destination rects (`present.c:53,283`) that place HUD sub-regions
at the left/center/right screen edges. Those rects have no meaning once fed through the
diorama's mesh + UV-warp projection — you cannot cheaply "draw the anchored chunks on a
tilted plane." A *tilted AND anchored* HUD would require re-projecting each anchored
chunk as its own world-space quad (new geometry work on the scale of B6's
`BuildQuadMesh`), which is out of scope here. So the honest menu is exactly the two
options above: **anchored-and-flat, or tilted-and-unanchored.** Do not promise a third.
This is NOT an author-blocker: implement `diorama_hud_flat` default-true now; the author
may flip the default later, but that's a one-line change, not a prerequisite.
**Implementation (concrete, both threads). A7 does the heavy lifting; A5 is the
selector on top of it:**
- Setting: `BOOL_SETTING(diorama_hud_flat, NULL, "Flat HUD", "...",
  kSettingCat_Presentation, /*def*/1, false, Diorama_ModeIsOn, NULL)` + the
  `g_settings.diorama_hud_flat` field.
- **true branch = A7's path unchanged:** BG3 excluded from the diorama `kCaptureLayers`
  loop (A7), so its line-906 HUD RemoveFromGame capture into `g_hud_bg_pixels` survives,
  and the diorama branch calls `PresentHudOverlay(slot, viewport)`.
- **false branch = today's capture:** BG3 stays IN the diorama `kCaptureLayers` loop
  (`actraiser_rtl.c` ~1171) and is drawn as the tilted plane (diorama.c ~561-562); do
  NOT call `PresentHudOverlay` for it. This is the unanchored option — that's expected
  and documented, not a bug (per the constraint above).
- Snapshot `diorama_hud_flat` into `FrameSlot` (present thread must not read
  `g_settings` — same rule as every other present-read field). The capture-side
  decision (include/exclude BG3) is a game-thread read of the setting, so it can read
  `g_settings` directly in `ActRaiserDrawPpuFrame`; only the present-side branch needs
  the slot copy.
**Demo / acceptance test:** in diorama mode in an action stage, the BG3 status bar
(ACT/TIME/SCORE, HP, boss health) no longer keystones under tilt — it renders flat and
readable; toggling the new "Flat HUD" row switches it live between flat and
tilted-plane. Requires ROM + an action stage.

### A6. Missing settings for shipped/near-shipped features
**Severity:** low-medium (features exist only behind env flags).
**Where:** `settings.c` g_setting_descs[] has the diorama block but NO
`uncapped_framerate` or `scroll_interpolation` rows; interpolation is reachable only
via `AR_INTERP_ENABLE` (present.c ~580). These were §10 settings in the predecessor.
**Fix:** this is really Phase B territory (they gate B1/B2 features), but if any ship
enabled they need menu rows. Track under B and Track M's reorg, not as a standalone A
patch — noted here so it isn't lost.
**Demo / acceptance test:** *(not a deliverable)* forward-reference note only — the two
rows are actually built under B2/B1b. Discharged there; nothing to build in Phase A.

### A7. Widescreen HUD anchoring is skipped ENTIRELY in diorama mode (shipped bug)
**Severity:** medium (visible regression — diorama mode loses the widescreen HUD
layout that flat mode has).
**Where:** `PresentComposite`'s diorama branch (`present.c:576-611`) composites the
separated layers, then calls only `PresentSceneInspector` + `SettingsOverlay_Render`
and `return;`s — it **never reaches `PresentHudOverlay`** (`present.c:626`, flat branch
only). `PresentHudOverlay` (`present.c:279`) is the sole caller of
`BuildHudPresentationChunks` (`present.c:83`), which does ALL the widescreen HUD
anchoring: the 3-band split, ACT/TIME/SCORE left/center/right spread
(`present.c:106-138`), left-anchored player health / right-anchored magic scroll
(`present.c:140-169`), full-width boss-health row (`present.c:171-187`), and the
selected-magic-icon placement (`present.c:194-208`). None of that runs in diorama
mode. Instead BG3 is captured as a raw diorama layer (`actraiser_rtl.c` ~1169-1174,
RemoveFromGame) and projected as a flat 256-wide tilted plane at z=0.95 (`diorama.c`
~561-562) — so the HUD shows as an unanchored, non-widescreen strip.
**Why it matters:** this is the bug behind "our widescreen HUD placement/overlay
doesn't work in diorama mode." The recent `c07c377` ("fixes for widescreen hud") work
tuned the flat path's anchoring; the diorama path never invokes it. It is a shipped
regression, not a missing feature — flat mode anchors the HUD correctly and diorama
mode does not.
**Fix (unconditional — anchoring is correctness, not an A/B):** route the diorama
branch through the same HUD path the flat branch uses. Mechanically this is the plumbing
A5 already specifies, but it must ship **independent of the `diorama_hud_flat` toggle**:
- Capture side (`actraiser_rtl.c` ~1169-1174): EXCLUDE `kPpuOverlaySource_Bg3` from the
  diorama `kCaptureLayers` loop so the diorama block does NOT rebind BG3 to its own
  capture surface. **How this works (verified statically):** in flat play,
  `actraiser_rtl.c:906` already sets a BG3 `RemoveFromGame` capture over the HUD split
  region, routed to `g_hud_bg_pixels` by `RebindPpuOutputSurfaces` (`main.c:1726`) →
  uploaded to `g_hud_bg_texture` (`present.c:482`). The diorama block currently
  *overrides* that by rebinding BG3 to `g_diorama_layer_pixels[Bg3]` at
  `actraiser_rtl.c:1177`. Dropping BG3 from `kCaptureLayers` leaves the line-906 HUD
  capture standing, so `g_hud_bg_pixels`/`g_hud_bg_texture` populate exactly as in flat
  mode. The widescreen HUD split metrics are snapshotted into `FrameSlot` regardless of
  mode (`main.c:1801-1805`), so no new snapshot is needed. **(This is the one link that
  MUST be validated on a ROM — see Validation below.)**
- Present side: in `PresentComposite`'s diorama branch, after `Diorama_Composite` and
  before the inspector/overlay, call `PresentHudOverlay(slot, viewport)` — the exact
  call the flat branch makes at `present.c:626`. The `viewport` is already computed for
  the diorama output (`present.c:606-608`).
- Interaction with A5: with `diorama_hud_flat = true` (default) this anchored HUD is
  drawn flat — the intended look. With `diorama_hud_flat = false`, BG3 stays in the
  diorama capture and there is NO anchored HUD (the tilted-unanchored A/B option, per
  A5's constraint). **A7 guarantees anchoring is available; A5 chooses flat vs tilted.**
  Do NOT attempt to draw the anchored chunks on the tilted plane — see A5's load-bearing
  constraint (screen-space rects can't be fed through the mesh projection).

**Sibling non-issue — `PresentHdReplacements` skipped in diorama is CLOSED by design
(concern #3, author-decided 2026-07-21).** The diorama branch never calls
`PresentHdReplacements` (`present.c:627`, flat-only), so the *current* overlay-based HD
replacement mechanism does nothing in diorama mode. **This is not tracked as a bug:**
the author does not use HD replacements in action stages via the current mechanism, and
the planned N* tile/graphics replacement will flow through the **normal PPU pipeline**
rather than the post-scanout overlay path. That future mechanism sidesteps this entirely
— PPU-pipeline replacement is captured into the diorama layer planes *natively* (the
per-layer BG/OBJ overlay captures at `actraiser_rtl.c:1169-1181` read PPU scanout, so
whatever the pipeline substituted is already in `g_diorama_layer_pixels[...]`), with no
`PresentHdReplacements` call needed. So the diorama branch skipping the overlay path is
correct, not a gap: the overlay mechanism is the wrong tool for diorama, and the right
tool (pipeline replacement) requires zero diorama-specific work. **No action.**
(`PresentMode7Composite`, `present.c:625`, is likewise skipped and genuinely moot:
diorama is action-stages-only per `ActRaiser_IsActionMapGroup`, and Mode7 is the
sim/overworld.)

**Validation procedure (concern #4 — we can't run this here; no ROM on this machine).**
The fix hinges on `g_hud_bg_pixels` being populated in diorama mode. Confirm it in this
order — each step is cheap and localizes the failure:
1. **Static precondition (already done, re-confirm after editing):** grep that BG3's
   line-906 `PpuSetOverlayCapture(... kPpuOverlaySource_Bg3 ... RemoveFromGame)` still
   runs in the diorama frame path AND that BG3 no longer appears in the diorama
   `kCaptureLayers[]`. If BG3 is in both, they fight and the last bind wins (today: the
   diorama one, which is the bug).
2. **Instrument the buffer (no ROM-visual needed to disprove the null case):** in
   `PresentHudOverlay` (or `BuildProjectionInputsFromSlot`), add a one-shot
   `fprintf(stderr, "[hudchk] diorama=%d bg_tex=%p split=%d chunks=%d\n", ...)` behind an
   `AR_HUDCHK` env flag, logging `in.hud_bg_texture`, `in.hud_split_height`, and the
   `BuildHudPresentationChunks` count. Run a widescreen action stage in diorama mode:
   expect `bg_tex != NULL`, `split > 0`, `chunks >= 3`. If `split == 0` the metrics
   aren't reaching the slot in diorama mode (check `main.c:1801-1805` runs); if
   `chunks == 0` with `bg_tex` set, the split metrics are zero (same cause).
3. **Visual confirm:** the acceptance test below. If steps 1–2 pass but the HUD is
   blank/garbage, the buffer is bound but not being *written* in diorama mode — check
   that BG3's screen-enable bit (`g_ppu->screenEnabled[0] & (1<<Bg3)`) is set in the
   action stage (the line-906 capture is unconditional, but scanout only fills the
   surface if the layer is on).
4. **A/B against flat:** toggle diorama off in the same stage; the HUD anchoring should
   be pixel-equivalent between the two modes (same chunks, same dst rects). Divergence
   means the diorama `viewport` (`present.c:606-608`) differs from the flat one — expected
   if the output size differs, but the *anchoring topology* (edges hit the edges) must
   match.

**Demo / acceptance test:** in diorama mode in a **widescreen** action stage,
ACT/TIME/SCORE spread to the screen edges and boss health spans full width — matching
flat mode — instead of a centered 256-wide tilted strip. Requires ROM + a widescreen
action stage. Pairs with A5 (step 5 in the demo sequence): A7 makes the HUD *anchored*,
A5 makes it *flat*.

---

## Phase B — Next steps / improvements

### B1. >60fps (the headline) — TWO separable layers, only one is diorama-only

**">60fps" is two different things. Be explicit about which works where, because a
player who never enables diorama still gets one of them and not the other:**

- **B1a — Present decoupled from game tick (mode-agnostic; already works flat).**
  The M5/M6 present thread already runs presentation off the game thread. **Precise
  claim (corrected — the earlier "re-presented at 120/144Hz" framing overstated it):**
  a new present only fires when the game thread submits a frame (`g_frame_pending`,
  ~60/s) or on a wait-timeout re-present; without `AR_INTERP_ENABLE` the timeout is
  16ms and only fires when nothing is pending, so **the actual present cadence today
  is ~60Hz and flat pixels update at 60fps** — NOT a higher on-screen rate. The
  always-on win is real but *invisible by eye*: moving the vsync WAIT off the game
  thread lowers input-to-photon latency and steadies pacing. Verified:
  `PresentComposite`'s flat branch (present.c ~615+) is a plain `SDL_RenderTexture`
  of the whole framebuffer; `PresentThreadFn` (main.c ~501-537) gates presents on
  `g_frame_pending`. **There is no framerate cap in the loop at all** — the only
  limiter is vsync (`SDL_SetRenderVSync(g_renderer, 1)`, main.c ~2448) plus the
  16ms/4ms wait cadence, and **nothing reads an `uncapped_framerate` setting today**.
  So the B1a work item is NOT just "expose a setting" — the toggle must be WIRED to a
  real mechanism (`SDL_SetRenderVSync` on/off and/or the present redraw cadence) or it
  changes nothing regardless of state.
  **Demo / acceptance test:** *(no eyeball demo)* the new `uncapped_framerate` row
  appears and cycles (that's the only visible artifact); its EFFECT (latency/pacing)
  is read from `AR_PERF` `[present-perf]`/`[perf]` counters (main.c ~546,573,2094),
  not by eye. Ship the row only once it's wired to a real vsync/cadence mechanism.

- **B1b — Interpolated MOTION between game frames (the "120fps animation" payoff;
  diorama-only today).** The scroll-shift that makes motion actually look like 120fps
  is gated inside `if (slot->diorama_active)` (present.c:576); the flat branch has no
  separated planes to shift, so it can't interpolate — at 120Hz flat mode shows each
  60fps frame twice (smoother *presentation* per B1a, but the *animation* stays 60).
  Extending B1b to flat mode is possible but is the **same refactor as B3** — see
  "B1b in flat mode" below. Everything from here down in B1 is about B1b.

**Problem (root-caused with the author):** B1b's shipped interpolation is disabled by
default (`AR_INTERP_ENABLE`) because it made backgrounds *vibrate*. The documented
cause (present.c ~580-592) is that it interpolates the **end-of-frame PPU scroll
register residue**, and ActRaiser's BG2 parallax is **HDMA-driven** (per-scanline
rewrites), so the snapshotted `hScroll[1]` is not a stable camera position — it's
whatever the last HDMA line left, which jitters frame-to-frame with no real camera
motion.

**The rejected non-fix:** excluding HDMA-driven layers from interpolation. At >60fps
that makes excluded layers stutter at 60 while neighbors glide, and the *relative*
judder reads worse than honest 60-everywhere. It also permanently forks the render
path. Do NOT design around exclusion.

**The fix that scales to any framerate — interpolate the RIGHT quantity:**
- Separate **base camera scroll** (low-frequency viewport motion the eye tracks —
  must be smooth at 120/144fps) from **HDMA per-scanline displacement** (high-freq
  raster detail baked into the layer texture at 60fps).
- Interpolate the **base camera for EVERY layer** (including HDMA ones); let the
  baked HDMA raster ride along on the smoothly-gliding texture. The eye tracks the
  camera glide; nobody perceives that the wave *phase* refreshes at 60 while the
  whole layer moves at 120. This is standard emulator/remaster practice.
- **Source:** interpolate ActRaiser's stable logical camera in WRAM
  (`kActRaiserWram_Bg1CameraX/Y` = `$0022/$0024`, `Bg2CameraX/Y` = `$0026/$0028`,
  `actraiser_game.h:67-70`) — the game-authored camera *before* HDMA touches the
  registers — NOT the end-of-frame PPU register residue. Snapshot those into
  `FrameSlot` (via `ReadWram16`) instead of `hScroll[]`.
  - Caveat: `FrameSlot_Capture` runs *after* `RtlDrawPpuFrame` (main.c ~612/646), so
    the registers are already the residue by then — but WRAM still holds the logical
    camera regardless. That's why the WRAM source is the clean pick. (A game-agnostic
    alternative — latch base scroll at the top of the frame before the HDMA loop — is
    a runtime change deferred to "other games" later.)
- **Per-plane mapping — spell out OBJ and BG3, because OBJ is the trap:**
  `DioramaLayerBgIndex` (diorama.c ~681) currently maps only the BG1/BG2/BG3 planes;
  **OBJ planes return -1 = no interpolation today.** Left that way, sprites —
  including the player standing on BG1's platforms — step at 60 while the world
  glides at 120+: exactly the relative-judder artifact the rejected non-fix above
  rules out, on the most eye-tracked object on screen. The rule: **OBJ planes ride
  the BG1 base-camera delta.** Sprite screen positions already embed the camera
  (screen = world − camera), so shifting the OBJ plane by the interpolated camera
  delta keeps sprites rigidly attached to the gliding world; their own world-space
  animation refreshes at 60 — the same acceptable residual as the HDMA raster.
  **BG3 (HUD) has no WRAM camera; its delta is 0** (static HUD — composes with A5's
  flat-HUD default, where BG3 isn't a tilted plane at all). Backdrop stays 0, as
  today.
- **Adjust the wrap fix-up with the source:** `ComputeDioramaScrollDelta`'s ±512
  wrap correction (present.c ~548-551) exists for the 10-bit modular PPU registers.
  16-bit WRAM camera values wrap naturally in int16 arithmetic — drop/adapt the
  correction, or it will itself corrupt large legitimate deltas. (Terminology note:
  the shipped math *extrapolates* forward from the newest slot — `t` runs from
  `curr`'s timestamp — rather than blending prev→curr. Right call, no added latency,
  and unchanged by the source swap.)

**Residual ceiling (be honest):** if an HDMA effect is *itself* the animation (a
water surface whose wave scrolls with no camera motion, or a Mode-7-style per-line
warp), the base camera is static and only the raster moves → that specific motion
stays 60fps. Fine and imperceptible for raster detail. Making *that* smooth needs
capturing the full per-scanline HDMA scroll TABLE for two frames and interpolating
per-line — a much bigger extension (store the HDMA table, not just render output).
**Flag as a future stretch; do not build now.** Base-camera interpolation gets ~95%
of the visible benefit.

**B1b in flat mode — possible, but it's the B3 refactor (cross-linked).** Flat
interpolation needs the layers kept *separate through present* instead of composited
to one framebuffer — the exact "separated planes survive into the flat present path"
plumbing B3 requires for depth-shading/shadows/parallax. So flat motion-smoothing and
B3 are ONE work item, not two; sequence them together (build the plumbing once, see
B3). Two notes specific to the flat case:
- **Easier technique than diorama.** The flat path can interpolate by shifting the
  `SDL_RenderTexture` **source rect** by a sub-pixel amount per layer (no
  `SDL_RenderGeometry`, no tilted-quad UV warp), which sidesteps the NEAREST-quad
  wobble the diorama path hits. Same B1 *source* fix still applies — interpolate the
  stable WRAM camera, not the HDMA-polluted register, or flat mode vibrates exactly
  as diorama did.
- **Ceiling:** flat B1b buys smooth scrolling + the B1a latency/pacing wins. It does
  NOT enable B4 (dynamic camera) — that needs planes at different Z to sway, which
  flat mode has no analog for. Flat's >60fps ceiling is "smooth + responsive," not
  "cinematic camera."

**Crispness pass (secondary, independent):** the diorama layer textures are
`SDL_SCALEMODE_NEAREST` (main.c ~2508) and the interpolated UV shift is fractional
(present.c ~552), so once the source is stable this manifests as fine *stepping*
rather than vibration. A supersample pass fixes it — **new design in this doc, not
in the predecessor**. **Use factor ×4** (`enum { kDioramaSupersample = 4 };`) — match
the existing HD/Mode-7 supersample scale the codebase already uses (`kHdMode7Scale=4`,
present.c:42 / main.c:105), so buffer-sizing and pitch conventions carry over. Render
each layer to a ×4 integer-upscaled NEAREST
intermediate, then sample THAT with LINEAR for the tilt+shift. What §5.8 actually
supplies is the constraint the design must honor — "keep NEAREST for game planes,
or premultiply before a LINEAR sample" — so premultiply before the LINEAR sample to
avoid the straight-alpha dark fringe. This is the difference between "smooth" and
"crisp and smooth" at high refresh — schedule as polish after the source fix, not
blocking.

**Settings:** wire `uncapped_framerate` + `scroll_interpolation` as real menu rows
(they're currently env-only; see A6). Default decision after the source fix proves
stable in-game.

**Demo split — B1 is THREE checkpoints, not one:**
- **B1a — uncapped row** *(no eyeball demo — see B1a above)*: row appears/cycles;
  verify pacing via `AR_PERF`; wire to a real vsync/cadence mechanism.
- **B1b — motion-interp SOURCE fix** *(VISIBLE)*: interpolate the stable WRAM camera
  (`Bg1/Bg2CameraX/Y`) instead of the HDMA-polluted `hScroll[]` residue, drop the
  ±512 wrap fixup (present.c ~548-551), and map OBJ planes to the BG1 delta
  (`DioramaLayerBgIndex` returns -1 for OBJ today, diorama.c ~681-689). Ship the
  `scroll_interpolation` row WITH this fix so a no-env-flag user can enable it.
  **Acceptance test:** in diorama + action stage, enable Scroll interpolation — BG2
  parallax stops *vibrating* while the camera is static, and on a >60Hz display
  scrolling motion visibly glides instead of stepping. (Needs a >60Hz monitor or
  turbo to see the glide; the vibration-fix is visible at any refresh.)
- **B1b-crisp — ×4 supersample + premultiplied-LINEAR** *(VISIBLE, static)*:
  **Acceptance test:** tilt the camera and hold still — high-contrast pixel-art edges
  on the tilted quads stop shimmering/stepping. Best judged as a still A/B; visible
  even with interpolation OFF.

### B2. Enhanced video settings
This section is a menu of candidates. **A question-less implementer CANNOT "select
from candidates" or run a requirements pass — so this section is split into
monkey-implementable items (build these) and AUTHOR-BLOCKED items (SKIP, do not
guess).**

**B2-build (monkey implements — these are settings the doc already decided elsewhere,
just surfaced here):**
- `uncapped_framerate` toggle (BOOL_SETTING, `kSettingCat_Presentation`, default
  OFF) — exposes B1a present pacing. Wired to the present thread's rate cap.
- `scroll_interpolation` toggle (BOOL_SETTING, default OFF) — replaces the
  `AR_INTERP_ENABLE` env flag (B1b). Default OFF until B1b's source fix lands.
- The diorama knobs from B4/§10 (camera mode, baseline, reactive strength) and the
  existing depth-shade/camera-reset rows — these are specified in B4 and already
  exist; just place them in the reorganized menu (Track M).

**B2-author-blocked (SKIP — require author requirements + art/tuning a monkey can't
supply; do NOT attempt):**
- Integer-scale / pixel-perfect output modes — needs a decision on scaling policy
  vs the existing `window_scale`/`pixel_aspect` interaction; author-scoped.
- Scanline / CRT filter — needs shader authoring + a look decision (interacts with
  the `pixel_aspect` 7:6 path). Author-scoped; treat like the M8 shader work.
- Frame-pacing / vsync exposure — only if the author wants it surfaced; not required
  for B1a to function.
These stay OUT of scope for the handover implementation. Track M should leave menu
room for them but the monkey does not build them.

**Demo / acceptance test (B2-build):** the two toggle rows appear in the reorganized
menu and cycle On/Off. `uncapped_framerate` is *(no eyeball demo)* — see B1a (wire it
or it's inert). `scroll_interpolation` is the visible one but only once B1b's source
fix lands (ship them together) — see B1b's test. **B2-author-blocked: nothing to
demo, nothing to build.**

### B3. Diorama effects ported into the flat 2D view ("within reason")
The interesting/fuzzy one: which diorama visual techniques survive WITHOUT true 3D
projection, applied to the normal flat present path. Per-effect feasibility triage
(the "within reason" filter):
- **Per-plane depth shading (PORTABLE, high value — the tint is near-free, the
  plumbing is not):** the atmospheric-perspective tint (farther layers
  darker/cooler) is just per-plane vertex-color modulation, and a
  `diorama_depth_shade` setting already exists (`settings.c:632`) driving the
  tilted-view shading. BUT "apply it in the flat path" means the separated-capture
  + multi-plane flat composite must run in flat mode — capture (RemoveFromGame) is
  currently gated to diorama mode + action stages. That shared plumbing is the real
  cost of ALL the flat-mode effects in this list (shadows, parallax too) — pay it
  once behind the "2D depth enhancement" toggle, and note it carries the capture
  caveats into the flat view: per-frame capture cost, and per-layer capture loses
  cross-layer color math (predecessor §5.8/§8.1). Once the plumbing exists, the
  tint is the cheapest, best-ratio effect to evaluate first.
- **Drop shadows between planes (PORTABLE, medium):** the cheap offset-and-darken
  shadow (already in `diorama.c` for the tilted view) works flat too — a soft dark
  offset of each nearer plane onto the one behind gives 2D layers solidity.
- **Parallax separation (PARTIAL):** true parallax needs per-plane motion which the
  flat path doesn't currently do (it composites to one framebuffer). It rides the
  SAME separated-plane plumbing as depth-shade/shadows above, plus per-plane offsets
  on top — a small delta once that plumbing exists, not an independent mountain.
  Worth it only if depth-shade/shadows land first and it still feels flat.
- **Perspective tilt / sprite-upright / true Z (NOT PORTABLE):** these ARE the 3D
  effect; no meaningful flat analog. Excluded by "within reason."
- **Data point from sprite-upright:** small, densely-animated, near-side-on action
  sprites don't have the vertical extent for "standing up" reads — informs which
  figure-oriented effects are worth attempting in either mode.

Recommendation: build the shared flat-separation plumbing once, behind a "2D depth
enhancement" toggle so purists can keep authentic flat output; then port
**depth-shading first** (best ratio), then shadows.
**The toggle setting (concrete):** `BOOL_SETTING(flat_depth_enhance, NULL, "2D depth
enhancement", "Separate background layers for subtle depth shading in normal (non-3D)
view.", kSettingCat_Presentation, /*def*/0, false, NULL, NULL)` — **default OFF**
(opt-in: it changes authentic flat output and adds per-frame capture cost; purists
keep it off). Field `g_settings.flat_depth_enhance`.

**Shared plumbing with B1b (flat >60fps motion).** This same "separated planes in the
flat present path" refactor is what flat-mode motion interpolation (B1b) needs — the
planes it would shift per-frame are the planes this section separates. Treat flat-B1b
and B3 as one plumbing effort: separate the planes once, then B3 adds per-plane
tint/shadow/offset and B1b adds the per-plane interpolated source-rect shift on top.
Sequence: land the shared separation, then whichever of {depth-shade, flat motion}
you value first.

**Demo split — B3 is FOUR checkpoints, plumbing first:**
- **B3-plumbing — flat separated-plane refactor** **(REGRESSION CHECKPOINT)**: with
  `flat_depth_enhance` ON (default OFF), the flat path stops compositing one
  framebuffer (present.c ~614-631) and runs the separated multi-plane composite in
  flat mode. **Acceptance test:** *cannot be asserted pixel-identical* — per-layer
  capture loses cross-layer SNES color math (§5.8/§8.1) — so verify by instrumentation
  instead: with the flag ON there is no *visible* regression, the per-layer buffers
  actually populate in flat mode, and any color-math divergences are catalogued. Gate
  this BEFORE porting any effect on top.
- **B3-depth-shade** *(VISIBLE)*: **Acceptance test:** with the flag ON in normal 2D
  view, farther background layers render visibly darker/cooler; reuse the existing
  `diorama_depth_shade` slider (settings.c ~632), which today is `Diorama_ModeIsOn`-
  gated (settings.c ~634) and must be made reachable in flat mode — the slider changes
  the strength live.
- **B3-shadows** *(VISIBLE)*: **Acceptance test:** each nearer flat layer casts a
  subtle dark offset shadow onto the layer behind it, giving 2D layers solidity.
- **B3-parallax (PARTIAL)** *(VISIBLE)*: **Acceptance test:** on scroll, background
  layers move at slightly different rates in normal 2D view. Pin scope: this is a
  60fps per-plane *offset*, distinct from flat-B1b's sub-frame source-rect shift on the
  same plumbing — decide which you're building so they don't collide.

### B4. Reactive camera — make the diorama a GAMEPLAY effect, not a screenshot
**The core problem (author's insight):** the diorama's payoff is parallax between
planes, but parallax is only visible when the *camera* moves — and manually orbiting
the camera while playing is awkward and not something the player wants to do. So
today the effect is essentially a static tableau: impressive in a screenshot, inert
in motion. The fix is to make the camera **react to gameplay** so the depth comes
alive without the player thinking about it.

**This is very feasible and cheap — the game already exposes the signals in WRAM:**
- `PlayerVelocityX/Y` (`$08A6/$08A8`, actraiser_game.h:101-102) — the Hero's motion.
- `PlayerPositionX/Y` (`$08A2/$08A4`) and `Bg1CameraX/Y` (`$0022`) — position within
  the frame (player's screen-relative offset = position − camera).
- `PlayerFlags` (`$08D0`) incl. invulnerable/just-hit (`$2000`, ~181), and
  `PlayerBoost`/`PlayerInvulnerabilityTimer` — event triggers.
All are already snapshot-friendly (read via `ReadWram16` in `FrameSlot_Capture`,
same path B1 uses for the camera source). No engine changes.

**Mechanism — "sway toward motion," all on the present side, all clamped:**
In Dynamic Cam, drive small offsets from gameplay signals, layered on top of the
**Dynamic-Cam baseline** (see the two-mode section below — NOT the free-cam angle):
1. **Velocity-lean (the primary effect):** yaw the camera slightly toward horizontal
   run direction and pitch slightly with vertical velocity. `target_yaw =
   baseline_yaw + k_run * clamp(velX / velX_max)`; likewise pitch from velY
   (jump/fall). Running
   right leans the shadowbox so the right side opens up → parallax reveals depth
   *in the direction you're moving*, exactly when motion makes it visible.
2. **Positional pan:** offset the camera by the player's screen-relative position
   (player near right edge → camera drifts right), so the figure stays framed and the
   background slides behind it — parallax from ordinary play, no camera input.
3. **Event kicks (juice):** a small impulse on discrete events — a downward jolt on
   landing/taking a hit (`PlayerFlags` invuln bit rising edge), a slight zoom-punch on
   a boost. Decays back over ~0.2s.
4. **Critical damping (dt-corrected — the present rate is variable here by
   construction):** never snap the camera to the target — smooth it toward the
   target each present frame with a wall-clock exponential (`cam += (target − cam) *
   (1 − exp(−dt/τ))`), NOT a fixed per-frame `k_smooth`: B1 makes the present rate
   monitor-dependent, and a fixed per-frame factor is twice as stiff at 120Hz as at
   60Hz — the tuned feel would ship differently on every display. Event-kick decay
   (~0.2s) runs on the same wall-clock basis. The damping is what separates "alive
   and cinematic" from "nauseating and twitchy," and it doubles as the low-pass
   filter that hides the HDMA/velocity noise B1 worries about.

**Provisional formula constants (IMPLEMENT THESE — a monkey needs literals):**
```
τ (damping time constant)     = 0.15 s     // eased but responsive; used in 1 - exp(-dt/τ)
k_run  (max yaw lean)         = 0.10 rad    // at full run speed; scaled by reactive_strength/100
k_pitch(max pitch from velY)  = 0.06 rad    // at full vertical speed
k_pan  (max lateral pan)      = 0.08 (world units) // player-at-screen-edge → full pan
event_kick_magnitude          = 0.05 rad    // landing/hit jolt
event_kick_decay              = 0.20 s      // wall-clock, same exp basis as damping
reactive_strength mapping     : final_gain = (reactive_strength/100) * k_*  // 0 → no sway
```
**`velX_max` (velocity normalizer) — SELF-CALIBRATE, do not hardcode a guess.**
ActRaiser's `PlayerVelocityX/Y` ($08A6/$08A8) are signed int16 (already read this way
at actraiser_rtl.c:346-349), but the run-speed magnitude isn't documented. Rather
than a magic constant: track a running max of `abs(velX)` seen this session (seed
with a safe floor, e.g. 256) and normalize `clamp(velX / velX_max, -1, 1)` against it.
Self-scaling means the lean saturates near real top speed regardless of the actual
unit scale, and it can't over-lean from a wrong guess. Same for velY. (If the author
later measures the true max via the existing velocity read/log at actraiser_rtl.c:376,
it can be pinned to a constant — but self-calibration ships correctly without it.)

**Two camera modes (the architecture — decided).** Reactive camera is **opt-in** and
lives as a distinct mode, not a blend the player accidentally lands in:
- **Free Cam (what exists today):** the manual orbit/zoom from §5.6. The player owns
  the angle; it persists; nothing moves it automatically. This is the creative/
  screenshot tool.
- **Dynamic Cam (new, opt-in):** the reactive system. The player does NOT drive the
  angle directly; gameplay does, swaying around a fixed **designed baseline**.

A `diorama_camera_mode` enum setting selects between them (default **Free Cam** —
opt-in per the author's call). They are mutually exclusive: in Dynamic Cam the manual
orbit controls are inert (or repurposed to nudge the baseline — see below).

**Lock in the Dynamic-Cam baseline (the load-bearing decision).** Reactive sway only
reads well if it sways *around a pose with headroom in every direction it wants to
move*. If Dynamic Cam swayed around whatever Free Cam was left at (could be flat, or
already maxed to one side), the effect would be weak or lopsided. So Dynamic Cam has
its **OWN dedicated baseline**, separate from Free Cam's persisted angle:
- A tuned default pose — a gentle 3/4 tilt with symmetric room to lean left/right and
  pitch up/down (e.g. `baseline_tilt_x ≈ 0.20 rad`, `baseline_tilt_y ≈ 0`, a mid
  distance). Symmetry matters: `baseline_tilt_y = 0` so run-left and run-right lean
  equally; the base pitch sits mid-range so jump and fall both have travel.
- This baseline is its own persisted setting(s) (`diorama_dyncam_baseline_*`),
  independent of the Free-Cam angle. The reactive offsets are computed *relative to
  this baseline*, and the camera eases back to it when the player is idle.
- Optionally, the manual orbit controls in Dynamic Cam can *re-tune the baseline*
  (drag to set your preferred neutral pose, sway happens around it) rather than being
  fully inert — a nice touch, but ship "sway around the fixed default baseline" first.

**State-ownership split (required, not optional).** Today ONE `g_diorama_cam` is
both the render camera and the persistence source: the game thread writes it (mouse
→ `Diorama_AdjustCamera`, which also writes the values back into `g_settings`),
`Diorama_FlushSettingsIfDirty` (game thread, main.c ~2219) persists it, and the
present thread reads it at composite. Dynamic Cam inverts the writer — the present
thread animates the camera every present frame — so split the state: an
**authored/persisted camera** (settings-backed: the free-cam pose and the dyncam
baseline; game-thread-owned, existing flow unchanged) vs an **effective render
camera** (present-side: baseline + ephemeral sway, recomputed per present, never
written back). `Diorama_FlushSettingsIfDirty` must never see a swayed value — it
persists authored state only. Mode transitions follow: Dynamic→Free restores the
persisted free-cam pose; sway state is discarded, never saved.

**Concrete state split (implement exactly this):**
- Keep `static DioramaCamera g_diorama_cam` (diorama.c:443) as the **authored/
  persisted** camera — game-thread-owned, settings write-back + `Diorama_Flush...`
  flow UNCHANGED.
- Add a **present-thread-only** `static DioramaCamera g_diorama_render_cam` in
  `present.c` (or pass it on the stack through the composite call). Each present
  frame: read the authored baseline from the `FrameSlot` (snapshot the active mode's
  base pose + the reactive settings into `FrameSlot` in `FrameSlot_Capture`), compute
  the swayed target, damp `g_diorama_render_cam` toward it (the `1−exp(−dt/τ)` step),
  and hand `g_diorama_render_cam` to `Diorama_Composite` INSTEAD of the authored one.
- `Diorama_Composite`'s camera parameter therefore comes from the render cam in
  Dynamic mode and from the (snapshotted) authored pose in Free mode. The authored
  `g_diorama_cam` is NEVER mutated by the present thread and NEVER reflects sway.

**Design guardrails (so it enhances rather than annoys):**
- **Tiny ranges.** The reactive offsets are a fraction of the manual clamp (e.g.
  ±0.1 rad lean vs the ±0.7 free-cam clamp). Subtle life, not a swinging camera.
  Over-tilt during fast play induces motion sickness — the #1 failure mode.
- **Return-to-baseline.** With no player motion, the camera eases back to the
  Dynamic-Cam baseline (NOT flat, NOT the free-cam angle), so standing still gives a
  stable, composed, readable frame.
- **Respect `diorama_hud_flat` (A5).** If the HUD is flat it doesn't sway — only the
  world planes react (UI stays put, world feels alive).

**Settings — EXACT rows to add (there is no ENUM_SETTING macro; enum settings use a
raw struct literal like `display_mode`/`extended_aspect`, settings.c:562/581).**
- `diorama_camera_mode` — raw `kSettingType_Enum` struct row (copy the
  `extended_aspect` row form, settings.c:581-587): field `&g_settings.diorama_camera_mode`,
  default `kDioramaCam_Free`, min `kDioramaCam_Free`, max `kDioramaCam_Dynamic`, a
  new `kDioramaCamModeLabels[] = {"Free Cam","Dynamic Cam"}` + count, availability
  `Diorama_ModeIsOn`, category `kSettingCat_Presentation`. Add the enum
  `{ kDioramaCam_Free=0, kDioramaCam_Dynamic=1, kDioramaCam_Count }` to settings.h.
- `diorama_dyncam_baseline_tilt_x/_tilt_y/_distance` — three `INT_SETTING` rows
  (scaled ints, same mrad/x100 convention as the existing `diorama_tilt_*_mrad`,
  settings.c:619-626). Availability `Diorama_ModeIsOn`.
- `diorama_reactive_strength` — `INT_SETTING`, range 0-100, availability `Diorama_ModeIsOn`.

**Provisional numeric values (IMPLEMENT THESE LITERALS — do not wait for tuning;
tuning is a follow-up pass, see "Author-blocked" note below):**
```
diorama_dyncam_baseline_tilt_x_mrad  = 200   // ~0.20 rad, gentle 3/4 pitch
diorama_dyncam_baseline_tilt_y_mrad  = 0     // symmetric: left/right lean equal
diorama_dyncam_baseline_distance_x100 = <same default as diorama_distance_x100> // reuse the free-cam default
diorama_reactive_strength            = 35    // 0-100; 0 disables sway
```
All clamped to the same ranges as the free-cam camera settings; all present-thread;
seeded via `Diorama_SeedCameraFromSettings` extended to also load the baseline (B4
state-split below). These are *safe, ship-able* starting values — the effect is
subtle and correct at these numbers; the author may retune later but the monkey
implements exactly these.

**Why this is the right "best utilization":** it inverts the effort model. Instead of
"player must move the camera to see the effect," the effect *responds to the movement
the player is already doing* — running, jumping, getting hit. That's how 2.5D games
(and good camera-shake/lean systems) make depth feel intentional. It's also cheap:
pure present-side math off already-exposed WRAM, no new capture or engine work,
composes with everything already built. **Strong candidate for the highest
gameplay-feel-per-effort item in Phase B.**

**Demo split — B4 is SIX checkpoints; the state-split is an invisible gate:**
- **B4-mode — Dynamic Cam enum row** *(VISIBLE, menu only)*: **Acceptance test:** a
  "Camera mode" row cycles Free / Dynamic and is present only when Diorama 3D is on.
  No render change yet — the row IS the demo.
- **B4-split — authored-vs-render state split** **(REGRESSION CHECKPOINT)**: move the
  render camera to a present-thread `g_diorama_render_cam` fed from a FrameSlot
  snapshot, instead of the composite reading `g_diorama_cam` directly (diorama.c
  ~736). Invisible; it gates every sway. **Acceptance test:** diorama screenshot
  byte-identical, mouse-drag orbit + settings persistence across restart unchanged,
  and `Diorama_FlushSettingsIfDirty` never sees a swayed value. Pass this BEFORE
  stacking any sway.
- **B4-baseline — dedicated Dynamic-Cam pose** *(VISIBLE)*: **Acceptance test:** with
  `reactive_strength=0`, switching to Dynamic snaps to the fixed ~0.20 rad 3/4 pose;
  editing the `diorama_dyncam_baseline_*` rows re-poses it; Reset Camera returns to it.
- **B4-vellean + B4-damp** *(VISIBLE)*: **Acceptance test:** in Dynamic Cam, running
  left/right yaws the camera toward motion (and pitch with vertical velocity), and it
  EASES rather than snapping. Verify identical feel at 60 vs 120/144Hz — the
  `1 − exp(−dt/τ)` damping (B4-damp) is rate-independence, not eyeball-verifiable at a
  single refresh, so it folds into this test rather than being its own checkpoint.
- **B4-pan** *(VISIBLE)*: **Acceptance test:** walking the hero toward a screen edge
  drifts the camera so the figure stays framed and the background slides behind it.
- **B4-kick** *(VISIBLE)*: **Acceptance test:** taking a hit / landing gives a ~0.2s
  decaying downward jolt; a boost gives a slight zoom-punch.

**Sequencing note (corrected):** B4's *quality* benefits from B1's stable-camera-source
work — a reactive camera in constant gentle motion makes interpolation always visible
at >60fps, so B1 and B4 amplify each other. **But for DEMO purposes B4 is NOT gated on
B1b:** the sway reads the WRAM velocity/position/flags on the present side, independent
of scroll interpolation, so B4-baseline/vellean/pan/kick are all visible in Dynamic Cam
whether or not B1b is enabled. B4's only hard prerequisite is B4-split. Preferred order
is still B1b → B4 (for the amplification), but B4 can be demoed with interpolation off.

### B5. Skybox — the diorama floats in a sky, not a void
**Problem:** today the farthest plane is a fixed finite quad (backdrop `g_pixels` at
`z_world=-0.5`, diorama.c:615-657); everything behind/around it is the flat clear
color `(20,20,30)` (diorama.c:715-716). At rest the auto-fit distance roughly hides
this, but the moment the camera tilts/yaws/zooms, the finite quad's edges rotate in
and reveal the dark void at the margins. The diorama reads as flat planes in an empty
box rather than a scene in a world.

**Design (author-confirmed): take the LAST (farthest) background layer as the skybox.**
In ActRaiser Mode 1 that's **BG2** — already captured as its own plane
(`kPpuOverlaySource_Bg2`, diorama.c:549), and confirmed by the code's own comments to
be the sky/distant-scenery layer in action stages (diorama.c:536-541). Instead of
rendering BG2 as an in-box parallax plane, promote it to an **enveloping skybox**:
blow it up to fill the whole viewport *behind* the projected scene, dimmed and DoF'd
so it reads as atmosphere, not focus.
- **Fill the frustum, not a quad.** Draw the skybox as a **full-viewport background
  pass BEFORE the camera-projected layers** — a screen-space quad covering the entire
  output (or an oversized far-plane quad), so no camera tilt/zoom can reveal a void
  edge. This is new: the current backdrop is a normal projected finite plane; the
  skybox must be viewport-filling. Optionally scroll/parallax its UVs slightly with
  camera yaw for a touch of sky motion, but keep it subtle.
- **Dim it (reuse existing machinery).** Per-plane dimming is already per-vertex
  `SDL_FColor` shade (diorama.c:769-774); give the skybox a heavy low-value shade,
  e.g. `{0.30,0.30,0.40,1}` (the existing shades are already blue-biased = a cool sky
  cast). To make the skybox dim INDEPENDENT of the `diorama_depth_shade` slider
  (which scales the per-layer table shades), apply the skybox dim as a fixed vertex
  color not run through `shade_mix`.
- **DoF it (reuse existing machinery).** The Metal blur shader is per-draw-call
  bindable (diorama.c:871-880 binds `g_blur_state`, sets `BlurUniforms.radius`, draws,
  then clears). Bind it for the skybox pass with a large `radius` (max blur) so the
  sky is soft and clearly not the focal plane — exactly like the existing DoF path,
  just at higher strength and on the skybox draw only. Gated behind the same GPU-FX
  availability + fallback (if the shader's unavailable, draw the dimmed skybox
  un-blurred — still far better than the void).

**Three modes, not a toggle (author A/B — enum, not bool).** `diorama_skybox`
(`kSettingType_Enum` raw-struct row per Handover Conventions, `kSettingCat_Presentation`,
availability `Diorama_ModeIsOn`, labels `{"Off","Skybox only","Plane + skybox"}`,
default `kDioramaSky_Off`):
- **Off (`kDioramaSky_Off`, default):** today's look — BG2 is an in-box parallax
  plane; far edge is the finite backdrop quad (void at margins under tilt).
- **Skybox only (`kDioramaSky_Only`):** BG2 is promoted OUT of the box to the
  enveloping dimmed+DoF'd skybox. Fills the void, adds atmosphere, but the box has one
  fewer internal parallax plane.
- **Plane + skybox (`kDioramaSky_Both`, author's third option — likely the winner):**
  BG2 stays an in-box parallax plane AND a copy is *also* drawn as the enveloping
  dimmed+blurred skybox behind everything. You keep the internal parallax depth
  (Off's advantage) AND fill the void (Skybox-only's advantage), and — the specific
  reason it's compelling — **the skybox backstops any transparent gaps/seams in the
  BG2 plane and the widescreen margin edges.** BG2 is captured with alpha (blits
  BLEND over the backdrop, diorama.c:797-798), so it's transparent wherever it has no
  tiles; in the box that shows the void/backdrop through the holes, and at the level's
  off-screen margins the seam shows. A dimmed, blurred, viewport-filling copy of the
  same layer sitting behind it fills exactly those gaps with content that *matches*
  (it's the same sky), so seams read as "distant sky continuing" rather than "hole."
  Cost: BG2's texture is drawn twice (once in-box sharp, once as skybox
  dim+blur) — cheap (one extra full-viewport textured quad + one blur pass).
**Why enum, not three bools:** the modes are mutually exclusive views of the same
layer; an enum is the right primitive (and matches how `display_mode`/`extended_aspect`
model mutually-exclusive presets, settings.c:562/581). Ship default Off so the A/B
starts from today's known-good look; the author compares all three.

**Note for B6 (shoebox):** "Plane + skybox" also composes best with the box — the
skybox fills the box's far opening while the walls mask the side/top/bottom edges, so
between them there's no void from any angle, and BG2's own transparency gaps are
backstopped. If the author keeps the shoebox, `kDioramaSky_Both` is the natural
pairing.
**Caveat (bake in):** BG2 is a *heuristic* sky, not a guaranteed one — there is no
programmatic "sky" flag (confirmed: the `AR_WS_ONLYBG` probe exists precisely because
which layer is sky is discovered per-scene, actraiser_rtl.c:812-814). In scenes where
BG2 is pillars/fog, skybox mode will blow those up dimmed+blurred behind the scene —
usually still fine as ambient backing, but it's why this is opt-in, not default.
**Demo / acceptance test:** *(VISIBLE)* in diorama + action stage, cycle the Skybox
enum: on **Off**, yaw/tilt/zoom the camera and the dark void (RGB 20,20,30,
diorama.c:715) rotates in past the finite backdrop quad's edges; **Skybox only** and
**Plane + skybox** promote a dimmed viewport-filling BG2 so no angle reveals the void,
and **Plane + skybox** additionally backstops BG2's transparent gaps. Requires GEO
(the viewport-fill quad); the DoF-blur half needs an `AR_GPU_SHADERS` build, but the
dim + void-fill core shows on every build.

**KNOWN LIMITATION, shipped as-is (live report + investigated, 2026-07-21):** near a
level's start/end, the captured BG2 content goes black at the world-bound edge instead
of extending, visible as a black wedge clipping the skybox. Root cause: the widescreen
margin ceiling (`extraLeftCur`/`extraRightCur`, `ActRaiser_ApplyWidescreenPolicy`,
actraiser_rtl.c ~908-925) is a single GLOBAL PPU value derived from BG1's world
position — every layer's scanline rendering shares it, and the existing per-layer
knobs (`wsLayerClamp`/`wsLayerMirror`/`wsLayerRepeat`, consumed by `PpuLayerExtra`,
ppu.c ~419) can only shrink a layer's margin down to 0 from that ceiling, never extend
past it. Real fix needs either (a) a new per-layer NUMERIC margin ceiling in
`PpuLayerExtra` (touches a hot per-scanline path in core PPU rendering — author's
preferred direction, more performant than the alternative), or (b) a second BG2-only
scanout pass per frame just for this capture, run with the ceiling forced to the full
budget + mirror/repeat. Deferred as its own follow-up item, not part of B5's scope.

### B6. Shoebox enclosure — floor, ceiling, and side walls
**Idea (author):** put the layer stack inside an actual box — a floor, a roof, and
side walls — so the level's off-screen edges are masked by box surfaces instead of
ending in void, like looking into a shoebox. The near-camera side wall (when yawing
on Y) hides so it doesn't occlude the view in.

**Geometry (grounded — this needs a NEW mesh builder).** The layers live in world
space X∈[-0.5,+0.5]·aspect_x, Y∈[-0.5,+0.5], Z (z_world) ∈ [-0.50 backdrop … +0.45
HUD] (diorama.c:626-627,776). The box walls span the Z axis, so:
- **Floor** = quad at `y=-0.5`, x∈[-hx,+hx], z∈[z_back,z_front]. **Ceiling** = `y=+0.5`.
  **Side walls** = `x=±hx`, y∈[-0.5,+0.5], z∈[z_back,z_front]. Use `hx = 0.5*aspect_x`
  and `z_back=-0.50 / z_front=+0.45` so the walls line up exactly with the layer
  quads' edges.
- **`BuildLayerMesh` CANNOT build these** — it hardcodes a constant `z_world` on every
  vertex and varies only X/Y (diorama.c:626-632). The walls vary a different axis
  pair. **New code required:** factor the projection kernel (diorama.c:628-635) into a
  shared `ProjectWorldPoint(mvp, x,y,z, w,h) -> SDL_FPoint`, then add
  `BuildQuadMesh(mvp, origin_corner, edgeU, edgeV, uv..., subdiv, color, ...)` that
  lerps an arbitrary world quad from a corner + two edge vectors and reuses the
  existing index/triangulation loop (diorama.c:644-655) verbatim. The camera/MVP math
  is already fully general (`BuildViewProjection`, diorama.c:583-603) — only the mesh
  generator is plane-specialized, so this is additive, not a rewrite.

**Wall texturing / color:** simplest shippable version is flat-shaded walls (a dark
neutral or a color sampled from the backdrop, drawn with vertex-color shade like the
layers) — reads as a physical box interior. Fancier (later): extend the floor
downward from BG1's bottom edge with a dimmed slice of the ground tiles so the floor
looks like the level's own terrain folding into 3D. Start flat.

**Near-wall culling (grounded).** When the camera yaws (`tilt_y`), the near side wall
would occlude the view into the box, so it must hide. The camera is
`{tilt_x, tilt_y, distance, fov_y}` (diorama.c:436-440); **the sign of `tilt_y`
directly tells you which side wall is near** — no dot-product needed for a simple
box: yaw one way → right wall faces the camera (cull it), yaw the other → left wall.
Rule: **draw only the FAR side wall** (the one whose X-sign is opposite the lean), and
fade it in/out near `tilt_y≈0` so the transition isn't a hard pop. Floor and ceiling
are always drawn (yaw doesn't bring them toward the camera; extreme pitch could, but
the ±0.7 clamp keeps them safe — revisit only if pitch range grows).

**Draw order (grounded — painter's algorithm, no depth buffer).** `SDL_RenderGeometry`
does NOT depth-test; the diorama relies on **draw order** (the layer table is authored
back-to-front, diorama.c:542-563). So the box must slot into that order:
`skybox (B5) → back wall/floor/ceiling → [layers back-to-front, unchanged] → near
wall (culled/faded)`. The far walls draw before the layers (they're behind/around the
stack); nothing draws in front of the nearest layer except the flat HUD/overlays.

**Toggle:** `diorama_shoebox` (BOOL_SETTING, `kSettingCat_Presentation`, default OFF,
availability `Diorama_ModeIsOn`). Composes with B5: skybox fills the far opening of
the box; the box walls mask the level's side/top/bottom edges. Independent toggles so
each can be A/B'd alone.

**Demo split — the geometry factor-out is a separate invisible gate:**
- **GEO — `ProjectWorldPoint` factor-out + `BuildQuadMesh`** **(REGRESSION
  CHECKPOINT — shared with B5):** extract the projection kernel from `BuildLayerMesh`
  (diorama.c ~628-635) and add `BuildQuadMesh`; the MVP math is already general so
  routing `BuildLayerMesh` through it must be pixel-identical. **Acceptance test:**
  screenshot-diff the diorama in an action stage before/after — byte-identical.
  `BuildQuadMesh` is dead code until B5/B6 call it. **Near-wall culling belongs to B6,
  NOT here** — do the pure factor-out only at this checkpoint.
- **B6 — walls + near-wall culling (ship TOGETHER)** *(VISIBLE)*: **Acceptance test:**
  toggle `diorama_shoebox` — the layer stack sits inside a visible box (floor,
  ceiling, side walls) masking the off-screen margins, and yawing the camera looks
  correct because the near side wall culls/fades. Walls without culling demo cleanly
  only at rest / far-side yaw; near-side yaw is visibly wrong (the near wall occludes
  the view in) — so walls and culling are one checkpoint, not two.

**Sequencing:** B5 and B6 both build on the same generalized-geometry step (the
`ProjectWorldPoint`/`BuildQuadMesh` factor-out B6 needs also makes the viewport-fill
skybox quad trivial). Do the geometry factor-out once, then B6 (walls) and B5
(skybox) in either order. Both are pure present-side diorama rendering — no engine,
no capture changes beyond BG2 already being captured. They are the "make it look like
a crafted object" polish tier, sequenced after B1/B4 (the motion/feel tier).

---

## Track M — Settings menu organization & visual polish

Grounded against `settings.c` / `settings.h` / `settings_overlay.c` at `cc0b042`.

### The sprawl, quantified
The overlay's left-column nav order is `kCategoryOrder[]`
(`settings_overlay.c:147-156`): **Display, Presentation, Audio, Widescreen,** Cheats,
Save, Extras, Inspector. Graphics-related settings total **31 rows across 3 nav
sections that are NOT adjacent** — Audio is wedged between Presentation and
Widescreen:
- **Display (10 rows)** — mixes window plumbing (`window_scale`, `fullscreen`,
  `new_renderer`, `ignore_aspect_ratio`), aspect (`extended_aspect`, `pixel_aspect`),
  scaling (`hud_scale_percent`, `menu_scale_percent`), asset swap (`hd_replacements`),
  and `display_mode` — which is itself just a *preset over the 9 Widescreen flags*.
- **Presentation (12 rows)** — 100% diorama (camera, layer toggles, depth-shade,
  reset). The label "Presentation" is generic; the content is entirely "Diorama 3D."
- **Widescreen (9 rows)** — the granular `ws_*` flags that `display_mode` (over in
  Display) presets. So a related pair is split across two non-adjacent sections.

(Correction to the `cc0b042` commit message: it claims a "Graphics" menu, but there
is no `kSettingCat_Graphics` — it added `kSettingCat_Presentation`, and the
interpolation/shadow/blur/DOF/rim effects it mentions are **env-var only**
(`AR_INTERP_ENABLE`, `AR_GPU_SHADERS`, `AR_GPU_FX_*`), not menu rows. See A6.)

### M1. Regroup graphics into one coherent Video hierarchy
**Proposal:** a single top-level **Video** nav section with ordered sub-groups, so
everything graphics is in one place and related things are adjacent:
- **Output** — `window_scale`, `fullscreen`, `new_renderer`, `ignore_aspect_ratio`
- **Aspect** — `extended_aspect`, `pixel_aspect`, `display_mode`
- **Widescreen** — the 9 `ws_*` flags (now adjacent to `display_mode` that presets
  them)
- **Diorama** — the 12 diorama rows (rename "Presentation" → "Diorama", its actual
  content)
- **HUD** — `hud_scale_percent`, `menu_scale_percent`, + the new `diorama_hud_flat`
  (A5)
**Mechanism:** the overlay already keys sections off `kCategoryOrder` + a submenu
render; the smallest path is either (a) reorder `kCategoryOrder` so the 3 graphics
categories are adjacent and rename Presentation→Diorama (near-zero risk, ~1 array +
1 string), or (b) a deeper sub-grouping within one Video category (more work — the
overlay's `DrawMenu` would need an intra-category header/group concept it doesn't
have today). **Recommend (a) first** (adjacency + rename buys most of the clarity for
almost nothing), (b) only if the author wants true nesting. Watch: `ACTION_SETTING`
hardcodes `kSettingCat_Extras` (predecessor §10.3) — the diorama `reset` action
already uses `PRESENTATION_ACTION_SETTING`, so category-scoped action macros are an
established pattern to copy if actions move.
**Demo split — two checkpoints:**
- **M1(a) — reorder + rename** *(VISIBLE)*: **Acceptance test:** open the settings
  menu — Display / Diorama / Widescreen are adjacent (Audio no longer wedged between
  them), and the section formerly labelled "Presentation" now reads "Diorama." Reorder
  `kCategoryOrder` (settings_overlay.c ~147-156) + rename the label in
  `Settings_CategoryName` (settings.c ~1217). Directly eyeball-verifiable.
- **M1(b) — intra-category nesting — SHIPPED, as a section/tab split rather than
  inline group headers.** Nesting landed the other way up from the sketch above: a
  `SettingCategory` is now one *tab*, and a new host-side `kSections[]` table
  (`settings_overlay.c`) groups tabs under eight nav sections — Video, Diorama,
  Town 3D, Audio, Controls, Cheats, Save, System — down from 13 nav rows (11
  categories + the promoted Restart/Exit leaves, which are now the last two rows of
  System > Tools). Tabs cycle with L/R on a pad and Q/E, `[`/`]`, Tab on a keyboard,
  and Left/Right from the nav column.
  Inline headers were rejected: they cost a row each and still leave one 45-row list
  to scroll. Instead the oversized categories were split at the source into
  panel-sized ones (`kSettingCat_DioramaCamera`, `SimCamera`, `SimLighting`,
  `SimAtmosphere`, `InputBinds`) — one token per descriptor, since `BOOL_SETTING` /
  `INT_SETTING` already take the category as a parameter.
  The two ad-hoc in-list page selectors (`save_editor_page`, `input_bind_page`) are
  now driven by a tab's optional `page_key`/`page_value`, and stop listing themselves
  as rows (`Settings_IsMenuVisible`). **Watch:** that sync is a direct field store,
  NOT `Settings_SetLong` — row enumeration runs inside `SettingsOverlay_Render`,
  which is on the present thread (`present.c`), and the host change observer
  quiesces that thread. Routing it through the mutation API self-deadlocks.
- **M1(c) — presentation polish, SHIPPED alongside.** 16x16 per-section icons in a
  dedicated ARGB atlas (`CreateIconAtlas`) baked in each section's accent color,
  which also tints that section's title, tab underline, selection bar, rules and
  scrollbar. The description panel, tab bar, status and hint line moved to the 6x8
  small font (real lowercase authored into `kFallbackFont`, now 8 rows so descenders
  fit), which fits four wrapped lines of tooltip instead of three truncated ones and
  takes a free-form color via `SetTextureColorMod`. Row and nav scroll arrows became
  proportional scrollbars, returning a text cell to the value column. The
  description panel also names the row's apply kind (Live / Restart required /
  Unavailable in this mode).
  `AR_OVERLAY_PREVIEW_DIR=<dir>` on the overlay test dumps one BMP per (section,
  tab) — the practical way to eyeball a layout change across the whole menu.
- **M1(d) — no text entry for numeric rows, SHIPPED.** Every `kSettingType_Int`
  row (50 of them: scales, camera mrad/x100, percentages, deadzones, ...) is now
  adjusted by Left/Right with **hold-to-accelerate** and never opens the text
  editor. `SettingsOverlay_Tick()` (main.c's paused loop, main thread — a write
  on the present thread would deadlock the quiesce) steps the held row every
  ~55ms after a ~350ms initial delay, growing the step from the base (fine tap)
  to a range-proportional coarse amount (`HoldStepMultiplier`, coarse ≈
  NiceStep(range/24)) so a 0..2000 range crosses in ~1s while a tap still moves
  by one. Changes apply live; the settings.ini write is deferred to release so a
  fast hold isn't one disk write per frame. Confirm (B) on a numeric row is a
  single fine step, not a prompt.
  **Text entry remains only for the genuine non-numeric holdouts** — nowhere to
  eliminate it without a character/hex picker that isn't worth building: player
  name (`save_player_name`), PAR pins (`pins`), and the developer hex layer mask
  (`sim3d_diagnostic_layers`, Mask). `warp_target` stays hidden from the menu.
  Test seams: `SettingsOverlay_HoldStepForTest` (pure curve),
  `SettingsOverlay_TickAtForTest` (injected clock), `SettingsOverlay_IsEditing`.
- **M1(e) — "Show debug settings" gate, SHIPPED.** A `show_debug_settings` Bool
  (System > Tools, default off) collapses developer-only rows out of the menu so
  players see only the master toggles and the major on/off effects they tune for
  performance. `Settings_IsDebugOnly(desc)` (settings.c) is the classifier — no
  per-descriptor flag: it returns true for every Int/Mask row in the six
  diorama/town categories (the mrad/px/pct/deg dials + the SIM diagnostic mask),
  a short key list of internal A/B bools (`sim3d_separated_composite`,
  `sim3d_cull_lift_inset`, `sim3d_picker_exit_ease`, the `diorama_layer_*`
  toggles, `diorama_hud_flat`), and the whole Inspector category. Master
  toggles, the on/off effect switches, camera-mode, and reset stay.
  `Settings_IsMenuVisible` checks it first. In the overlay a tab whose rows are
  all hidden **collapses** (`RawTabHidden`/`VisibleTabCount`): debug-off, Town 3D
  shows Scene/Camera only, System loses its Inspector tab (no strip). To hide a
  future dev setting, make it Int/Mask in a 3D category or add its key to
  `kDebugKeys` — nothing per-row. Verified with a debug-off contact sheet
  (`AR_OVERLAY_PREVIEW_DIR` now emits `-dbgoff` variants).

### M2. Value-vs-key text colors (CHEAP — machinery already exists)
**Key finding:** the overlay ALREADY has a multi-palette text system. `TextStyle`
enum `{ kText_Normal, kText_Dim, kText_Warning }` (`settings_overlay.c:63-68`); color
is BAKED per-style by building one pre-tinted 128×128 ARGB atlas per palette
(`kTextPalettes[kTextStyle_Count][4]`, ~73-80; `CreateFontAtlas(style)`, ~391;
`s_font_textures[]`, ~186). The glyphs are 3-tone (outline/shadow/face), so a uniform
`SetTextureColorMod` would muddy them — the per-baked-atlas approach is why the code
does it this way, and it's the right tool.
**The gap:** a menu row draws label AND value with the SAME `style` —
`DrawTextN(...desc->label..., style)` (`:1456`) and `DrawTextRight(...value..., style)`
(`:1457-1458`).
**Fix (tiny, ~3 lines) — with the EXACT palette to add:** `kTextPalettes` rows are
4-tone `{ transparent, outline, shadow, face }` ARGB (settings_overlay.c:73-80). Add
a `kText_Value` row that keeps the same transparent + dark outline as Normal but
swaps the shadow/face to a cool cyan so values read distinct from warm-white labels.
**Reuse the cyan already in-tree** (`ARGB(255,92,196,255)`, used by the resize-grip at
settings_overlay.c ~1610) for palette harmony:
```c
// append to kTextPalettes[], and bump kTextStyle_Count / add kText_Value to the enum.
// 4-tone row {transparent, outline, shadow-mid, face} — mirror kText_Normal's structure:
{ ARGB(0,0,0,0), ARGB(255,0,0,0),
  ARGB(255,40,120,140), ARGB(255,92,196,255) },   // kText_Value — cool cyan face
```
Then one more atlas builds at load (the `for i < kTextStyle_Count` loops at ~471/851
already cover it). **Route the value through it CONDITIONALLY — do NOT hardcode
`kText_Value` at `:1457`:** the row's `style` becomes `kText_Dim` for unavailable/
disabled rows (settings_overlay.c ~1432-1433), and hardcoding cyan would light up
greyed-out rows in bright cyan (contradicting M4's dim-when-unavailable intent). Use:
`DrawTextRight(..., value, value_chars, style == kText_Normal ? kText_Value : style)`
— so a value is cyan only when its row is normal/enabled, and stays dim when the row
is dim. Leave the label's `DrawTextN` at `:1456` on plain `style`.
**Highest readability-per-effort change in the doc.** (Precedent: the debug panel
already does live per-style tinting via `SetTextureColorMod` on its monochrome atlas,
`DrawDebugGlyph` ~1136-1155 — proof the multi-color intent is established.)
**Demo / acceptance test:** *(VISIBLE)* open any settings section — values render in
cool cyan while their labels stay warm-white, and greyed/unavailable rows keep both
label and value dim (proving the conditional routing). Directly eyeball-verifiable; the
cheapest visible win — do it early.

### M3. Icons / graphics beside labels
The overlay already draws non-text graphics host-side: a ROM-decoded 24×24 dialog
frame (`DrawDialogPanel`/`CreateDialogFrameTexture` ~527-571/1055-1090), solid
highlight/separator rects, and font-glyph pseudo-icons (blinking `>` cursor, `*`
restart marker). Two routes for real icons:
- **Glyph-atlas route (cheapest):** author a handful of 8×8 icon "glyphs" into the
  fallback/supplemental font mechanism (`kFallbackFont`, ~85) and blit them via the
  existing `DrawGlyph` path beside category labels (monitor→Video, speaker→Audio,
  etc.). No new asset pipeline, no new load path — reuses everything.
- **PNG icon-atlas route (more flexible art):** the `stb_image` decoder is in-tree
  (`main.c:50`, used by `LoadHdReplacements` at `main.c:1621-1681` — `stbi_load` →
  `SDL_CreateTexture` → `UpdateTexture`). Copy that pattern for a small host icon
  atlas. Do NOT route icons through the HD manifest — it's WRAM/BG-mode-gated for
  in-game capture, wrong tool for UI (confirmed).
**Layout:** rows sit on a fixed glyph grid (`kGlyphSize` steps, `label_x`,
`value_right`); a left icon inset is a bounded reflow, not a rewrite.
**Recommend:** category icons via the glyph-atlas route first (cheapest, biggest
navigational win — you scan sections by icon), per-row icons only if they still add
value, richer PNG art as optional polish.
**Demo / acceptance test:** *(VISIBLE)* open the settings menu — a small icon draws to
the left of each category name in the nav column (monitor→Video, speaker→Audio, etc.).
Directly eyeball-verifiable. Do after M1/M2.

### M4. Selection/focus already has color hierarchy to extend
A selection highlight exists (`kHighlight` fill behind the selected row, ~1425-1428)
and the cursor uses `kText_Warning`. The key/value/selected color hierarchy (M2)
should be designed as one coherent palette with these — e.g. label=normal,
value=cyan, selected-row-value=brightened, restart/warning=amber, dim=disabled/
unavailable rows (the availability-gated diorama rows already exist and could render
`kText_Dim` when their gate is false, making "why is this greyed" self-evident).
**Demo / acceptance test:** *(mostly not a deliverable)* the highlight/cursor hierarchy
already ships (`kHighlight` fill, `kText_Warning` cursor). The one net-new bit — the
selected row's VALUE brightening — folds into M2's palette work; verify there (the
selected row's value reads brighter than unselected values). Nothing standalone to
build here.

### M5. Settings correctness — widget/range/default hygiene
A settings-correctness sweep (every descriptor's declared type/range/default vs its
field type and the overlay's widget behavior) confirmed the interaction is
type-driven: `BeginEditing` (settings_overlay.c ~702-704) makes **Bool/Enum/Action
toggle-or-cycle** and **everything else (Int/Mask/Custom) TEXT-EDITABLE**. So the
correctness rule for a new/existing setting is: *if it's semantically on/off, it must
be `kSettingType_Bool`, not `Int` with a 0..1 range* — an Int-0..1 is typeable, which
is wrong for a toggle. (The cited "Moonjump" case is already correct — it IS
`kSettingType_Bool`; its parse fn only affects env/ini load, not the menu widget.)
Confirmed defects to fix (both low, both in the shipped diorama camera rows):

- **`diorama_distance_x100` reachable "dead zone" 1..199 (real, visible).** 0 is the
  auto-fit sentinel (render auto-fits only when `distance <= 0.0`, diorama.c:737) and
  the usable min is `kDioramaDistMin=2.0` (=200), but the descriptor range is a
  contiguous 0..2000 with `step=1`, so values 1..199 (0.01x..1.99x) are reachable by
  keyboard/typing and pass straight through. A **single right-arrow from the default
  0 sets distance=1 → 0.01 world units**, inside the near plane (kNear=0.1) → the
  whole scene clips into a broken frame. The range can't encode "0 OR ≥200", so
  **enforce the floor at consume time:** in `Diorama_Render`, next to the auto check
  (diorama.c:737), add `if (cam.distance > 0.0f && cam.distance < kDioramaDistMin)
  cam.distance = kDioramaDistMin;`. Optionally also give the row a coarse step (below)
  so a single press can't land in the gap.
- **Camera rows have `step=1` → arrow-cycling is uselessly fine (UX friction, low).**
  `diorama_tilt_x/y_mrad` (±700) and `diorama_distance_x100` (0..2000) use
  `INT_SETTING`, which hardcodes `step=1`, so each arrow press moves 1 mrad / 0.01x —
  ~1400 presses to traverse tilt. Values are still reachable (type or mouse-drag), so
  low. **Fix:** write these three rows as full descriptor literals with a coarse
  `step` (e.g. 10-25 mrad tilt, 25 distance) instead of the `step=1` `INT_SETTING`
  macro — the codebase already does exactly this for `hud_scale_percent`/
  `menu_scale_percent` (step 25, settings.c:571/576), so it's an established pattern,
  not new machinery.

**Demo split — two checkpoints, different visibility surfaces:**
- **M5-coarse-step** *(VISIBLE in the MENU — no ROM/action stage needed)*:
  **Acceptance test:** enable `diorama_mode` first (so the camera rows aren't
  `Diorama_ModeIsOn`-greyed), then arrow the Camera pitch/yaw/distance rows — each
  press now jumps by a coarse step (10-25 mrad / 25 x100), not 1. The overlay reads
  `desc->step` (settings_overlay.c ~764), so the change shows immediately in the menu.
- **M5-dead-zone-floor** *(VISIBLE only in-game)*: **Acceptance test:** clamp to
  `kDioramaDistMin` at consume time (diorama.c:737); with the fix, distance values in
  1..199 that today clip the scene into a broken near-plane frame instead render a
  valid scene. Requires ROM + diorama in an action stage to see.

**Coverage — the typeable-toggle question is now cleared.** The parse/format-and-
editability lens (the one aimed at "typeable things that should be toggles") initially
errored out, so it was re-run standalone: it inventoried every Int/Mask/Custom row and
every parse/format-fn setting (a genuine ~7-tool, ~65K-token pass) and confirmed **no
settings are wrongly typeable** — every text-editable row is legitimately arbitrary-
valued (e.g. `cheat_inf_hp` "larger values are literal HP", `pins` arbitrary PAR codes,
`cheat_no_knockback` raw hex offsets, the scaled-int camera values), and `Moonjump` is
already a proper `kSettingType_Bool` (toggles, not typeable — its parse fn only affects
env/ini load). So M5's confirmed defect list is exactly the two camera-row items above;
the broader "on/off settings wearing a typeable widget" concern checked clean across
the whole descriptor table. **M5 is complete.** (Keep the Bool-vs-Int-0..1 rule at the
top of M5 as the standing convention for any NEW setting added later.)

## Appendix: suggested sequencing

```
Phase A (baseline)         : A1 (F9 quiesce) first — it's a real race.
                             A2/A3/A4 are trivial cleanups, batch them.
                             A7 (widescreen HUD anchoring in diorama) — shipped bug;
                             lands the PresentHudOverlay-in-diorama plumbing A5 needs.
                             A5 (HUD-flat) — build on A7; ship default = true (readable).
Track M (menu)             : M2 (value/key colors) is the cheapest win — do early.
                             M1 (regroup) alongside, since B2 adds video rows.
                             M3 (icons) after M1/M2.
Phase B (features)         : B1a (present pacing) is ALREADY shipped + mode-agnostic
                             — just expose uncapped_framerate as a setting (with A6).
                             B1b (diorama motion interp source fix) is the headline
                             feature; do after A.
                             B4 (reactive camera, diorama-only) right after B1b — they
                             amplify each other; B4 is the "best utilization" answer.
                             B2 — build the decided toggles (uncapped_framerate,
                             scroll_interpolation); SKIP the author-blocked items
                             (integer-scale, CRT/scanline, vsync exposure).
                             B3 + flat-B1b (2D depth effects + flat motion) SHARE the
                             separated-plane plumbing — build it once, depth-shade first.
                             B5/B6 (skybox + shoebox) = the "crafted object" polish
                             tier; after B1/B4. Share a geometry factor-out
                             (ProjectWorldPoint + BuildQuadMesh) — do that once, then
                             walls + viewport-fill skybox in either order.
```

A1 and Track M are independent of B1 and can proceed in parallel. **B1a already
benefits flat (non-diorama) play** (latency/pacing) with no new work beyond a
setting; **B1b (motion smoothness) is diorama-only until the B3 plumbing lands**,
after which flat mode gets it too via source-rect shift. B4 (reactive camera) is
diorama-only (needs Z-separated planes); its *quality* is amplified by B1b's stable
camera source but its *demo* is not gated on it (see B4's corrected sequencing note) —
B1b → B4 is still the natural order and the highest-impact pair for making diorama
mode feel alive.

### Demo-checkpoint sequence (each ends in something you can SEE or TEST)

Ordered so every step lands a visible/testable result; invisible cleanups are folded
into an adjacent visible chunk, and the three **REGRESSION CHECKPOINT** refactors are
gated (pixel-identical / unchanged) before any feature stacks on them.

1. **A1 — F9 quiesce race** *(no eyeball demo)* — F9 still cycles all display modes;
   rapid-F9 with the present thread live is crash/glitch-free and ThreadSanitizer-clean.
   Do first; independent.
2. **M2 + A4 (folding A2/A3)** — settings values render cool cyan while labels stay
   warm-white (greyed rows stay dim), and the phantom "Sprite upright" row is GONE.
   (A2/A3 verified by clean build + Reset-Camera pose-unchanged diff.)
3. **M1(a) — regroup + rename nav** — Display / Diorama / Widescreen adjacent (Audio no
   longer wedged), and "Presentation" now reads "Diorama."
4. **M3 — category icons** — a small icon draws left of each category name.
5. **A7 — widescreen HUD anchoring in diorama** — in diorama + a WIDESCREEN action
   stage, ACT/TIME/SCORE spread to the screen edges and boss health spans full width
   (matching flat mode), instead of a centered 256-wide tilted strip. Lands the
   `PresentHudOverlay`-in-diorama plumbing A5 builds on; do it before A5.
6. **A5 + M5** — MENU (no ROM): with diorama_mode on, arrowing Camera pitch/yaw/distance
   jumps by a coarse step, not 1. IN-GAME (diorama + action): the now-anchored (A7) BG3
   status bar stops keystoning and a "Flat HUD" toggle switches it live; distance 1..199
   no longer clips.
7. **B2 uncapped row + B1a** *(no eyeball demo)* — an "Uncapped framerate" row appears
   and cycles; pacing/latency verified by `AR_PERF` at 120/144Hz (NOT by eye — wire the
   toggle to a real vsync/cadence mechanism or it's inert). (A6 discharged here.)
8. **B1b — motion-interp source fix + scroll_interpolation row** — a "Scroll
   interpolation" row appears; ON in diorama+action, BG2 parallax stops vibrating while
   static, and on a >60Hz display motion glides.
9. **B1b-crisp — ×4 supersample AA** — tilt the camera: high-contrast pixel-art edges on
   tilted quads stop shimmering (visible with interpolation off; best as a still A/B).
10. **B4-mode — Dynamic Cam enum row** — a "Camera mode" row cycles Free/Dynamic, present
    only when Diorama 3D is on. (No render change yet — the row is the demo.)
11. **B4-split + B4-baseline** **(REGRESSION CHECKPOINT)** — REGRESSION: Free Cam drags +
    persists byte-identical, screenshot diff unchanged, Flush never sees sway. VISIBLE:
    with reactive_strength=0, Dynamic snaps to a fixed ~0.20 rad 3/4 pose; editing
    baseline rows re-poses it; Reset returns to it. Don't stack sways until this passes.
12. **B4-vellean + B4-damp** — in Dynamic Cam, running left/right yaws the camera toward
    motion and it EASES (not snaps); verify identical feel at 60 vs 120/144Hz.
13. **B4-pan** — walking the hero toward a screen edge drifts the camera so the figure
    stays framed and the background slides.
14. **B4-kick** — a hit/land gives a ~0.2s decaying downward jolt; a boost gives a slight
    zoom-punch.
15. **GEO — geometry factor-out** **(REGRESSION CHECKPOINT)** — after routing
    `BuildLayerMesh` through `ProjectWorldPoint`, diorama output in an action stage is
    PIXEL-IDENTICAL (screenshot diff). Shared prereq for B5+B6; `BuildQuadMesh` is dead
    until called.
16. **B5 — skybox enum** — cycle Off / Skybox only / Plane + skybox: void rotates in at
    the margins on Off; the other two promote a dimmed viewport-filling BG2 so no angle
    reveals the void (Both also backstops BG2's transparent gaps). (DoF-blur needs an
    `AR_GPU_SHADERS` build; the dim + void-fill core shows on every build.)
17. **B6 — shoebox walls + near-wall culling** — toggle `diorama_shoebox`: the stack sits
    inside a visible box masking off-screen margins, and yaw looks correct because the
    near side wall culls/fades. (Walls + culling ship together.)
18. **B3-plumbing — flat separated-plane refactor** **(REGRESSION CHECKPOINT)** —
    `flat_depth_enhance` appears (default OFF); with it ON, flat output shows no visible
    regression, instrumentation confirms per-layer buffers populate, color-math
    divergences catalogued (NOT pixel-identical — per-layer capture loses cross-layer
    color math).
19. **B3-depth-shade** — with the flag ON in normal 2D view, farther layers render
    darker/cooler; the `diorama_depth_shade` slider (made reachable in flat mode) changes
    strength.
20. **B3-shadows** — each nearer flat layer casts a subtle dark offset shadow onto the one
    behind.
21. **B3-parallax (PARTIAL)** — on scroll, background layers move at different rates in
    normal 2D view. (Pin scope vs flat-B1b's source-rect shift on the shared plumbing.)
