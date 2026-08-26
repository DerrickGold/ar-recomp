<!-- Authored 2026-07-26 by a 4-lens investigation + 45-agent adversarial verify
     (34 findings confirmed, 6 refuted). Fix order: A, then C, then B (B is
     inert wherever A applies). The lead's original one-line "narrow the skybox
     UV" idea is Fix B here -- correct, but only the FALLBACK, and only for
     Skybox-only mode. -->

# Diorama skybox / backdrop margin clipping — fix design

**Status: IMPLEMENTED and confirmed in play (2026-07-28).** All three fixes
landed in `b9dc4f3` ("fix: edge margin black wedge at level bounds, behind an
A/B toggle"); the black wedge at level bounds is no longer observed.

**BH6 supersession (2026-08-09):** the root-cause and A/C implementation below
remain current, but B's scalar handoff is now historical. Commits `f13d19f` and
`963b4cb` replace `DioramaBg2MarginSource`, `bg2_margin_source`, PPU-mask reverse
classification, and the one-span API with the resolved `ActionBgPlan` carried
through `FrameSlot` plus exact row-banded spans. The old names below are kept as
the original fix design, not as live API guidance. See `SPEC-bg-hle.md` BH6 and
`docs/rendering-engine.md` for the current contract.

**Per-layer extent supersession (2026-08-10):** mirror/repeat remains the edge
source described here, but it no longer implies that every row occupies the
whole canvas. `SPEC-bg-layer-extents.md` is authoritative for the independent
presentation cap: Bloodpool's upper moon/cloud family is now fixed to the
authentic viewport while its `136..224` water band remains repeat-available.

| Fix | Where it lives |
| --- | --- |
| **A** — pad captured layers out to the full margin budget instead of the live margin | `actraiser_rtl.c:952`, `ppu.c` merge loops |
| **B** — crop each skybox row band's UV to BG2's actually valid span | `diorama_skybox_uv.c/h`, resolved plan latched via `FrameSlot` |
| **C** — compositor writes only the ACTIVE window; gap strips take the scene backdrop, not black | `actraiser_ws_gap.c/h`, `actraiser_rtl.c:1062` |

`g_settings.diorama_margin_fix` is a **live A/B** for the whole artifact — Off
restores every pre-fix path exactly, which is how the fix was verified and how a
regression would be confirmed. Covered by `actraiser_ws_gap_test.c`,
`diorama_skybox_uv_test.c`, `ppu_render_pipeline_test.c` and `settings_test.c`.

Everything below is the original design document, kept for the reasoning behind
the code — in particular §1's verifier corrections, which are still the
authority on why the fallback ordering is A → C → B.

## 1. Root cause (corrected)

At a level bound the PPU's **live** per-side margin collapses (`ActRaiser_ApplyWidescreenPolicy`, `src/actraiser/actraiser_rtl.c:952-969`: `margin_left = min(camera_x, g_ws_extra)`), so scanout only ever writes screen x in `[-extraLeftCur, 256+extraRightCur)` — while every diorama consumer maps the **fixed** 446-column capture span. Two distinct consumers then render the never-written columns as *opaque* black: `DrawDioramaSkybox` maps U over the whole span (`src/diorama/diorama.c:950,970-971`) and forces `SDL_BLENDMODE_NONE` (`src/diorama/diorama.c:987`), and the flat framebuffer's gap strips are explicitly zeroed every frame (`src/actraiser/actraiser_rtl.c:1039-1052`) and drawn as `kDioramaPlane_Backdrop`, also `SDL_BLENDMODE_NONE` (`src/diorama/diorama.c:1351-1352`). Critically, the KNOWN LIMITATION comment at `src/diorama/diorama.c:904-922` is **wrong** that "there is no cheap fix": for the majority of action maps BG2's margins are not fetched from tilemap at all, they are *synthesized by memcpy* in `PpuMergePaddedBackground` (`snesrecomp-go/runtime-next/src/snes/ppu.c`) from the always-present authentic 256 columns — and those two loops are gratuitously bounded by the **live** margin instead of the **budget**.

**Verifier corrections to the lead's model — read these before writing code:**

| Lead's assumption | Verified truth |
|---|---|
| `texture_extra` might be `kPpuExtraLeftRight`(96) | **Confirmed as the lead wrote it**: `texture_extra` derives from the bound *pitch* (`ppu.c:1371-1373`), pitch = `(256+2*g_ws_extra)*4` (`actraiser_rtl.c:1212-1213`), so `texture_extra == g_ws_extra == 95` in diorama mode. Texture column of screen x is `x + 95`. The UV **denominator is still `kPpuBufWidth` = 448** (textures are 448 wide, `src/main.c:929-931`; only `[0,446)` is uploaded). No off-by-one. |
| `bounded_world_margins` active by default? | **Yes, in the configuration where the symptom exists.** At implementation time `ws_bgrefresh` supplied the finite-world gate. BH7 made the bounded provider default-on, and BH8 removed that transaction/setting from runtime policy: a bound provider on the role-selected playfield now supplies the finite canvas directly, while an unbound world layer clamps to the authentic viewport. The symptom still needs widescreen plus Diorama and a visible skybox/backdrop contributor. |
| "The dead columns might be stale/garbage" | **No** — deterministically `0x00000000` (transparent), from `PpuClearOverlayRenderLine`'s full-pitch memset (`ppu.c:1341`), `PpuBeginBackgroundOverlay`'s memset (`ppu.c:1439-1440`), `PpuObjColor` returning 0 for index 0 (`ppu.c:88-89`), and the creation-time full-extent zero-fill (`src/main.c:926-939`). Nothing to sanitize; Option C is safe. |
| "Option C's stretch is imperceptible" | **No.** Span 446 → 351 at `camera_x==0` = **1.27x**, and it *animates* over 95 px of camera travel (~24-48 frames), with `u1` pinned and `u0` sliding — a one-sided pan+zoom whose apparent scroll rate fights BG2's parallax. This is why Option C must be the *fallback*, not the primary fix. |
| "Option C fixes the symptom" | **Only in `kDioramaSky_Only`.** In `Both` the backdrop plane draws *after* the skybox and covers it; in `Off` no skybox is drawn at all. |

## 2. Contributors, ranked by what the user actually sees

| # | Contributor | Where | Verdict |
|---|---|---|---|
| **1** | **BG2's captured margins are empty past the live bound** — the upstream cause. `PpuMergePaddedBackground` (`ppu.c:997`, `ppu.c:1004-1005`) stops at `extraLeftCur/extraRightCur`; for non-padded BG2 (`bg2_width >= 512`, e.g. **Fillmore act 1**, or `clamp`) there is no margin content at all. | `ppu.c:997/1004`, `actraiser_rtl.c:757-786` | **Root.** Fixing it removes the artifact at the source for the *narrow-BG2 majority* (Bloodpool, Aitos 01-03, Northwall 01-05/08, Death Heim 02-08). |
| **2** | **Skybox quad maps U over the dead columns**, `BLENDMODE_NONE` → opaque black band ≈21% of screen width. | `src/diorama/diorama.c:950,970-971,987` | **Dominant in `Skybox only`** (there the backdrop and in-box BG2 planes are skipped, `diorama.c:1263-1266,1276-1278`, so the skybox IS the whole background). |
| **3** | **Backdrop plane's zeroed gap strips**, drawn *after* the skybox, `BLENDMODE_NONE` → opaque black over the corrected sky. Begins at `y = hud_split_height` (40) because HUD rows are re-composited edge-to-edge (`ppu.c:1562-1565`) — this is why it reads as a "wedge"/notch, not a full-height bar. | `src/actraiser/actraiser_rtl.c:1039-1052` → `src/present.c:575,2936` → `src/diorama/diorama.c:696,1351-1352` | **Dominant in `Plane + skybox`; sole cause in `Off`.** A skybox-only fix leaves the symptom visibly present in 2 of 3 modes. |
| 4 | In-box BG2 / Bg2Hi tilted planes have the same content gap | `src/diorama/diorama.c:1182-1183` | **Cosmetic, not black** — `SDL_BLENDMODE_BLEND` (`diorama.c:1351-1352`) so alpha-0 columns are *transparent*; they merely fail to cover. Fixed for free by contributor 1. Do **not** narrow their UV (would desync from world-registered BG1). |
| 5 | DOF/edge-AA feather uses the fixed-span window, so the real content edge gets no feather; unclamped DOF blur bleeds ≤0.9 texel | `src/diorama/diorama.c:1413-1418` | **Cosmetic, macOS+Metal + `gpu_shaders_enabled=1` only** (default 0, `settings.c:1365-1371`). Out of scope; note only. |
| — | Shoebox, HUD/`PresentHudOverlayComposited`, former margin refresher, tile streamer, `PpuWriteOverlayRenderLine` writeback | — | **Verified non-contributors to this bug.** BH8 later retired the refresher for independent HLE cleanup; the other paths remain untouched. |

**Direct answer to "backdrop plane vs skybox — which does the user see?"** Both, mode-dependently, and they must both be fixed. `Skybox only` → skybox quad. `Plane + skybox` → the backdrop's black bars painted over the skybox. `Off` → backdrop only.

## 3. The fix, per contributor

### Fix A — widen the *synthesized* margins to the budget, but only for captured layers (contributors 1, 2-partial, 4)

Uses the existing `wsLayerMirror`/`wsLayerRepeat` machinery, no new PPU concept, not on the `PpuLayerExtra` hot path.

**File:** `snesrecomp-go/runtime-next/src/snes/ppu.c`, functions `PpuMergePaddedBackground` and its caller.

```c
static void PpuMergePaddedBackground(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                     const PpuPixelPrioBufs *layerbuf,
                                     bool repeat,
                                     int margin_left, int margin_right) {
  ...                                        /* centre loop unchanged */
  for (int x = -margin_left; x < 0; x++) {          /* was -ppu->extraLeftCur  */
  ...
  for (int x = kPpuXPixels; x < kPpuXPixels + margin_right; x++) {  /* was +extraRightCur */
```

At the call site (`ppu.c:1039`):

```c
  /* A captured layer's consumer (the diorama) samples the FULL fixed capture
   * span, not the live window, so synthesize padding out to the whole budget
   * there. The game framebuffer path keeps the live margin exactly as before —
   * flat-mode output must stay byte-identical (HUD-split rows composite the
   * full budget, ppu.c:1562-1565, and would otherwise newly show mirrored BG2). */
  bool captured = (dstbuf == &ppu->overlayBuffers[layer]);
  int ml = captured ? (int)ppu->extraLeftRight : (int)ppu->extraLeftCur;
  int mr = captured ? (int)ppu->extraLeftRight : (int)ppu->extraRightCur;
  PpuMergePaddedBackground(ppu, dstbuf, &layerbuf,
                           repeat_band || (ppu->wsLayerRepeat & (1u << layer)) != 0,
                           ml, mr);
```

Verified safe: `dstbuf == &overlayBuffers[layer]` is exactly the diorama-capture condition (`PpuBeginBackgroundOverlay`, `ppu.c:1433-1441`). Index safety with budget 95: `di ∈ [1,95] ∪ [352,446]` inside the 448-entry buffer; mirror `si ∈ [1,95]/[160,254]`, repeat `si ∈ [161,255]/[0,94]` — all inside the rendered centre. `PpuWriteOverlayRenderLine` already iterates `x ∈ [-95,351)` (`ppu.c:1377-1378`), so it copies the whole widened span with no further change. Leakage into `bgBuffers` is prevented by `RemoveFromGame` over `[-95,351)` (`ppu.c:1457-1461`); our loops never touch `x = -96`, the one column that branch does merge. Mirrored *backdrop-marker* pixels (`0x0500` from `ClearBackdrop`, `ppu.c:1034`) mask to color 0 in `PpuObjColor` → stay transparent, no new artifact.

**Scope limit (must be documented in the commit):** only runs for mirror/repeat layers. Wide BG2 (`bg2_width >= 512`, e.g. Fillmore act 1) and clamped BG2 (`AR_WS_BG2_MIRROR=0`, Death Heim hub `$07:$01`) get nothing — hence Fix B.

### Fix B — narrow the skybox's source UV to BG2's *effective* valid span (contributor 2, remainder)

Three pieces: a producer-side classification, two new FrameSlot fields, and a pure UV helper. D6 is respected: `present.c` reads only the slot; `diorama.c` receives plain ints.

**B1. New pure module `src/diorama/diorama_skybox_uv.c` / `.h`** (precedent: `src/diorama/diorama_scroll_math.c`):

```c
typedef enum {
  kBg2Margin_Live = 0,   /* raw tilemap fetch: valid out to the live margin */
  kBg2Margin_Padded,     /* mirror/repeat synth (post-Fix-A): valid to the budget */
  kBg2Margin_Clamped,    /* clamped: no margin content at all */
} DioramaBg2MarginSource;

/* Pure. Classifies BG2's captured margin extent from the PPU's per-layer
 * policy masks. A clamp bit combined with a repeat BAND varies per scanline
 * (Death Heim hub $07:$01, actraiser_rtl.c:829-838), which one pair cannot
 * express, so it is reported CONSERVATIVELY as Clamped: cropping the sky is
 * never worse than the black band that is there today. */
int DioramaBg2MarginSource_Classify(uint8_t ws_clamp, uint8_t ws_mirror,
                                    uint8_t ws_repeat, bool bg2_repeat_band);

/* Pure. Texture-column half-open span of BG2's valid captured content. */
void DioramaBg2ValidSpan(int ws_extra, int budget, int live_left, int live_right,
                         int margin_source, int *out_x0, int *out_x1);

/* Pure. Replaces DrawDioramaSkybox's UV math wholesale. tex_width == kPpuBufWidth. */
void DioramaSkyboxUvRange(int tex_width, int valid_x0, int valid_x1,
                          float blur_radius, float *out_u0, float *out_u1);
```

`DioramaBg2ValidSpan`: clamp `live_*` into `[0, budget]`; `m = (margin_source == Padded) ? budget : (margin_source == Clamped ? 0 : live)` per side; `*out_x0 = ws_extra - m_left`, `*out_x1 = ws_extra + 256 + m_right`; clamp into `[0, tex_width]`.
`DioramaSkyboxUvRange`: `mu = (blur_radius + 1.0f)/tex_width; *u0 = x0/tw + mu; *u1 = x1/tw - mu; if (*u1 < *u0) *u1 = *u0;` — **keep** the blur inset (the taps reach exactly `blur_radius`, so `+1` leaves one texel of slack) and keep the degenerate guard as defensive code (it is provably unreachable: the span is always ≥256 texels).

**B2. `src/diorama/diorama.c`** — `DrawDioramaSkybox` (:945) gains `int bg2_valid_x0, int bg2_valid_x1` and replaces lines 950-972 with one `DioramaSkyboxUvRange(kPpuBufWidth, bg2_valid_x0, bg2_valid_x1, blur_radius, &u0, &u1)` call. `Diorama_Composite` (`src/diorama/diorama.h:69`, `src/diorama/diorama.c:1117`) gains the same two ints and forwards them at :1160-1162. Do **not** apply them to the per-layer loop.

**B3. FrameSlot** (`src/present.h`, beside `extra_left_right` at :248):

```c
  uint8_t extra_left_cur;    /* live per-side margin for the frame just rendered */
  uint8_t extra_right_cur;
  uint8_t bg2_margin_source; /* DioramaBg2MarginSource; only meaningful when diorama_active */
```

Snapshot in `FrameSlot_Capture`, `src/frame_slot.c`, immediately after :370. **Do not read `g_ppu->extraLeftCur` there** — `src/main.c:329-334` runs `ActRaiser_RebindPpuOutputSurfaces()` between `RtlDrawPpuFrame` (main.c:306) and `HostDisplay_SubmitFrame` (main.c:402), and that reaches `PpuSetExtraSpaceCentered` (`src/hd_replacement_host.c:212`) which zeroes both live values. Instead latch them at the end of `ActRaiserDrawPpuFrame` (after the scanline loop, `src/actraiser/actraiser_rtl.c:1406-1408`) into file-scope ints exposed as `void ActRaiser_LiveMargins(int *l, int *r, int *bg2_source);` and read that. `bg2_margin_source` is classified from `g_ppu->wsLayerClamp/wsLayerMirror/wsLayerRepeat` and `wsRepeatY1[1] > wsRepeatY0[1]` at the same latch point.

**B4. `src/present.c`** — in the diorama branch (~:3075), compute `DioramaBg2ValidSpan(slot->ws_extra, slot->extra_left_right, slot->extra_left_cur, slot->extra_right_cur, slot->bg2_margin_source, &x0, &x1)` and pass `x0,x1` to `Diorama_Composite`. Use **`slot->ws_extra`** (`frame_slot.c:277`) as the offset, not `extra_left_right` — `ws_extra` is what the capture pitch and `Diorama_Upload` rect are derived from (`actraiser_rtl.c:1212`, `diorama.c:862`). They are equal today; keep them conceptually distinct.

**Interaction with Fix A:** with A landed, padded maps classify as `Padded` → span `[0,446)` → the UV values are *bit-identical to today's*, so B is a pure no-op there. B only engages for wide/clamped BG2, where the black band is replaced by a 1.27x stretch.

### Fix C — stop the backdrop plane's gap strips from being black (contributor 3)

**File:** `src/actraiser/actraiser_rtl.c:1034-1054`, inside `ActRaiser_ApplyWidescreenPolicy`. Replace the two `memset(...,0,...)` loops with a fill of the frame's backdrop colour when diorama mode is on — the colour the authentic renderer already shows for every unrendered pixel *inside* the live span (`ClearBackdrop` → `cgram[0]`).

Extract the loop as a pure helper (new `src/actraiser/actraiser_ws_gap.c`/`.h`) so it is testable:

```c
/* Pure. Fills the framebuffer gap strips the compositor never writes.
 * gap_l = budget - live_left at column 0; gap_r = budget - live_right at
 * column budget + 256 + live_right. fill_argb 0 reproduces the previous
 * memset-to-black behaviour exactly. */
void ActRaiserFillMarginGaps(uint8_t *rows, size_t pitch, int height,
                             int budget, int live_left, int live_right,
                             uint32_t fill_argb);
```

Caller:

```c
  uint32_t gap_fill = g_settings.diorama_mode ? ActRaiserBackdropArgb(g_ppu) : 0u;
  ActRaiserFillMarginGaps(g_ppu->renderBuffer, g_ppu->renderPitch,
                          kActRaiserAuthenticHeight,
                          g_ppu->extraLeftRight, l, r, gap_fill);
```

`ActRaiserBackdropArgb` is the same expansion as `BackdropArgb` in `src/sim/sim3d.c:72-79` (`ExpandColor5` × `PPU_brightness`); make that one non-static/shared rather than duplicating it. **Gate on `diorama_mode` is mandatory**: in flat widescreen the black gap is intentional pillarbox at a world edge, and changing it unconditionally alters flat output and breaks byte-identical replay. Known imperfections, both acceptable and to be commented: the fill uses start-of-frame `cgram[0]` (per-line HDMA palette changes are not tracked), and forced-blank rows are re-blackened by scanout's memset (`ppu.c:1523-1527`) — correct, since the screen is blank.

Explicitly **rejected** for contributor 3: narrowing the backdrop plane's UV (its mesh shares `aspect_x`/`height_scale` with BG1/OBJ, `diorama.c:1196-1197`, so it would de-register from the other planes) and switching it to `SDL_BLENDMODE_BLEND` (the compositor writes alpha 0 for *every* pixel, `ppu.c:1578-1581` — the whole plane would vanish).

## 4. Unit tests

All three modules are new pure files; wire them following the `actraiser_diorama_scroll_math_test` pattern at `CMakeLists.txt:508-520`.

### T1 `tests/diorama_skybox_uv_test.c` → `src/diorama/diorama_skybox_uv.c`

`DioramaBg2ValidSpan(ws_extra, budget, live_l, live_r, source, &x0, &x1)`:

| ws_extra | budget | live | source | expect `[x0,x1)` | note |
|---|---|---|---|---|---|
| 95 | 95 | 0,95 | Live | `[95,446)` | level start, wide BG2 → 351 cols, 1.27x |
| 95 | 95 | 95,0 | Live | `[0,351)` | level end, mirrored |
| 95 | 95 | 0,95 | Padded | `[0,446)` | post-Fix-A majority → **must equal today's mapping** |
| 95 | 95 | 95,95 | Live | `[0,446)` | mid-level no-op |
| 95 | 95 | 0,95 | Clamped | `[95,351)` | centre 256 only |
| 95 | 95 | 0,0 | Live | `[95,351)` | 4:3-in-diorama (`PpuSetExtraSpaceCentered`) — intended behaviour change |
| **0** | **0** | 0,0 | Live | `[0,256)` | **degenerate: `g_ws_extra == 0`** → identical to today |
| 95 | 95 | 200,-5 | Live | `[0,446)` | defensive clamp into `[0,budget]`, no OOB |

`DioramaSkyboxUvRange(448, x0, x1, r, &u0, &u1)`: for `[95,446)`, `r=1.0` → `u0 = (95+2)/448`, `u1 = (446-2)/448`; `r=3.0` → `(95+4)/448`, `(446-4)/448`. For `[0,446)`, `r=1.0` → `2/448`, `444/448` — assert these **exactly equal** the current `margin_u`/`uv_u1-margin_u` formula with `snes_width=446`. Degenerate `[95,351)` with an absurd `r=200.0` → `u1 == u0`, no inversion.

`DioramaBg2MarginSource_Classify(clamp, mirror, repeat, band)`: `(0,0x02,0,false)→Padded`; `(0,0,0x02,false)→Padded`; `(0x02,0,0,false)→Clamped`; `(0x02,0,0,true)→Clamped`; `(0,0,0,false)→Live`; `(0x01,0,0,false)→Live` (BG1's bit must not leak into BG2's classification).

**How it fails on the current/broken build:** rebuild the helper body with today's math (`*u0 = mu; *u1 = snes_width/448 - mu;`, i.e. ignore `x0/x1`) — rows 1, 2, 5, 6 fail; rows 3, 4, 7 still pass (proving the no-op guarantee is real and not vacuous). Break `Classify` to ignore the clamp bit → the `Clamped` rows fail. Break `ValidSpan` to use `budget` for `Live` → rows 1, 2, 6 fail.

### T2 extend `tests/ppu_render_pipeline_test.c` (already links the real `ppu.c`, `CMakeLists.txt:424-446`)

New `TestCapturedBg2PaddingUsesBudget()`: `ppu_reset`; `inidisp=0x0f`; enable BG1+BG2; one solid asymmetric 4bpp BG2 tile pattern; `PpuSetExtraSpace(ppu, 95)`; `PpuSetWidescreenLayerMirror(ppu, 1u<<1)`; `PpuSetExtraSideSpace(ppu, 0, 95, 0)`; `PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, buf, 446*4)`; `PpuSetOverlayCapture(ppu, Bg2, -95, 0, 446, 224, kPpuOverlayFlag_RemoveFromGame)`; `PpuBeginDrawing`; `ppu_runLine(ppu, 1)`.

- Assert captured texture columns `0..94` of row 0 are **non-zero** and equal the mirror of the authentic columns (`col[95-1-k] == col[95+k]` for the mirror source `sx = -x`). *Fails today: those columns are `0x00000000`.*
- Assert `repeat` variant (`PpuSetWidescreenLayerRepeat`) gives the cyclic mapping instead. *Fails today.*
- Assert columns `351..445` (right side, `extraRightCur == 95`) are unchanged between old and new code — no regression on the already-live side.
- **Gate proof (the regression the fix could introduce):** repeat the identical setup with **no** bound overlay surface, and assert the *framebuffer* columns `0..94` of row 0 remain the backdrop/zero. *This must pass both before and after* — it is the byte-identical-flat-mode guard; break the fix by removing the `captured` condition and it fails.
- Clamp variant (`PpuSetWidescreenLayerClamp(ppu, 1u<<1)`): capture columns `0..94` stay zero — documents Fix A's scope limit and justifies Fix B.

### T3 `tests/actraiser_ws_gap_test.c` → `src/actraiser/actraiser_ws_gap.c`

Seed a 446×8 buffer with a sentinel `0xDEADBEEF`, then `ActRaiserFillMarginGaps(buf, 446*4, 8, 95, live_l, live_r, 0xff1030a0)`:

| live | expect |
|---|---|
| `0,95` | cols `0..94` == `0xff1030a0` on all 8 rows; `95..445` still sentinel |
| `95,0` | cols `351..445` == fill; `0..350` sentinel |
| `95,95` | nothing written (whole buffer sentinel) |
| `0,0` | cols `0..94` **and** `351..445` filled; centre sentinel |
| budget `0` | nothing written |
| `live_l = 200` (> budget) | nothing written, no OOB store (run under ASan) |
| `fill_argb = 0` | byte-identical to the previous `memset` behaviour |

**How it fails on the current/broken build:** the current code memsets `0`, so the `0xff1030a0` expectation fails on the first four rows; the `fill_argb = 0` row passes, proving the flat-mode path is unchanged. Break the right-hand offset to `budget + 256` (dropping `+ live_right`) → the `0,0` and `95,0` rows fail.

## 5. What cannot be validated here (no ROM) — exact on-device checks

Settings for every check: **Screen ratio = 16:9**, **Diorama mode = On**. Walk to a level's extreme left edge (`camera_x == 0`, i.e. the spawn point before moving) and to the far right edge.

1. **Fix A, padded BG2** — Bloodpool act 2 or Northwall act 1 (narrow BG2, mirror/repeat). `Diorama skybox = Skybox only`. **Correct:** at spawn the sky fills the full viewport with *no* black band at the left edge and **no horizontal scale change** as you walk right (Fix A gives the full 446 columns, so Fix B is inert). Also check `Plane + skybox`: the tilted in-box BG2 plane's left half should now be filled instead of showing a transparent gap.
2. **Fix B, wide BG2** — Fillmore act 1 (`bg2_width = 2304`, no padding policy). `Skybox only`. **Correct:** no black band; instead the sky is ~27% wider at spawn and smoothly relaxes to normal over the first ~95 px of camera travel. **Judgement call:** watch for objectionable "breathing" on recognizable cloud/mountain shapes. If unacceptable, the pre-authorized fallback is a *fixed* conservative span (always crop `g_ws_extra` per side, i.e. classify everything unpadded as `Clamped`) — a permanent 1.74x stretch that never moves.
3. **Fix B, clamped BG2** — Death Heim hub `$07:$01` and `AR_WS_BG2_MIRROR=0` on any narrow-BG2 stage. **Correct:** no black side bands; sky cropped to the centre 256 and static. Confirm the conservative classification did not visibly harm the fog rows below y=144.
4. **Fix C, backdrop plane** — any action stage at a level bound with `Diorama skybox = Off` and again with `Plane + skybox`. **Correct:** the black notch starting below the HUD (y ≈ 40) is replaced by the stage's backdrop colour, continuous with the rest of the scene. Watch specifically for stages whose `cgram[0]` is *not* sky-coloured (cave/interior sections) — if the fill reads wrong there, prefer skipping the backdrop plane in `Plane + skybox` over reverting to black.
5. **Flat-mode byte-identity** — the whole point of the `captured` gate and the `diorama_mode` gate. Run the existing replay/canary comparison (`tools/canary.sh`) in flat 16:9 with diorama off, across a level start *and* a level end, including HUD-split rows. **Correct: zero pixel differences.** This is the highest-risk item and cannot be checked here at all.
6. **Perf** — Fix A adds ≤190 compare-stores per BG2 scanline in diorama mode (~42k/frame); Fix C converts two memsets into ~42k word stores. Confirm with `AR_PERF` that the per-frame draw time is unchanged within noise.
7. **GPU-shader path** — `gpu_shaders_enabled = 1` on macOS/Metal with `Plane + skybox`: confirm the blur inset still keeps the (now interior) boundary out of tap range, i.e. no dark seam appears at the new UV edge. Contributor 5 (DOF/edge feather anchored to the fixed span) is deliberately left unfixed; log it as a follow-up.

## 6. Risks and rejected alternatives

**Risks accepted**
- **Fix A leaks into flat mode if the `captured` gate is wrong.** On HUD-split rows (`y < 40`) `PpuLayerExtra` grants the color window the full budget and `composite_left = extraLeftRight` (`ppu.c:490-492,1562-1565`), so unconditional widening *would* make mirrored BG2 appear in the HUD border gaps at a level bound — a visible change and a replay break. Mitigated by the gate + T2's gate-proof assertion.
- **Fix B's one-sided pan.** During the ramp `u1` stays pinned while `u0` slides, so the sky's apparent scroll rate is wrong for ~95 px, not just its scale. Only reachable on wide/clamped-BG2 maps after Fix A. Fallback: fixed conservative span (see on-device check 2), or scale the quad's *screen positions* instead of its UVs.
- **Death Heim hub is deliberately over-cropped** by the conservative `clamp + repeat band → Clamped` classification. Strictly better than today's permanent black bands.
- **Fix C's colour source** is start-of-frame `cgram[0]`; per-line HDMA palette animation is not tracked.
- **Snapshot ordering.** `src/main.c:329-334` can zero `extraLeftCur/extraRightCur` before `FrameSlot_Capture`; the latch-in-`ActRaiserDrawPpuFrame` approach makes the new fields immune. If instead you read `g_ppu` directly at `frame_slot.c:370`, you *must* leave a comment at `main.c:332` — today it is harmless only because that branch is `!diorama_mode`-gated.
- **FrameSlot zero-default.** `FrameSlot_Capture` memsets the slot (`frame_slot.c:229`), so `0/0` means "live span = centre 256" (safe over-crop). Never invert this to "0 = unset, use the full budget" — that reintroduces the wedge at exactly `camera_x == 0`.

**Rejected**
- **Comment's option (a), a per-layer numeric ceiling in `PpuLayerExtra`** — *inert* for the dominant case: `ppu.c` returns 0 for any layer with a clamp/mirror/repeat bit **before** the numeric `extra` argument is consulted, and BG2 always has one of those bits when `bg2_width < 512`. At implementation time it was additionally harmful for wide BG2 because it widened fetches beyond the host refresher's live interval. BH8 removed that refresher, but the ordering/inertness objection and provider-owned finite bounds still reject this option.
- **Comment's option (b), a second BG2-only scanout pass** — needs `renderBuffer` save/rebind, unbinding sibling overlay sources (`PpuClearOverlayRenderLine` memsets *every* bound source's line, `ppu.c:1332-1341`, so a naive replay erases the first pass's BG1/OBJ captures), and re-initializing the 8 `SimpleHdma` channels. Full cost for no benefit over Fix A. (HDMA is not the obstacle the comment implies — `SimpleHdma_DoLine` never touches `DmaChannel`, so re-init is free.)
- **Changing either `SDL_BLENDMODE_NONE` to `BLEND`** — for the skybox it only swaps pure black for the `{20,20,30}` clear (the sky is still absent); for the backdrop it deletes the plane entirely, since the compositor writes alpha 0 for *all* pixels.
- **Narrowing the in-box BG2/Bg2Hi UV window** — their `BLENDMODE_BLEND` already handles the dead columns correctly (transparent), and narrowing would desync them from world-registered BG1.
- **A symmetric UV inset** (`min(left,right)` both sides) — the collapse is one-sided, so this also crops the still-valid side, pushing the stretch from 1.27x to 1.75x.
- **Deleting the gap memset at `actraiser_rtl.c:1039-1052`** — the compositor never writes those columns, so they would hold last frame's pixels (stale ghost strips), strictly worse than black.
- **Mirroring `diorama_skybox` into FrameSlot** — `diorama.c` reads its own settings live *by documented design* (`src/diorama/diorama.c:130-132`); D6 fences `present.c` as a translation unit only. Not a gap, not part of this fix.

**Files touched:** `snesrecomp-go/runtime-next/src/snes/ppu.c` (2 functions), `src/actraiser/actraiser_rtl.c` (gap fill + margin latch), new `src/diorama_skybox_uv.{c,h}`, new `src/actraiser_ws_gap.{c,h}`, `src/present.h` (3 fields), `src/frame_slot.c` (3 assignments), `src/present.c` (span computation + 2 args), `src/diorama.{c,h}` (2 params, UV math replaced), `src/sim/sim3d.c` (un-static `BackdropArgb`), `CMakeLists.txt` (2 new test targets + 2 new sources in the app target), `tests/diorama_skybox_uv_test.c`, `tests/actraiser_ws_gap_test.c`, `tests/ppu_render_pipeline_test.c`. Delete the now-obsolete "no cheap fix" paragraph at `src/diorama/diorama.c:904-922` and replace it with the corrected scope note (Fix A covers padded BG2; Fix B covers wide/clamped BG2 with a stretch; contributor 5 remains open).
