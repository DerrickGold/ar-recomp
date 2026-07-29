# Wave 4 — combined spec: 3D/render fixes and improvements

**Base:** `main` @ `addd8f8`. **Authored on a ROM-free macOS/arm64 machine.**

Everything here is either already committed and gated (§1) or specified for
implementation (§2–§5). The organising constraint: **macOS/Metal is the backend
where most of these bugs are invisible**, so the design rule throughout is *make
it provable by a unit test on this machine, or state plainly that it cannot be*.

---

## 0. The pattern behind most of this wave

Every confirmed defect below is an instance of one of three failure modes, and
naming them is more useful than the individual fixes:

1. **An API that happens to work on Metal is assumed contractually guaranteed.**
   (Write-only lock read-back; device-reset handling; blend-mode support.)
   Metal hands back persistently-mapped buffers, never emits
   `RENDER_DEVICE_RESET`, and supports every blend mode we use — so three whole
   classes of bug cannot manifest here.
2. **A cache is gated on a serial that does not depend on what actually
   invalidated it.** (Sim-3D textures keyed on game-side serials survive a GPU
   device reset forever.)
3. **A correct-looking intermediate value is trusted instead of the pixels.**
   (Interpolation: five regressions, each of which looked right in
   `AR_INTERP_LOG` while the output disagreed.)

**Testing rule for this wave:** a unit test must assert the property that would
break, not the value that happens to be correct today. Prefer *ratios* and
*invariants* over absolute numbers — e.g. the IJ1 test asserts `dv/du ==
448/224` (unit-independent) rather than `du == 0.0089`. And every new test must
be run against a deliberately-broken build to prove it fails there.

---

## 1. Already committed (5 commits, gated: canary 19/19 + full link)

| # | Commit | What |
|---|---|---|
| WM1 | `acd7b45` | Blur mip was built by reading back a **write-only** `SDL_LockTexture` mapping. Fine on Metal, garbage on Vulkan/Mesa — the reported Deck world-map garbling. Now reads the persistent CPU image via `SimWorldMap_Downsample`. |
| — | `d5a9791` | `canary.sh` never built `actraiser_camera_orbit_test`, so the canary failed on **any clean checkout**. Found by running the apply script against a fresh clone. |
| IJ1 | `bfafe3e` | Scroll interpolation divided the U delta by `snes_width` (256) while consumers normalize U by `kPpuBufWidth` (448) — **1.75× too much shift**, a 60Hz backward sawtooth even at constant velocity. Four existing test assertions had encoded the bug. |
| DR1 | `d38da85` | Three sim-3D textures were never rebuilt after `RENDER_DEVICE_RESET`; `PresentSimUnderlay_Reset()` existed with **zero callers**. Serial-gated on game state, so it never self-heals. |
| SB1 | `dc07371` | Shoebox walls sat at 2.00×/2.07× the layer extent (`kShoeboxOverscan = 2.0`), leaving the reported gap. Now 1.05× on both axes, with `half_y` honoring C1's `height_scale`. |

**On-device verification still outstanding for all five** — see each commit's
`Verify-gate:` trailer. WM1, DR1 and SB1 cannot be reproduced here at all.

---

## 2. Confirmed audit findings, NOT yet fixed

From a 6-lens resource-discipline audit (12 candidates → **5 confirmed**, 7
refuted with reasons). The refuted ones are listed in §6 so they are not
re-filed.

### W4-1 — rim-light mask blend mode is clobbered by the callee (medium)

`present.c:1512` sets `SDL_SetTextureBlendMode(g_sim_obj_atlas_texture,
mask_blend)` for the mask pass, then calls `DrawSimObjectPriority` at `:1513` —
which **unconditionally resets that same texture to `SDL_BLENDMODE_BLEND` at
`:1281`**, before drawing anything. The mask pass therefore never uses its blend
mode, so the rim is never trimmed to the sprite body: the effect draws a filled
offset silhouette instead of a rim.

Verified directly: `:1281` is the first statement after the early-out, so the
caller's set at `:1512` cannot survive.

**Fix:** pass the desired blend mode down as a parameter of the pass struct
(`SimBillboardPass` already carries per-pass data), or have
`DrawSimObjectPriority` only set the default when the caller has not specified
one. Do **not** simply delete the `:1281` set — other callers rely on it.

**ROM-free test:** the blend-mode plumbing is testable by extracting the
"which blend mode does pass P use" decision into a pure function and asserting
Fill→`BLEND`, Mask→`mask_blend`. That catches the clobber as a logic error
without needing a renderer.

### W4-2 — custom blend support is never checked (medium)

`present.c:1069` (and the rim mask path) construct custom blend modes via
`SDL_ComposeCustomBlendMode` and compare against `SDL_BLENDMODE_INVALID`, but
**`SDL_ComposeCustomBlendMode` does not validate support** — it composes an
opaque value. Support is only discovered when `SDL_SetTextureBlendMode` /
`SDL_SetRenderDrawBlendMode` is called, and **those return values are discarded**.
So on a backend without custom-blend support (reachable: D3D9, some GLES
profiles, the software renderer) the fallback path is unreachable and the effect
silently draws with the previous mode instead of degrading.

**Fix:** check the return of the `SetBlendMode` call that first uses the composed
mode, and latch a "custom blend unsupported" flag that the effect gates on —
mirroring the existing `g_blur_available` pattern in `diorama.c`. Log once.

**ROM-free test:** the latch logic is pure; assert that a simulated failed
`SetBlendMode` disables the effect rather than proceeding.

### W4-3 — cloud shroud never regenerated after device reset (was high → low)

`present.c:1934` guards on the handle alone, so the noise field is generated
exactly once. **Already fixed in practice by DR1** (`PresentSimUnderlay_Reset`
destroys `s_sim_cloud_texture`), which is why this drops to low — but the guard
is still handle-only, so a future reset path that forgets to call the reset
helper reintroduces it. **Action:** none required; recorded so the coupling is
not silently broken later. Consider an assert.

### W4-4 — `DrawSimCullMarkers` leaks draw color/blend (low, diagnostic-only)

`present.c:2474` sets a green/red translucent draw color inside its loop with no
restore, and is the last call in `RenderSimProfile`. Requires
`AR_SIMCULLMARK=1` **and** a display mode with the HUD split disabled (which
makes `PresentHudOverlayComposited` early-return instead of overwriting the
state). In that configuration the next flat-path `SDL_RenderClear` tints the
whole window — including the persistent letterbox bars — green or red.

**Fix:** save/restore around the loop, matching `PresentSceneInspector`
(`:526-531`/`:563-564`), and add an explicit `SDL_SetRenderDrawColor` before the
flat path's clear at `:3033`. Low priority (developer-only) but it corrupts
exactly the diagnostic view it exists to make trustworthy.

### W4-5 — `HostDisplay_WindowPointToOutput` rounds out of range when minifying (low, latent)

`host_display.c:108` rounds half-up: `(window_x * output_width + window_width/2)
/ window_width`. Harmless magnifying (high-DPI), but **overflows by one when
`output <= window/2`**. Verified: window_x 2559 → output_x **1280** when valid
columns are 0..1279.

Benign today because every consumer bounds-checks, so the symptom is a one-pixel
dead edge. But `SDL_mouse.h:510-512` warns coordinates may be outside the
window, so no consumer may assume in-range, and this becomes an OOB the moment
one indexes an array by it. **Fix:** clamp to `output_width-1`/`output_height-1`.
One line. **ROM-free test:** trivially unit-testable if the conversion is
extracted; assert the last row/column maps in-range at 1×, 1.5×, 2× and 4×.

---

## 3. RR1 — decouple render resolution from display resolution

Full spec: `SPEC-render-resolution.md`. **The audit broke it in three places;
this section is the amended version and supersedes that file's §3a/§5/§8.**

**Goal (unchanged):** let the user render at e.g. 720p so the game *presents* a
smaller surface, allowing an external upscaler (gamescope FSR, driver/display
scaling) to engage. **Not** implementing an upscaler — SDL3's 2D renderer
exposes none.

**Amendment 1 — the mechanism must suppress `HIGH_PIXEL_DENSITY`, not just set a
mode.** `SDL_DisplayMode.pixel_density` (`SDL_video.h:145`) means a 1280×720 mode
with density 2.0 yields a **2560×1440 pixel drawable**. With
`SDL_WINDOW_HIGH_PIXEL_DENSITY` set (`main.c:746`), selecting 720p on a scaled
panel would report success, keep input correct, and **upscale nothing** — the
exact silent no-op the spec says it must refuse to ship. So: suppress
`HIGH_PIXEL_DENSITY` in all modes *or* filter
`SDL_GetFullscreenDisplayModes()` to `pixel_density == 1.0f`, **and read back
`SDL_GetRenderOutputSize` to publish the ACHIEVED size** (the
`Settings_SetHostVsyncActive` pattern).

**Amendment 2 — input mapping is a non-risk, but not for the reason given.**
The original §5 claimed "a larger window with a smaller drawable is the same
arithmetic as high-DPI reversed." Both halves are wrong: the arithmetic is not
symmetric (see W4-5), and **neither mechanism produces window≠drawable at all** —
`SDL_video.h:1014-1016` says a mode change emits `WINDOW_RESIZED` with the new
dimensions, so the window follows the mode and the ratio stays ~1.0. Suppressing
`HIGH_PIXEL_DENSITY` likewise makes drawable *equal* window. Drop R1 from the
risk list; keep W4-5 as an independent fix.

**Amendment 3 — an acceptance test in §8 is unexecutable.** It said "mouse
hit-testing in the settings overlay still lands correctly". `settings_overlay.c`
has **zero** mouse handling — the menu is keyboard/gamepad only. The two
output-space hit-tests serve the *scene-inspector debug panel*, gated on
`s_debug_panel_visible`. Restate as: "scene-inspector debug-panel drag tracks the
cursor at 720p (requires scene_inspector on and the panel visible)."

**Still unknown:** whether gamescope honors a client mode request at all
(finding R8 established it suppresses mode-change events and misreports refresh).
If it does not, the Deck lever is `gamescope -w 1280 -h 720 -W 1280 -H 800 -F
fsr` in launch options and RR1 is desktop-only — which must be **documented, not
papered over**. One audit lens (letterbox arithmetic at non-integer ratios,
risk R5) **stalled and is unexamined**.

**ROM-free tests:** settings round-trip; `Native` selects byte-identical window
flags and issues no mode call (assert the decision, not the pixels); the
mode-choice function is pure and testable — given a list of `SDL_DisplayMode`
and a requested resolution, assert it picks a `pixel_density == 1.0` mode or
reports failure.

---

## 4. WN1 — 3D world-navigation view (map `$09`)

Full spec: `SPEC-world-navigation-3d.md`. Steps 1-5e are implemented except the
2048² high-fidelity-town reconstruction research and the final complete
movement/action-entry replay. The whole developed world is one 1024² texture;
the host owns its pure build from ROM tables plus simulation state and never
observes shared scratch `$7E:C000`.

`AR_SIM3D_WORLD_NAV` independently selects a forced-top-down scene for
`$18/$19=00/09`. The game thread captures canonical focus `$0300/$0302`,
current/staged signed 8.8 matrices `$0304-$0312`, rotation/zoom
`$0314-$0318`, active location `$0341`, Palace/UI OAM, and master brightness.
The presentation preserves movement and the act-entry spin/zoom while adding
the shared backdrop, location haze, optional seamless world clouds and
lighting, zoom-relative cloud ceiling, and animated water. Partial-brightness
frames remain enhanced throughout fade-in/out; forced blank and unsupported
OAM layouts fail closed.

The former WN-R2 camera risk is resolved: `$09` has `$92=00` in both steady and
mid-transition fixtures, and `$02:8384` uploads the explicit WRAM matrix/focus
to M7A-D/M7X-Y. The immutable frame therefore uses the WRAM contract rather
than sampling end-of-frame PPU state.

**ROM-free tests:** the coordinate mapping (WRAM matrix/focus →
source-to-screen), singular-matrix rejection, zoom-relative cloud visibility,
location-haze mask, fade-alpha mapping, OAM classification, and view-selection
predicate are extracted and covered. ROM-backed fixture tests additionally
assert exact HLE/oracle parity for direct-transition, steady-navigation, and
mid-action-entry snapshots.

---

## 5. IJ2 — extrapolation vs interpolation (deferred, deliberately)

`SPEC-interp-jitter.md` proposed replacing forward extrapolation with
`prev + t*(curr-prev)`. **Not done, on purpose:** the audit showed the IJ1 unit
error was the dominant term, and a lerp with that error still present would also
snap `0.75*delta` per tick. So the units were fixed alone, and whether
extrapolation *still* misbehaves on velocity changes is now answerable from a
clean baseline.

**Correction to that spec:** it claimed the player sprite is not an interpolated
layer, so a lerp's one-tick latency would be harmless. False —
`DioramaLayerBgIndex` (`diorama.c:890-901`) maps all four OBJ planes to BG index
0, deliberately, so sprites stay attached to the gliding world. The lerp fix
survives (OBJ and BG1 share `bg_du[0]`, so alignment stays exact) but the
justification must be rewritten.

**Decide with data, not analysis:** `AR_INTERP_LOG=1` walking steadily. `bg1_du`
at 2px/tick should now read ~0.0045 (2/448), not ~0.0078 (2/256), and must **not**
plateau at 0.00893 (the 4/448 slack ceiling). If motion is smooth, IJ2 is
unnecessary. If it still snaps on direction changes only, IJ2 is the fix.

**Unexamined:** one audit lens (per-present dyncam damping / layer divergence)
died on an API error. Note that enabling interpolation also **raises the present
rate** (R17/C5 gates re-presents on that setting), so the B4 dyncam's
wall-clock exponential at `present.c:2742` starts moving when the setting is
flipped — a jitter source that would be naturally misattributed to interpolation.
**Re-run that lens before concluding anything about residual jitter.**

---

## 6. Refuted — do not re-file

Seven audit candidates were refuted on inspection. Recorded so they are not
rediscovered:

- **Town canvas dirty-rect uploads after reset** — the claimed "incremental
  contract" premise is wrong; DR1's reset covers it.
- **Diorama GPU shaders latched past a reset** — the GPU objects only exist under
  SDL's "gpu" renderer, where the claimed path cannot fire.
- **Blur-mip serial latched on lock failure** — the cited header declares that
  failure impossible here.
- **`RenderSimProfile` backdrop draw-color leak** — real leak, but overwritten
  before it can reach the flat path.
- **`PresentHudOverlayComposited` / `DrawSimShadowMask` / `DrawSimRimLight` /
  `BuildDioramaSupersample` state leaks** — mechanisms real, consequences do not
  survive contact with the source (later code re-sets the state
  unconditionally).

Also refuted earlier, from the 3D-subsystem audit: out-of-range UV sampling is
deliberate (`SDL_TEXTURE_ADDRESS_CLAMP` is set *because* of it);
`sim3d.c:289`'s one-frame-stale brightness copy is the design's fail-closed path;
the zero-width aspect NaN is unreachable.

---

## 7. Suggested order

1. **W4-1** (rim light is visibly wrong today) and **W4-5** (one line, latent
   OOB) — both small, both unit-testable.
2. **W4-2**, **W4-4** — honesty/diagnostic correctness.
3. **RR1** as amended — needs the gamescope question answered first, and should
   ship with the achieved-size readback or not at all.
4. **WN1** — largest, and the most interesting; gate on WN-R2.
5. **IJ2** — only if the on-device log says the jitter survived IJ1.

## 8. Gates for every commit in this wave

`bash tools/canary.sh` (21 tests as of the edge-margin fix) · full game link
(`cmake --build build/game --target ActRaiserRecomp`) · `cc -fsyntax-only -Wall
-Wextra` · **every new test probed against a deliberately-broken build to prove
it is not tautological** · `git format-patch` + a fresh-clone replay with a
matching tree hash before delivery.
