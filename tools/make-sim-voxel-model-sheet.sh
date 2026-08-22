#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
output_path="${1:-$repo_root/docs/research/sim-voxel-model-audit/voxel-model-audit-sheet.png}"
work_root="${TMPDIR:-/tmp}/actraiser-sim-voxel-model-sheet"
render_dir="$work_root/renders"
card_dir="$work_root/cards"
section_dir="$work_root/sections"
renderer="$work_root/sim_voxel_model_sheet"
magick_bin="${MAGICK:-/opt/homebrew/bin/magick}"
font_file="/System/Library/Fonts/HelveticaNeue.ttc"

cd "$repo_root"

mkdir -p "$render_dir" "$card_dir" "$section_dir" \
  "$(dirname "$output_path")"

cc -std=c11 -O2 -Isrc -Isrc/sim -I/opt/homebrew/include \
  tools/sim_voxel_model_sheet.c \
  src/scene3d_math.c \
  src/sim/sim3d_depth_pass.c \
  src/sim/sim_background_voxel_biome.c \
  src/sim/sim_background_voxel_lighting.c \
  src/sim/sim_background_voxel_model_cache.c \
  src/sim/sim_background_voxel_models.c \
  src/sim/sim_background_voxel_palette.c \
  src/sim/sim_background_voxel_project.c \
  src/sim/sim_background_voxel_proportions.c \
  src/sim/sim_background_voxel_region.c \
  src/sim/sim_background_voxel_surface.c \
  -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lSDL3 -lm \
  -o "$renderer"

"$renderer" "$render_dir"

sheet_width=2640
panel_width=420
panel_height=360
label_height=72
card_index=0
section_index=0
section_outputs=()

sections=(
  "Regional houses"
  "Landmarks and infrastructure"
  "Vegetation"
  "Bridges"
  "Construction"
)

for section in "${sections[@]}"; do
  section_cards=()
  while IFS=$'\t' read -r row_section label file; do
    if [[ "$row_section" != "$section" ]]; then
      continue
    fi
    card_index=$((card_index + 1))
    card_path="$card_dir/$(printf '%03d' "$card_index").png"
    panel_path="$card_dir/panel.png"
    label_path="$card_dir/label.png"
    "$magick_bin" \
      -size "${panel_width}x${panel_height}" xc:'#59646b' \
      "$render_dir/$file" -compose over -composite \
      "$panel_path"
    "$magick_bin" \
      -background '#171c21' -fill '#f1f4f6' \
      -font "$font_file" -pointsize 22 \
      -size "${panel_width}x${label_height}" -gravity center \
      caption:"$label" "$label_path"
    "$magick_bin" "$panel_path" "$label_path" -append \
      -bordercolor '#30383f' -border 2 "$card_path"
    section_cards+=("$card_path")
  done < <(tail -n +2 "$render_dir/manifest.tsv")

  section_index=$((section_index + 1))
  grid_path="$section_dir/grid-$(printf '%02d' "$section_index").png"
  padded_grid_path="$section_dir/grid-padded-$(printf '%02d' "$section_index").png"
  header_path="$section_dir/header-$(printf '%02d' "$section_index").png"
  section_path="$section_dir/section-$(printf '%02d' "$section_index").png"
  "$magick_bin" montage "${section_cards[@]}" \
    -font "$font_file" -tile 6x -geometry +10+10 \
    -background '#0d1115' "$grid_path"
  grid_height="$($magick_bin identify -format '%h' "$grid_path")"
  "$magick_bin" "$grid_path" -gravity north -background '#0d1115' \
    -extent "${sheet_width}x${grid_height}" "$padded_grid_path"
  "$magick_bin" -size "${sheet_width}x88" xc:'#222a31' \
    -font "$font_file" -pointsize 34 -fill '#f1c96b' \
    -gravity west -annotate +44+0 "$section" "$header_path"
  "$magick_bin" "$header_path" "$padded_grid_path" -append "$section_path"
  section_outputs+=("$section_path")
done

title_path="$section_dir/title.png"
footer_path="$section_dir/footer.png"
"$magick_bin" -size "${sheet_width}x188" xc:'#0a0e12' \
  -font "$font_file" -gravity north \
  -pointsize 52 -fill '#f5f7f8' -annotate +0+36 \
    'SIM MODE VOXEL MODEL AUDIT' \
  -pointsize 24 -fill '#aeb9c1' -annotate +0+112 \
    'Production builders · Ultra detail · Varied architecture · Material-aware lighting + AO · Per-model facing · Common scale' \
  "$title_path"
"$magick_bin" -size "${sheet_width}x112" xc:'#0a0e12' \
  -font "$font_file" -pointsize 22 -fill '#8f9ba3' -gravity center \
  -annotate +0+0 \
    'Flat audit datum; terrain relief and mountains are intentionally excluded because they are not voxel model builders.' \
  "$footer_path"

"$magick_bin" "$title_path" "${section_outputs[@]}" "$footer_path" \
  -append -strip "$output_path"

printf '%s\n' "$output_path"
