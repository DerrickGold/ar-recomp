# Spec — RR1: decouple render resolution from window/display resolution

**Status:** authored, not implemented. Awaiting the audit in §9.

Source locations were refreshed on 2026-08-12. References use symbols instead
of line numbers so the proposal survives routine file reorganization.

## 1. Problem

The engine has no concept of a *render* resolution—only a window resolution.
`SDL_GetRenderOutputSize` returns the window's physical pixel size, and
`AppBoot_CreateVideo` (`src/main.c`) requests
`SDL_WINDOW_HIGH_PIXEL_DENSITY`, making that as large as the panel allows.
Several presentation paths derive geometry from it.

Two consequences:

1. **Cost.** The 3D paths build projection matrices at native resolution in
   `Diorama_Composite` (`src/diorama/diorama.c`) and `PresentSim3D`
   (`src/present_sim3d.c`), using the viewport from
   `ComputePresentationViewport` (`src/present.c`). On a 4K panel they genuinely
   rasterize 4K geometry. There is no way to trade resolution for framerate.

2. **We deny external upscalers.** Even in fullscreen we hand the compositor a
   native-resolution surface. gamescope's FSR (and any comparable
   compositor/driver upscaler) only engages when the application presents
   something *smaller* than the output. Presenting at native means there is
   nothing to upscale. **This is the primary motivation: the goal is not to
   implement an upscaler, it is to stop preventing one.**

Note what is NOT the problem: the flat 2D path already renders at SNES
dimensions (256x224) and is scaled by `SDL_SetRenderLogicalPresentation`. A
render-resolution setting changes nothing there. This is a 3D-path and a
present-surface concern.

## 2. Goal

A user-selectable render resolution such that the game *draws and presents* at
that resolution, leaving any upscaling to the compositor, driver, or display.

Explicit non-goal: implementing FSR/DLSS/XeSS ourselves. SDL3's 2D renderer
exposes no upscaler (verified: no such hint in `SDL_hints.h`, no scale mode
beyond nearest/linear/pixelart in `SDL_render.h`). Hand-writing FSR1 would mean
authoring MSL *and* HLSL *and* SPIR-V — and note the existing GPU shader effects
are hand-written MSL only, i.e. macOS-only today.

## 3. Chosen approach — shrink the drawable

Reduce the size of the surface we render to and present, so
`SDL_GetRenderOutputSize` *naturally* returns the smaller number. Every existing
consumer then becomes correct with no change, because they all already size
themselves from that call.

Rejected alternative—**logical presentation as a scaler**: use
`SDL_SetRenderLogicalPresentation` around `Diorama_Composite` and `PresentSim3D`
instead of disabling it. This fixes the GPU-cost half, but the *presented surface
stays native*, so it does not achieve goal (2). It remains a reasonable fallback
if §3's mechanism proves unavailable on a platform—see §7 R3.

### 3a. Mechanism per window mode

| Mode | Mechanism |
|---|---|
| `kWindowMode_Exclusive` | `SDL_SetWindowFullscreenMode()` to a mode at/near the requested resolution. |
| `kWindowMode_Borderless` | Cannot change the display mode by definition. Requires the fallback (§7 R3) or is documented as unsupported. |
| `kWindowMode_Windowed` | Suppress `SDL_WINDOW_HIGH_PIXEL_DENSITY` so the drawable is points-not-pixels, and/or size the window to the requested resolution. |

**This table is the least certain part of the spec and is what §9 must settle.**

## 4. Setting

```
render_resolution : enum, kApply_Restart, kSettingCat_Display
  Native (default) | 1080p | 900p | 720p | 540p
```

`Native` must be the default and must reproduce today's behavior exactly —
byte-identical output, no new code path taken.

Restart-scoped, matching the `gpu_shaders_enabled` descriptor in
`src/settings.c`: this touches window/renderer creation, which is fixed for the
process lifetime. A live-apply version is a later enhancement, not this change.

Label honesty (the R13/R8 lesson): a row must not offer a resolution the display
cannot produce, and if the request is refused the menu must not claim it was
honored. Either enumerate real modes via `SDL_GetFullscreenDisplayModes()` or
read back the achieved drawable size and publish it (mirroring
`Settings_SetHostVsyncActive` from `src/host/host_display.c`).

## 5. What does NOT need to change (verified)

This is why the estimate is ~1 day rather than a subsystem rewrite:

- **No render target.** No `SDL_TEXTUREACCESS_TARGET` texture, no
  `SDL_SetRenderTarget` plumbing, no device-reset reload path for it. The
  drawable *is* the smaller surface.
- **All current `SDL_GetRenderOutputSize` consumers appear structurally
  compatible.** They ask the renderer how big it is; the answer simply becomes
  smaller. The calls live in `src/dev/dev_tools.c`, `src/diorama/diorama.c`,
  `src/host/host_display.c`, `src/present.c`, and `src/settings_overlay.c`.
- **Input mapping has exactly ONE conversion point**, not four as first feared:
  `HostDisplay_WindowPointToOutput` (`src/host/host_display.c`) already scales
  window-client coordinates to renderer-output pixels by the
  window-size/output-size ratio, explicitly to cover high-DPI backing scale.
  Callers in `src/dev/dev_tools.c` and `src/main.c` pass output coordinates
  onward, and `src/settings_overlay.c` works purely in output space.
  **A larger window with a smaller drawable is the same arithmetic as high-DPI,
  in the other direction.** This must be re-verified, not assumed — §9 R1.
- **The 2D flat path** already renders at SNES dimensions.

## 6. What DOES need to change

1. `settings.h` / `settings.c` — the enum, labels, description, `kApply_Restart`.
2. `AppBoot_CreateVideo` in `src/main.c`—conditionally drop
   `SDL_WINDOW_HIGH_PIXEL_DENSITY` and request the display mode after creation.
3. `src/host/host_display.c`—publish the achieved drawable size for the menu;
   possibly adjust `HostDisplay_CalculateWindowSize`.
4. Interaction with **R6 pixel density**: `Settings_HostPixelDensity` and
   `WindowScaleInPoints` document and implement the split between physical
   renderer pixels and window points. Changing the drawable changes that ratio.
   **Review that contract first; this is the most likely place to introduce a
   subtle regression.**
5. `docs/settings-system.md` plus a settings unit test.

## 7. Risks

- **R1 — input mapping.** If `WindowPointToOutput`'s ratio does not in fact cover
  the shrunk-drawable case, mouse clicks land in the wrong place: silent and
  annoying. Highest-likelihood regression.
- **R2 — HUD/menu scale.** R6's density work assumes a particular
  points-vs-pixels relationship (§6.5).
- **R3 — gamescope may ignore the request.** gamescope is documented to suppress
  mode-change events and misreport refresh (finding R8). If it also ignores
  `SDL_SetWindowFullscreenMode`, then on Deck the correct lever is
  `gamescope -w 1280 -h 720 -W 1280 -H 800 -F fsr` in the launch options — an
  external config, not our code. **If so, the Deck case is not ours to solve and
  this spec should say so rather than shipping a setting that silently does
  nothing there.**
- **R4 — Wayland.** Fullscreen mode setting is compositor-mediated; a Wayland
  compositor may refuse a mode change outright.
- **R5 — non-integer scaling artifacts.** 720p→800p on a Deck OLED is a
  non-integer ratio; the letterbox math (`ComputePresentationViewport`,
  `src/present.c`) must not produce off-by-one bars or a half-pixel offset.
- **R6 — screenshots.** `WriteFramebufferPpm` / `AR_SHOT` capture at output size,
  so shot dimensions change with the setting. Acceptable, but the PPM byte-
  identity gates used elsewhere must not be run across a resolution change.

## 8. Acceptance

ROM-free: settings round-trip; `Native` produces a byte-identical window/renderer
setup to today (assert the flags and mode calls, not just the output); canary
green; full link.

On-device: at 720p on a >1080p display the 3D paths visibly cost less
(`AR_PERF=1` present-ms drops) and the image is upscaled by the
compositor/display rather than letterboxed into a corner; mouse hit-testing in
the settings overlay and the scene inspector still lands correctly; HUD scale
looks right at both `Native` and 720p; **on Deck, gamescope FSR actually engages**
(the surface it receives is 720p).

## 9. What the audit must settle before implementation

1. **R1 in detail.** Trace `HostDisplay_WindowPointToOutput` for
   window=2560x1440, drawable=1280x720. Is the ratio correct, and do all three
   callers plus every `settings_overlay.c` hit-test stay correct?
2. **Does the mechanism exist per mode?** Confirm against the SDL3 headers what
   `SDL_SetWindowFullscreenMode` guarantees, and whether a *borderless* or
   *windowed* window can have a drawable smaller than its client area at all
   without a render target. If windowed cannot, say so — the setting may have to
   be fullscreen-only.
3. **gamescope/Wayland reality (R3/R4).** From documentation only.
4. **R2** — enumerate every place the points/pixels ratio is assumed.
5. **Are the current output-size consumers correct?** Check each for a
   hidden assumption that output size == window size or == display size.
6. **R5** — the letterbox arithmetic at a non-integer target ratio.
