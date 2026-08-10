# CRT post-processing

A fullscreen shader pass that simulates a curved colour CRT: barrel glass,
scanlines with a luminance-driven beam profile, an aperture-grille phosphor
mask, horizontal signal bandwidth limiting, convergence fringing and corner
falloff.

Settings live in **Video > CRT** (`kSettingCat_Crt`). The master toggle is a
player row; the seven knobs behind it are developer-only (they appear when
`show_debug_settings` is on) because their defaults *are* the intended look
rather than a performance trade-off.

## Why there is only one integration point

`PresentFrame()` is the single function every render mode funnels through — flat
2D, diorama, sim3D enhanced and world-navigation 3D. It owns the scene target,
mode-specific composite, resolve, and terminal host-UI pass in that order. It
has exactly three callers: the two present paths in `host_display.c` and the
screenshot path in `dev_tools.c`.

```
PresentFrame(...);                /* begin -> scene -> resolve -> UI */
SDL_RenderPresent(renderer);
```

The internal CRT begin/end calls are no-ops when the effect is off, leaving the
scene path byte-for-byte untouched.

The inspector, cheat disclosure, manual and settings overlay deliberately render
after the resolve. They are host UI, not part of the emulated video signal;
keeping them outside the scene target also prevents a 4:3 fullscreen resolve
from painting black pillar bars over them. The developer screenshot path calls
the same function, so captures still match the live window.

## The trap: "back to the backbuffer"

While the effect is engaged, `SDL_SetRenderTarget(renderer, NULL)` is **wrong**.
Anything that restores the target that way drops its drawing onto the backbuffer,
behind the resolve, where it is silently overwritten — no error, just missing
pixels.

Use `CrtPost_BaseTarget()`, which returns `NULL` when the effect is off and the
scene target while it is engaged. Four sites depend on this today (three in
`present.c`, one in `diorama.c`). **Any new render-to-texture code must use it.**

## Two pitches, deliberately different

Mixing these up is the classic way CRT shaders end up looking wrong.

| | Follows | Why |
|---|---|---|
| Scanlines | **Source** (224 lines) | A property of the signal — the count must hold at any window size |
| Phosphor mask | **Output** pixels, uncurved | A property of the glass — it must not warp with the picture |
| Bandwidth | **Source** pixels | Models the signal feeding the tube |
| Convergence | **Output** pixels | A property of the tube |

Taking the mask pitch from logical rather than output pixels is what makes these
shaders shimmer into moiré at non-integer scales — and non-integer is the normal
case here, between the 7:6 `kPixelAspect_Crt43` stretch and a widescreen width
that varies with the window.

Everything geometric works in **image space**: the letterboxed viewport is passed
in and uv is remapped into it. Without that, curvature would stretch across the
black bars and the scanline count would run against the window height instead of
the picture. The rect comes from SDL's own logical-presentation rect where SDL
owns the letterboxing (flat mode), and from the computed viewport where the path
disables logical presentation and does its own (widescreen and 3D).

## Linear light

All modulation happens in linear light — beam intensity, phosphor absorption and
lens falloff are linear-light phenomena, but the frame arrives gamma-encoded.
Modulating the encoded values directly (as the first version did) crushes
midtones far harder than a real tube and forces the brightness knob up to
compensate.

Gamma 2.0 (`x*x` / `sqrt`) rather than the true sRGB 2.2 curve, on purpose: this
runs on up to nine taps per pixel across the whole screen, where `pow()` is not
free and the residual error is well below the tuning resolution of these knobs.

The beam profile's width is driven by local luminance, so bright lines bloom
across the gap while dark lines stay thin. A constant-depth scanline reads as
"image plus overlay"; this reads as an emitting surface.

## Regression check

The plumbing has its own bisect, separate from the shader:

```bash
AR_CRT_PASSTHROUGH=1 ./build/ActRaiserRecomp ar.sfc --config settings.ini
```

This engages the render target but blits straight back with no shader. **Output
must be byte-identical to the effect being off.** If it is not, the breakage is
in the target plumbing (most likely a missed `CrtPost_BaseTarget()` site), not in
the shader. It is a test hook, deliberately not exposed as a settings row.

Capturing frames for comparison needs a **pinned battery save**: the input replay
presses buttons against whatever is in `saves/save.srm`, so a drifting save lands
the replay in a different scene and silently invalidates any A/B. Copy the save
back before each run.

## Performance

Measured on Apple silicon at 3420x2082 with the effect on versus off: no
detectable difference (0.4ms present either way, uncapped). The frame rate is
bound by the fixed-timestep emulation, not the GPU.

The shader costs up to nine texture taps per pixel — three bandwidth taps for
each of the three convergence-split channels. Both collapse when their knob is
zero. If a weaker GPU ever needs it, that tap count is the thing to cut.

## Known gaps

- **HDR.** The obvious next step, and the linear-light work above is its
  prerequisite. A real tube drives small bright areas far above its average
  output; in SDR that headroom does not exist, which is why brightness has to be
  lifted globally. SDL 3.4 exposes what is needed
  (`SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER` with
  `SDL_COLORSPACE_SRGB_LINEAR`, plus HDR headroom properties and float render
  targets). Note it changes the output colorspace for the *whole* app, so the
  HUD, overlay and screenshot paths all need review.
- **Mask pitch on HiDPI.** The grille is a fixed 3 output pixels, which on a 2x
  display is 1.5 logical pixels — fine enough that it mostly desaturates rather
  than reading as phosphors. Scaling the pitch with the image would look more
  authentic but departs from "the mask lives on the glass".
- **Only an aperture grille.** Most consumer sets used slot or shadow masks,
  which look quite different.
- **No phosphor persistence.** One extra target plus a decay blend would give
  both motion trails and burn-in, and is the closest reachable approximation to
  a beam-drawn image at 60Hz.
- **NTSC composite artifacts** (dot crawl, chroma bleed) would need signal-domain
  simulation — a separate project, not a knob.
