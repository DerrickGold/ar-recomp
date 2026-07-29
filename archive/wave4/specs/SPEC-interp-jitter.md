# Spec — IJ1: diorama scroll interpolation is jittery when enabled

**Status:** authored, not implemented. Not yet audited. **The primary hypothesis
must be confirmed against data before any code changes** — see §7.

## 1. Symptom (author-reported, on-device)

With diorama mode and `Scroll interpolation` enabled, movement is **visibly
jittery**. Not "no effect" — that was the R17 symptom, now fixed — but actively
worse-looking than interpolation off.

Author's initial hypothesis: we lack "in-between frame rendering to blend two
frames together."

## 2. Ruling that hypothesis out first

**Frame blending is not the right mechanism and would not fix this.** Blending
two framebuffers is temporal blending (ghosting / motion blur); for scrolling
content it produces a smeared double image, not smooth motion. The correct
mechanism for sub-tick scroll is what the code already does: shift each layer's
UV window by a fractional offset, so the layer is *drawn at an in-between
position* rather than two positions being mixed.

So the mechanism is right. The **numbers** are wrong. This spec is about which
number.

## 3. Primary hypothesis — we EXTRAPOLATE, and every velocity change snaps

`src/diorama_scroll_math.c:83-86`:

```c
int dh1 = curr->bg1_camera_x - prev->bg1_camera_x;
d.bg_du[0] = (t * (float)dh1) / (float)curr->snes_width;
```

Displayed position is `curr + t × (previous tick's motion)`. The choice is
deliberate and documented at `src/present.c:658-668`: it extrapolates forward
using the prev→curr delta as a one-tick velocity estimate, explicitly rejecting
the design doc's §6.2 `prev + t*(curr-prev)` lerp because that costs one tick of
display latency.

**Why that produces jitter.** Extrapolation is only smooth while velocity is
constant. Every time the real velocity differs from the prediction, the error is
corrected *discontinuously* at the next tick:

- At `t→1` we have drawn a full extra tick of *predicted* motion.
- The next tick delivers the *actual* position.
- If the camera accelerated, decelerated, stopped at a wall, or changed
  direction, the actual position is not where we predicted → visible snap.
- The snap recurs every tick, i.e. a **60Hz sawtooth on top of the motion**,
  which on a high-refresh panel reads exactly as jitter.

ActRaiser's action-stage camera is *not* constant-velocity: it follows the player
with its own easing, accelerates and decelerates, and clamps at level edges. So
the "velocity is constant" precondition is the exception, not the rule — which
would make the artifact frequent rather than rare.

Worth stating plainly: extrapolation *is* a standard technique, and the comment is
not wrong that lerping costs a tick of latency. But it traded a **constant,
sub-perceptual 16.6ms latency** for a **visible artifact on every velocity
change**. Given the reported symptom, that looks like the wrong trade.

### 3a. Proposed fix

Interpolate between the two known-good positions instead of predicting past the
newest one:

```
position = prev + t × (curr − prev)
```

Never predicts, so never needs correcting; motion is monotone by construction.

Cost: one tick (~16.6ms) of additional display latency for the *interpolated
layers only*. Note this does NOT delay input response — the game logic, the
player sprite (OAM, not an interpolated layer), and the HUD are unaffected. It
delays only the background scroll, against which 16.6ms is not perceptible.

Implementation is small: the delta already exists; `t` is already correct; the
change is which base the offset is applied to, plus updating the comment at
`present.c:658-668` (which currently documents the rejected-then-chosen
reasoning and would become actively misleading).

**This changes the sign/base of a rendered offset, so it must be gated on the
existing off-by-default setting and verified by the mesh-UV test, not by
`AR_INTERP_LOG` alone** — see §6.

## 4. Secondary hypotheses (must be ruled out, not assumed away)

These are independent of §3 and could each produce jitter alone or additively.

**IJ-H2 — `capture_ticks` quantization.** `diorama_scroll_math.c:70`:
`t = alpha / curr->capture_ticks`. When the drain runs 2 ticks in one iteration,
`t` halves while the delta doubles — correct *on average*, but the on-screen
motion *rate* changes step-wise for that frame. If `capture_ticks` alternates
1,2,1,2 under load (which the Limit-aware catch-up cap permits), that alternation
alone is jitter, and it would persist after fixing §3.

**IJ-H3 — per-layer divergence.** BG1 and BG2 use independent deltas
(`:83-91`) — intentional parallax. If BG2's WRAM camera is noisier or updates on
a different cadence than BG1's, the layers jitter *against each other*: the scene
appears to shear rather than shake. Note finding B1b already established that
BG2 is HDMA-driven and its PPU registers carry residue; the fix moved to the WRAM
camera, but whether BG2's WRAM camera is as stable as BG1's is unverified.

**IJ-H4 — the `t` clamp hides a real out-of-range condition.**
`:71-72` clamps `t` to [0,1] silently. If `alpha` or `capture_ticks` is ever out
of the expected range, the clamp converts that into a *frozen* offset for that
frame (a stutter) instead of a loud failure. The R17 assertions cover `alpha`,
but only in debug builds.

**IJ-H5 — downstream in the UV/mesh path.** The offset could be computed
perfectly and then quantized or cancelled downstream. R17/C1 fixed exactly such a
bug (the UV window clamp cancelled 100% of the shift). The 4px
`kInterpUvSlackPx` saturation is a candidate: a shift larger than the slack
**saturates**, so fast scrolling would clip to a fixed maximum offset and then
release — which is itself a jitter source. **Check whether normal walking speed
exceeds 4px/tick.** If it does, the slack is too small and saturation is the
bug, not extrapolation.

## 5. Diagnosis before implementation (do this first)

`AR_INTERP_LOG=1` while walking steadily in a diorama action stage. The log line
is `t=%.3f alpha=%.3f ticks=%u bg1_du=%.5f bg1_dv=%.5f`.

| Observation | Indicates |
|---|---|
| `t` ramps 0→1 smoothly, but `bg1_du` jumps between ticks | §3 extrapolation / velocity-change snap |
| `ticks=` flickers 1↔2 | IJ-H2 quantization |
| `bg1_du` plateaus at a fixed value during fast motion | IJ-H5 slack saturation |
| `bg1_du` smooth and monotone, yet it still looks jittery | IJ-H5 downstream, or the camera itself |
| `alpha` never exceeds ~0.1 | the R17 cadence regressed again |

This costs one recording and settles which hypothesis to act on. **Fixing §3 on
a guess risks changing a rendered offset for the wrong reason** — and this
subsystem has now produced five regressions, every one of which looked correct in
an intermediate signal while the pixels disagreed.

## 6. Acceptance

ROM-free: `diorama_scroll_math_test` gains a case asserting the interpolated
position is monotone across a **velocity change** (delta 10px then 2px), which
the current extrapolating formula fails — this is the regression test for §3.
Existing monotone-ramp and multi-tick-normalization cases must still pass. Canary
green, full link.

Must be verified NON-TAUTOLOGICAL against the pre-fix formula, per the
now-standard practice for this subsystem.

On-device: walking steadily at >60Hz shows smooth parallax with no per-tick snap;
a direction reversal and a wall-stop produce no visible jolt; interpolation OFF
remains byte-identical (PPM compare).

## 7. Open questions for the audit

1. Confirm the §3 mechanism from source: does the extrapolated offset in fact
   get corrected discontinuously at the tick boundary, or is there some existing
   damping that absorbs it?
2. IJ-H5 quantitatively: what is the BG1 camera delta in pixels/tick at normal
   walking speed, and does `t × delta` exceed `kInterpUvSlackPx = 4.0f` (i.e.
   4/448 in UV)? If yes, saturation is a live bug independent of §3.
3. IJ-H2: can `capture_ticks` realistically alternate 1↔2 during steady play at
   default settings?
4. IJ-H3: is BG2's WRAM camera as stable per-tick as BG1's?
5. Does the one-tick latency of §3a interact with anything that *does* need to be
   latency-free — confirm the player sprite and HUD are not interpolated layers.
6. Is there any place other than `bg_du/bg_dv` that the diorama camera pose is
   perturbed per-present (the B4 dyncam lean/kick decay at `present.c:2742`
   runs per present on a wall-clock exponential) — could *that* be the jitter
   source rather than scroll interpolation, given both are only visible together?
