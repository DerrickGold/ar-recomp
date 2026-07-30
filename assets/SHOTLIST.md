# README image assets

What every image the [README](../README.md) embeds is, how the composed ones are
built, and the conventions to follow when retaking or adding one.

## Build the composed images

Four of the README images are montages built from committed source pairs. Never
hand-edit them — regenerate:

```sh
bash tools/make-readme-montages.sh
```

Then shrink any new **stills** for the web (safe to re-run; it does not touch
GIFs — see the warning below):

```sh
bash tools/optimize-readme-assets.sh
```

Both need ImageMagick 7 (`brew install imagemagick`).

## What's embedded

| Image | Kind | Source |
|---|---|---|
| `title.png` | still | hero |
| `builder-run-script.png` | still | unpacked bundle folder |
| `builder-stages.webp` | **montage** | `builder-gui-1.webp` + `-2` + `-3` (idle, mid-build, done + manual) |
| `widescreen-comparison.png` | **montage** | `comparison/bp-act2-normal.png` + `bp-act2-wide-fixed.png` |
| `diorama.gif` | motion | camera orbiting a Fillmore stage |
| `diorama-comparison.png` | **montage** | `diorama-off.png` + `diorama-hero.png` |
| `3dtown.gif` | motion | 3D town, camera moving |
| `sim3d-detail.png` | still | close on Fillmore's temple |
| `3dtown-zoom.gif` | motion | zoom out to the full landmass + clouds |
| `worldnav-3d.png` | still | world navigation, approaching Bloodpool |
| `shader-comparison.png` | **montage** | `shader-off.png` + `shader-on.png` |
| `mode7.png` | still | Mode 7 at internal render scale |
| `hd-title-comparison.png` | **montage** | `hd-title-original.png` + `hd-title-replaced.png` |
| `overlay.gif` | motion | navigating the settings overlay |
| `hud-scaling.png` | still | independently scaled sim HUD |

Kept but not currently embedded: `menu.png`, `sim.png`, `fillmore-boss.png`,
`bloodpool-act2.png`, and the full `comparison/` set (normal / expanded /
wide-fixed triples for Bloodpool Act 1 & 2, Death Heim, and the Sky Palace).
`comparison/*-expanded.png` is the raw widescreen mode with unfixed edge
columns — a diagnostic view, not a showcase one. Pull any of these in if a
section needs another image.

## Capture conventions

Keep these consistent so the README reads as one set rather than a scrapbook.

| | |
|---|---|
| **Window** | 1920×1080 or larger, *Screen ratio* 16:9, *Pixel aspect* 4:3, *Window mode* Windowed |
| **Render scale** | 3 or higher (Settings → Video → Display → Render scale) — this is what makes the 3D town and Mode 7 shots look sharp rather than upscaled |
| **Method** | `F2` writes a full snapshot including a PNG screenshot into `runs/latest/`, with no window chrome. An OS window capture is fine too — `tools/make-readme-montages.sh` knows how to strip the macOS titlebar and drop shadow. |
| **Format** | PNG. Don't pre-shrink; `optimize-readme-assets.sh` handles sizing. |
| **Pairs** | Change **one** setting and nothing else: same room, same camera, same frame. Pause with `P`, capture, toggle the setting, capture again. A mismatched pair is worse than no pair. |
| **HUD** | Leave it visible — it's part of what the presentation modes do. |
| **Debug rows** | Turn *Show debug settings* **off** before capturing any menu shot, so it reflects what a player sees. |

To reach a specific room quickly, set `AR_WARP` (see
[docs/manual.md](../docs/manual.md#verified-ar_warp-targets)) and press `F6`
from a transition-capable state.

## Per-shot notes

Things worth knowing if you retake one of these.

**`diorama-off.png` / `diorama-hero.png`** — a properly matched pair: same
paused frame (TIME 279), only *Diorama 3D* toggled. That's why the montage
works. Preserve that discipline if you replace them. For the tilted shot, put
*Skybox* on **Plane + skybox** and *Shoebox walls* **On** so no void shows at
the box edges.

**`shader-off.png` / `shader-on.png`** — currently *not* frame-matched (TIME 282
vs 287, and the camera shifted slightly between captures). The depth-of-field
difference still reads, but this is the weakest pair in the set; worth retaking
from a single paused frame. Leave *Soft shadow blur* and *Scroll interpolation*
**off** — both ship off by default with known issues.

**`3dtown.gif` / `3dtown-zoom.gif`** — pickers now stay in the tilted 3D space,
so you no longer have to close one before capturing. Zoomed out, the world-map
underlay and cloud deck are what make the shot.

**`hd-title-original.png` / `hd-title-replaced.png`** — needs real replacement
art at `game-assets/hd/title-logo.png`. **Licensing:** whatever art appears here
gets committed as a screenshot, so use your own work, not a rip or an upscale of
the original. See
[what can and can't be committed](../docs/contributing.md#what-can-and-cant-be-committed-here).

**`builder-gui-1/2/3.webp`** — the three builder captures, montaged into
`builder-stages.webp` by `tools/make-readme-montages.sh`. Shoot all three at the
SAME window size; the montage stacks them and an identical frame is what makes
the three read as one sequence. Stage 1 is the ROM picker, stage 2 mid-build
(any percentage), stage 3 the finished build with the **Manual** tab open.

These are the shots where browser chrome *helps*: a visible `127.0.0.1` address
bar is the fastest proof of the "nothing is uploaded" claim, so this montage is
the one that is NOT `dechrome`'d. Crop the session token out if you'd rather not
show it (it's per-process and worthless once the builder closes).

**Why these are WebP when everything else is PNG.** They are UI over a smooth sky
gradient, which is the worst case for PNG: truecolour costs 7.8 MB, and the
256-colour quantise the SNES frames take for free visibly bands and mottles that
gradient. WebP q82 is indistinguishable at 160 KB. The masters are LOSSLESS WebP
(2.6x smaller than PNG, verified pixel-identical with `magick compare -metric AE`),
so re-running the montage never compounds artifacts.

## Page weight

The README ships about 16 MB of images, ~11 MB of which is the four GIFs.
That's the ceiling of what feels reasonable; if you add more motion, retire
some. The builder montage replaced two PNGs totalling 862 KB with one 160 KB
figure, so it bought a little headroom back.

**Never run ImageMagick's GIF optimisers over these files.** Both
`-layers Optimize` and the OptimizeTransparency/OptimizeFrame pair produce
mixed frame disposal and visually destroy the animation while still looking
fine by file size. A good export has uniform `dispose={None}`; verify with
`magick identify -format '%D\n' assets/foo.gif | sort -u`, and always eyeball
the result after a re-encode. To shrink them, use `gifsicle -O3` or re-export
from the source video at a lower frame rate.
