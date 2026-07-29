#!/usr/bin/env bash
# Shrink the images the README embeds to sane web sizes.
#
# GitHub renders README images at roughly 880 CSS px wide, so a 3644px capture
# costs megabytes to deliver something nobody sees at full size. This resizes
# every embedded still to 1600px wide (still 2x for retina), quantises the
# palette-limited game frames, and runs lossless GIF frame optimisation.
#
# Idempotent: re-running on already-optimised files is a no-op in practice.
# Run it after adding new screenshots, then commit the result.
#
#   bash tools/optimize-readme-assets.sh
#
# Requires ImageMagick 7. Operates in place — the sources are the committed
# assets, so check `git diff --stat` before committing.
set -euo pipefail

cd "$(dirname "$0")/.."
A=assets
MAXW=1600

before=$(du -ck $(grep -o '](/assets/[^)]*)' README.md | tr -d '](' \
          | sed 's/)$//;s|^|.|') | tail -1 | cut -f1)

# Game frames: SNES-derived, palette-limited. 256 colours is visually identical.
for f in title mode7 hud-scaling sim3d-detail worldnav-3d; do
  [ -f "$A/$f.png" ] || continue
  magick "$A/$f.png" -resize "${MAXW}x>" -colors 256 \
    -define png:compression-level=9 -strip "$A/$f.png"
  printf '  %-28s %s\n' "$f.png" "$(du -h "$A/$f.png" | cut -f1)"
done

# Builder UI: box art has real gradients, so resize only — no quantisation.
for f in builder-gui builder-progress builder-run-script; do
  [ -f "$A/$f.png" ] || continue
  magick "$A/$f.png" -resize "${MAXW}x>" \
    -define png:compression-level=9 -strip "$A/$f.png"
  printf '  %-28s %s\n' "$f.png" "$(du -h "$A/$f.png" | cut -f1)"
done

# GIFs: lossless inter-frame optimisation — no requantisation, no frame
# dropping, identical pixels on screen.
#
# DO NOT run ImageMagick's GIF optimisers over assets/*.gif. It corrupts them.
#
# `-coalesce -layers OptimizeTransparency -layers OptimizeFrame` (and plain
# `-layers Optimize` too) emits a file with MIXED frame disposal — Background,
# None and Previous in one stream — while cropping frames to their changed
# region. Partial frames then composite onto the wrong base and the animation
# degrades into near-black noise. Measured on diorama.gif: 49-93% of pixels in
# a coalesced frame differed from the source, and the output still *looked*
# plausible by file size alone, which is exactly why it slipped through.
#
# A correctly encoded export here has uniform dispose={None}. Check with:
#   magick identify -format '%D\n' assets/foo.gif | sort -u
# and verify visually after ANY re-encode — file size is not evidence.
#
# To make them smaller, use a tool that handles GIF disposal properly:
#   brew install gifsicle && gifsicle -O3 --batch assets/*.gif
# or re-export from the source video at a lower frame rate, which is the only
# thing that really helps: these are orbiting 3D camera moves where nearly
# every pixel changes every frame, so inter-frame compression has little to
# work with.

after=$(du -ck $(grep -o '](/assets/[^)]*)' README.md | tr -d '](' \
         | sed 's/)$//;s|^|.|') | tail -1 | cut -f1)
echo
echo "README image payload: $((before/1024))M -> $((after/1024))M"
