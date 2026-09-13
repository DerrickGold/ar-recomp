# Enhanced-view fallback review — 2026-09-12

Reviewed baseline: `947d79d6` (`Enable connected SIM globe underlay by default`).
The findings below record the original read-only review. The follow-up now
fixes all three findings and removes the connected SIM background's silent
flat-underlay latch. See [startup preparation and resolution](graphics-startup-preparation.md)
for the implementation, tests, and remaining runtime limits. The original
no-op probe is historical evidence; regression coverage lives in the tests.
Unrelated installer, localization UI, and benchmark-tool work remains untouched.

## Findings

### P1 — Sky Palace silently discards its selected view on presentation failure

At [present.c:2445](../src/present.c#L2445), a missing foreground upload,
`CoreFailure` from `PresentWorldNavigationBackdrop`, or a rejected foreground
draw all become `palace_drawn == false`. A successful draw of `g_texture` then
replaces the entire view with native graphics, without surfacing the original
failure. A transient error can cause a flash; a persistent error leaves the
native backdrop indefinitely.

The foreground upload has several silent early returns at
[present.c:848](../src/present.c#L848), including invalid capture surfaces,
composition rejection, texture allocation, and texture upload. The globe
renderer explicitly reports core failures, including depth-target setup and
scene submission failures; the top-level Palace caller erases that distinction.
SIM town and action Diorama presentation already surface core failures instead
of switching the whole view.

Recommended direction: treat an enabled Palace backdrop and its foreground as
required parts of the selected mode. Preflight stable capabilities at startup,
prepare scene resources before revealing the scene, and preserve a typed error
through presentation. Recover the same mode or report the failure explicitly;
do not cover it with native graphics. This is a code-confirmed routing issue;
no live GPU failure was induced during this review.

### P2 — Inert colour state can unnecessarily reject SIM and Palace capture

[sim3d.c:441](../src/sim/sim3d.c#L441) and
[sim_world_navigation_palace.c:11](../src/sim/sim_world_navigation_palace.c#L11)
require a zero fixed colour for their no-op colour-math profile. With `CGWSEL=0`
and `CGADSUB=0`, no layer uses that fixed colour at all. A nonzero leftover
register value therefore need not change any native pixel, but SIM reports
`unsupported_color_math` and Palace rejects its PPU profile. The normal callers
then present native instead of enhanced.

The [focused probe](evidence/enhanced-fallback-review-2026-09-12/noop_capture_probe.c)
uses the real PPU and production capture functions. It confirms an identical
256-pixel native scanline before/after changing only the unused fixed colour:

```text
SIM: zero-fixed=1 leftover-fixed=0 status=unsupported_color_math identical-native-row=1
Palace PPU gate: zero-fixed=1 leftover-fixed=0 (CGADSUB=0, CGWSEL=0)
```

This confirms an over-restrictive predicate, not that a recorded normal gameplay
sequence currently leaves this register combination behind. No matching
`unsupported_color_math` transition was found in the searched existing run and
evidence logs. Fix narrowly by recognizing proven no-op state; retain checks
for real colour-window, subtract, half-add, or subscreen effects.

### P2 — Fallback telemetry does not describe the view actually presented

[host_display.c:642](../src/host/host_display.c#L642) labels scenes from the
producer's `sim.view`; line 648 counts only `kSimView_AuthenticFallback`.
Consequently:

- SIM capture can be invalid while the enum remains `Enhanced`; the compositor
  draws native, but the metrics report Town and no fallback.
- A Palace upload/render failure described above leaves `view == SkyPalace`;
  metrics report Palace and no fallback.
- Town metadata also uses `Enhanced` with the master switch off; native SIM can
  therefore be labelled as Town 3D.
- Forced-blank navigation transitions count as fallback frames even though
  there is no visible native flash.

`Sim3D_LogViewTransition` helps with producer-side capture rejection, but cannot
observe a later presentation failure. Palace capture also collapses different
rejection causes into one bool without a reason-specific diagnostic.

Recommended direction: keep requested mode, resolved presentation, visibility,
and failure reason distinct in presentation-owned diagnostics. Count visible
unexpected changes only; do not derive the count solely from scene metadata.
This need not change the runner ABI or leak host policy into the capture layer.

## Mode and transition coverage

| Area | Review result |
| --- | --- |
| Action Diorama, including room transitions and extra vertical capture | No whole-view native fallback found after selected-scene core rendering fails; the caller requests an explicit error. Frame-generation rejection uses current enhanced planes, not a flat game view. Forced blank explicitly clears black. |
| SIM towns, menus, HUD and pickers | Invalid object metadata/raw-atlas availability does not automatically drop the town view. Raw OBJ planes preserve the projected town. Capture rejection can still reach native; see the no-op colour-state finding. Default picker handling stays enhanced (`AR_SIM3D_PICKER_TOPDOWN` is off). |
| World navigation and Advent | Partial brightness and empty-animation OAM are supported. Unknown OAM ownership, unavailable developed maps, or singular source matrices still reject capture. Renderer core failures are explicit errors, not native substitution. No new ordinary-play false rejection was demonstrated here. |
| Enhanced Sky Palace | Brightness changes are supported; native foreground ownership is protected by a BG1 winner mask. Whole-view render/upload fallback is still present and should be removed under the selected-mode policy. |
| Connected SIM globe background | A resource failure can latch the ordinary flat underlay until reset at `present_world_nav.c:4751`. The town remains enhanced, so this is not a whole-view native fallback. It is nevertheless an automatic visual downgrade and merits inclusion if the policy is “no silent mid-session reductions.” |
| Authentic comparison, disabled enhancements, out-of-scope title/story screens | Intentional native presentation, not a rendering failure. |
| Resize and device reset | Resources and retained present history are invalidated explicitly. A successful startup check cannot guarantee subsequent allocations or device lifetime. No new whole-view fallback defect was established in reset handling. |

SIM's fixed-colour-add and targeted-miracle profiles currently require full
brightness. Combined miracle/fade states remain a coverage gap, not a confirmed
visible regression from this review. Unknown OAM and overlay ownership must not
be admitted simply to suppress a fallback counter: preserving native UI
semantics still requires an understood capture contract.

## Startup checks versus runtime failures

The application already requires the GPU backend, depth support, and the core
shader pipeline at [main.c:1372](../src/main.c#L1372). It does not intentionally
wait until entering town to discover the absence of baseline GPU support.

Scene-sized render targets, mesh buffers, and scene assets still have runtime
allocation paths. Targets depend on current output size, and buffers can grow
with scene contents. Device loss or resource invalidation can also occur after
startup. These are genuine runtime risks, unlike the capture-predicate bug.
Move predictable preparation into launch/loading/mode-enable boundaries; keep
runtime checks for correctness without silently changing the selected mode.

## Verification and limits

Twelve focused CTest cases passed in an assertion-enabled Debug build:
`sim_world_navigation_palace`, `present_world_nav`, `present_world_nav_profiled`,
`present_world_nav_radial_fallback`, `sim_render_metadata`, `present_frame_order`,
`presentation_outcome`, `ppu_render_pipeline`, `diorama_capture_blend`,
`diorama_projection`, `performance_metrics`, and `session_fatal` (all names have
the `actraiser_` prefix). The extra real-PPU no-op probe reproduced finding 2.

Existing navigation tests include fault injection into scene/cloud operations;
the Palace caller in `present.c` is not exercised by the frame-order test, which
stubs `PresentCompositeScene`. Add end-to-end routing fault tests when fixing
finding 1. Passing these suites is not an exhaustive all-room playthrough or a
new Steam Deck validation. No performance benchmarks were run for this review.
