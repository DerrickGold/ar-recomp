package builder

import (
	"html"
	"strconv"
	"strings"
)

// assetTrackRowTemplate carries a revert control per slot. A file input can be
// filled but never emptied by a page, so "no replacement" needs its own
// affordance: without one an installed track could only ever be swapped for a
// different file, never returned to the ROM's own music. The hidden companion
// field is what the save actually reads -- a button press alone would be lost
// on submit.
const assetTrackRowTemplate = `<div class="asset-row" data-track="{ID}" data-source="{SRC}" style="--tint-h:{HUE}">
  <div class="asset-copy"><label for="track-{ID}">{NAME}</label><details class="asset-technical"><summary data-i18n="builder.technical_details">Technical details</summary><span class="asset-source">Manifest [music:{ID}] &middot; ROM source {SRC}</span></details></div>
  <div class="asset-picker"><input id="track-{ID}" name="track-{ID}" type="file" accept=".ogg,.oga,audio/ogg">
    <div class="asset-row-foot">
      <span class="asset-current" id="track-state-{ID}" data-i18n="builder.assets.not_installed">Not installed</span>
      <button type="button" class="asset-clear" hidden data-i18n="builder.assets.use_original">Use original</button>
      <button type="button" class="split-toggle" aria-expanded="false" data-i18n="builder.assets.split">Split by level</button>
    </div>
    <input class="asset-remove" name="track-remove-{ID}" type="hidden" value="0">
    <input class="split-change" name="split-change-{ID}" type="hidden" value="0">
    <div class="asset-split" hidden>
      <p class="split-note" data-i18n="builder.assets.split_help">Give this song its own track in chosen levels. Each one becomes a
      gated entry below, ready for a file. Only the levels whose maps actually play this song
      are listed &mdash; that comes from the ROM&rsquo;s own per-map script, so the choice
      cannot name a level the gate could never fire in.</p>
      <div class="split-regions"></div>
    </div>
    <div class="audio-compare">
      <div><span data-i18n="builder.assets.original_rom">Original ROM</span><audio class="original-audio" controls preload="metadata" hidden></audio></div>
      <div><span class="replacement-caption" data-i18n="builder.assets.selected_replacement">Selected replacement</span><audio class="replacement-audio" controls preload="metadata" hidden></audio></div>
    </div>
  </div>
</div>`

// trackTintHue gives each slot its own hue. Seventeen rows of identical
// styling, each several controls tall and now carrying nested variants, read as
// one undifferentiated column -- a faint wash of colour is enough to tell where
// one song ends and the next begins without adding a rule or a heading.
//
// Stepped by the golden angle rather than evenly divided: 360/17 would put
// neighbours 21 degrees apart, which at this alpha is no difference at all,
// while 137 degrees separates every adjacent pair as far as the wheel allows.
// Offset off pure red so the first rows do not read as an error state.
func trackTintHue(index int) int { return (30 + index*137) % 360 }

func renderAssetTrackRow(id, name, src string, hue int) string {
	return strings.NewReplacer(
		"{ID}", html.EscapeString(id),
		"{NAME}", html.EscapeString(name),
		"{SRC}", html.EscapeString(src),
		"{HUE}", strconv.Itoa(hue),
	).Replace(assetTrackRowTemplate)
}

func renderAssetTrackRows() string {
	var output strings.Builder
	for index, track := range assetTracks {
		output.WriteString(renderAssetTrackRow(
			track.ID, track.Name, track.Src, trackTintHue(index)))
	}
	return output.String()
}

// renderAssetRowPrototype emits one unattached row for the page to clone per
// gated variant. Variant rows cannot be server-rendered with the rest: which
// records exist depends on a manifest that can change while the builder is
// open, and the Assets tab re-reads it on every visit. Cloning the same
// prototype keeps their markup, styling and wiring identical to every other
// row instead of a second, drifting copy written in JavaScript.
func renderAssetRowPrototype() string {
	// The hue is a placeholder: a cloned variant row takes its parent slot's.
	return renderAssetTrackRow("__ID__", "__NAME__", "__SRC__", 0)
}
