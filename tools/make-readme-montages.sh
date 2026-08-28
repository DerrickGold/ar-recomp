#!/usr/bin/env bash
# Build the side-by-side comparison images the README embeds.
#
# Each before/after pair in assets/ is combined into ONE labelled montage so the
# README can place a single image instead of a two-column table. Re-run this
# after retaking any source screenshot; the outputs are committed.
#
# Requires ImageMagick 7 (`magick`, `montage`).
#
#   bash tools/make-readme-montages.sh
#
set -euo pipefail

cd "$(dirname "$0")/.."
A=assets
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

FONT=/System/Library/Fonts/Supplemental/Arial\ Bold.ttf
[ -f "$FONT" ] || FONT=/System/Library/Fonts/Supplemental/Arial.ttf
BG='#0d0f13'
FG='#e6e6e9'

# A macOS window screenshot: drop the drop-shadow border, then the 56px titlebar.
dechrome() {
  magick "$1" -bordercolor black -border 1 -trim +repage \
    -gravity north -chop 0x56 +repage "$2"
}

echo "==> builder-stages.webp (the builder, three moments)"
# Replaces the old builder-gui.png + builder-progress.png pair: one figure showing
# idle -> building -> done, which is what the README walks the reader through.
#
# NOT dechrome'd, unlike every montage below. The browser chrome is the POINT
# here: a visible 127.0.0.1 address bar is the fastest proof of the "nothing is
# uploaded" claim. Only the drop-shadow border comes off.
#
# WEBP, also unlike the rest. These are UI captures over a smooth sky gradient,
# which is close to the worst case for PNG: truecolour costs 7.8 MB, and
# quantising to a 256-colour palette -- what the SNES frames below do for free --
# visibly bands and mottles that gradient. WebP q82 is indistinguishable from
# truecolour at 160 KB, so the figure is LIGHTER than the two PNGs it replaces.
# The masters are lossless WebP for the same reason (2.6x smaller than PNG, and
# verified pixel-identical), so regenerating never compounds artifacts.
for n in 1 2 3; do
  magick "$A/builder-gui-$n.webp" -bordercolor black -border 1 -trim +repage \
    "$TMP/stage$n.png"
done
montage \
  \( "$TMP/stage1.png" -set label 'Choose your ROM — nothing is uploaded' \) \
  \( "$TMP/stage2.png" -set label 'Building — step list and progress dock' \) \
  \( "$TMP/stage3.png" -set label 'Done — Launch game, and the original 40-page manual' \) \
  -tile 1x3 -geometry +26+26 -background "$BG" -fill "$FG" \
  -font "$FONT" -pointsize 64 \
  "$TMP/stages.png"
magick "$TMP/stages.png" -resize 1500x -strip \
  -quality 82 -define webp:method=6 "$A/builder-stages.webp"

echo "==> widescreen-comparison.png (4:3 vs 16:9, stacked)"
dechrome "$A/comparison/bp-act2-normal.png"     "$TMP/ws-43.png"
dechrome "$A/comparison/bp-act2-wide-fixed.png" "$TMP/ws-169.png"
# Pad the 4:3 frame out to the widescreen frame's width so montage centres it.
# Stacked and centred, the extra width reads as symmetric — which is what the
# feature actually does — instead of looking like it was bolted onto one side.
WSW=$(magick identify -format '%w' "$TMP/ws-169.png")
magick "$TMP/ws-43.png" -background "$BG" -gravity center \
  -extent "${WSW}x" "$TMP/ws-43-pad.png"
montage \
  \( "$TMP/ws-43-pad.png" -set label 'Authentic 4:3' \) \
  \( "$TMP/ws-169.png" -set label 'Widescreen 16:9 — backgrounds streamed into the extra width' \) \
  -tile 1x2 -geometry +26+26 -background "$BG" -fill "$FG" \
  -font "$FONT" -pointsize 52 \
  "$TMP/ws.png"
magick "$TMP/ws.png" -resize 1500x -strip "$A/widescreen-comparison.png"

echo "==> diorama-comparison.png (flat vs diorama 3D)"
# Same paused frame, one setting toggled; the two captures differ by 12px of
# window height, so normalise before tiling.
H=$(magick identify -format '%h' "$A/diorama-off.png")
H2=$(magick identify -format '%h' "$A/diorama-hero.png")
[ "$H2" -lt "$H" ] && H=$H2
magick "$A/diorama-off.png"  -gravity north -crop "x${H}+0+0" +repage "$TMP/d-off.png"
magick "$A/diorama-hero.png" -gravity north -crop "x${H}+0+0" +repage "$TMP/d-on.png"
montage \
  \( "$TMP/d-off.png" -set label 'Flat' \) \
  \( "$TMP/d-on.png"  -set label 'Diorama 3D' \) \
  -tile 2x1 -geometry +22+22 -background "$BG" -fill "$FG" \
  -font "$FONT" -pointsize 84 \
  "$TMP/dio.png"
magick "$TMP/dio.png" -resize 1600x -strip "$A/diorama-comparison.png"

echo "==> shader-comparison.png (GPU effects off vs on)"
montage \
  \( "$A/shader-off.png" -set label 'Shader effects off' \) \
  \( "$A/shader-on.png"  -set label 'Rim light + depth of field' \) \
  -tile 2x1 -geometry +22+22 -background "$BG" -fill "$FG" \
  -font "$FONT" -pointsize 58 \
  "$TMP/shader.png"
magick "$TMP/shader.png" -resize 1600x -strip "$A/shader-comparison.png"

echo "==> hd-title-comparison.png (original vs HD logo)"
# Both frames letterbox the same content region; crop identically.
CROP=$(magick "$A/hd-title-original.png" -bordercolor black -border 1 -trim \
        -format '%wx%h+%X+%Y' info:)
# Trimming the letterbox puts the copyright line flush against the bottom edge,
# which crowds the montage label. Give it back 48px of the title screen's own
# black so the caption has room to sit.
HDH=$(( $(echo "$CROP" | cut -dx -f2 | cut -d+ -f1) + 48 ))
for pair in "original:hd-orig" "replaced:hd-new"; do
  magick "$A/hd-title-${pair%%:*}.png" -crop "$CROP" +repage \
    -background black -gravity north -extent "x${HDH}" "$TMP/${pair##*:}.png"
done
montage \
  \( "$TMP/hd-orig.png" -set label 'Original' \) \
  \( "$TMP/hd-new.png"  -set label 'HD replacement' \) \
  -tile 2x1 -geometry +22+26 -background "$BG" -fill "$FG" \
  -font "$FONT" -pointsize 52 \
  "$TMP/hd.png"
magick "$TMP/hd.png" -resize 1500x -strip "$A/hd-title-comparison.png"

echo
echo "==> quantising (these are palette-limited SNES frames; 256 colours is"
echo "    visually identical and roughly 4x smaller in the repo)"
for f in widescreen-comparison diorama-comparison shader-comparison hd-title-comparison; do
  magick "$A/$f.png" -colors 256 -define png:compression-level=9 "$A/$f.png"
done

echo
echo "Done:"
# builder-stages is deliberately absent from the quantise loop above and carries
# its own extension -- see its section for why it is WebP and not a 256-colour PNG.
for f in builder-stages.webp widescreen-comparison.png diorama-comparison.png \
         shader-comparison.png hd-title-comparison.png; do
  printf '  %-32s %s  %s\n' "$A/$f" \
    "$(magick identify -format '%wx%h' "$A/$f")" \
    "$(du -h "$A/$f" | cut -f1)"
done
