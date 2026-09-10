package localizationkit

import (
	_ "embed"
	"encoding/json"
)

//go:embed data/native-hud-labels.json
var nativeHUDLabelJSON []byte

// Emission order, so a supplemented pack is byte-identical run to run: a Go
// map alone would reorder the messages between builds.
var nativeHUDLabelIDs = []string{
	"action.hud.act_label",
	"action.hud.enemy_label",
	"action.hud.player_label",
	"action.hud.score_label",
	"action.hud.time_label",
	"sim_sky.hud.angel_label",
	"sim_sky.hud.context_label",
	"sim_sky.hud.sp_label",
}

// Every release must carry the action bar, which has been transcribed for all
// of them. The status bar is keyed by ID rather than position precisely so a
// release can be missing one: a transcription nobody has read off that ROM is
// absent here, never filled in from a neighbour that happens to share the
// template's tile indices.
var nativeHUDRequiredIDs = nativeHUDLabelIDs[:5]

var nativeHUDTranscriptions = func() map[string]map[string]string {
	var labels map[string]map[string]string
	if err := json.Unmarshal(nativeHUDLabelJSON, &labels); err != nil {
		panic("invalid HUD transcriptions: " + err.Error())
	}
	known := make(map[string]bool, len(nativeHUDLabelIDs))
	for _, id := range nativeHUDLabelIDs {
		known[id] = true
	}
	for profile, row := range labels {
		for id := range row {
			if !known[id] {
				panic("unknown HUD transcription " + id + " for " + profile)
			}
		}
		for _, id := range nativeHUDRequiredIDs {
			if row[id] == "" {
				panic("missing HUD transcription " + id + " for " + profile)
			}
		}
	}
	return labels
}()

// These are transcriptions of the identified ROMs' packed graphical
// HUD lettering, not byte-decoder strings. Multiple partial letters can occupy
// one tile. The profile is selected only after the complete ROM identity and
// destination/font census pass. Optional author routes preserve compatibility
// with earlier complete source packs that retained these as graphics only.
// JP's HUD is English too: its tiles $60-$73 equal US $01-$14 byte-for-byte;
// the JP $02:8D27 template and $00:A495 ENEMY writer select those tiles.
//
// Sharing a tile index proves nothing about the word, and neither does not
// sharing one: German's PLAYER slot uses the same $09-$0E the US one does and
// reads SPIELER, while its sim context label moved to $80-$83 and still reads
// SIM. Every entry here was read off that release's own decompressed font,
// which is why French keeps its accent (Cité) and its shorter ANGE, and why
// Japanese has no context label at all: that one spans ten cells as a
// bracketed CRT beside a separate AREA, which is not the six-cell shape this
// table describes.
func nativeHUDLabels(profile string) []AuthorMessage {
	labels := nativeHUDTranscriptions[profile]
	if len(labels) == 0 {
		return nil
	}
	messages := make([]AuthorMessage, 0, len(nativeHUDLabelIDs))
	for _, id := range nativeHUDLabelIDs {
		text, found := labels[id]
		if !found {
			continue
		}
		messages = append(messages, AuthorMessage{ID: id,
			Operations: []AuthorOperation{{Op: "text", Value: text}, {Op: "end"}}})
	}
	return messages
}
