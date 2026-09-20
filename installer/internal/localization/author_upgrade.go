package localization

import (
	"fmt"
	"slices"
	"strings"
	"unicode/utf8"

	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

// Historical rendering policy belongs to this versioned conversion, never to
// the v2 renderer. Conversion produces a detached, reviewable project; saving,
// previewing, publishing and installing remain separate existing transactions.
type AuthorUpgradeReport struct {
	FromVersion         int      `json:"fromVersion"`
	ToVersion           int      `json:"toVersion"`
	Messages            int      `json:"messages"`
	AliasesPreserved    int      `json:"aliasesPreserved"`
	AliasesMaterialized int      `json:"aliasesMaterialized"`
	Notes               []string `json:"notes"`
}

func (p *AuthorProject) UpgradeV2(newID string) (*AuthorProject, AuthorUpgradeReport, error) {
	var empty AuthorUpgradeReport
	if p == nil || p.pack == nil {
		return nil, empty, fmt.Errorf("open a v1 project to upgrade")
	}
	if newID == "" || strings.EqualFold(newID, p.pack.manifest.metadata.ID) || strings.EqualFold(newID, "native-us") {
		return nil, empty, fmt.Errorf("choose a new package ID for the upgraded copy; the original is retained")
	}
	pack, report, err := upgradeAuthorPackV2(p.pack)
	if err != nil {
		return nil, empty, err
	}
	metadata := pack.manifest.Metadata()
	metadata.ID = newID
	pack, err = pack.WithMetadata(metadata)
	if err != nil {
		return nil, empty, err
	}
	next := p.clone()
	next.pack = pack
	return next, report, nil
}

func upgradeAuthorPackV2(pack *AuthorPack) (*AuthorPack, AuthorUpgradeReport, error) {
	report := AuthorUpgradeReport{FromVersion: 1, ToVersion: 2, Notes: []string{
		"Review the inferred styling and playback before installing the upgraded copy.",
		"Wording, controls, translation progress and font bytes are retained. Comments remain beside their messages; source lines are regenerated.",
	}}
	if pack == nil || pack.manifest.Version() != 1 {
		return nil, report, fmt.Errorf("upgrade requires a version 1 language pack")
	}
	converted := make(map[string]AuthorMessage)
	usedStyles := make(map[string]bool)
	for id := range pack.workspace.messageScript {
		message, err := resolvedAuthorMessage(pack.workspace, id)
		if err != nil {
			return nil, report, err
		}
		message = inferV1Appearance(message, pack.manifest.metadata.SourceProfile)
		converted[id] = message
		usedStyles[message.Appearance.Style.Treatment] = true
		for _, op := range message.Operations {
			usedStyles[op.Style.Treatment] = true
		}
		report.Messages++
	}
	// Numeric fields were rendered outside v1 packs. Make that previously hidden
	// presentation explicit in every upgraded copy.
	var added []AuthorMessage
	for _, message := range append(nativeHUDValues(), nativeWorldLabel()) {
		if _, exists := converted[message.ID]; exists {
			continue
		}
		message = inferV1Appearance(message, pack.manifest.metadata.SourceProfile)
		converted[message.ID] = message
		added = append(added, message)
		usedStyles[message.Appearance.Style.Treatment] = true
		report.Messages++
	}
	definitions := v1TreatmentDefinitions(usedStyles)
	scripts := make([]*AuthorScript, len(pack.workspace.scripts))
	for i, source := range pack.workspace.scripts {
		var messages []AuthorMessage
		for _, original := range source.messages {
			message := converted[original.ID]
			if original.Alias != "" {
				target := converted[original.Alias]
				if message.Appearance == target.Appearance && slices.Equal(authorPresentation(message.Operations), authorPresentation(target.Operations)) {
					message = AuthorMessage{ID: original.ID, Alias: original.Alias}
					report.AliasesPreserved++
				} else {
					report.AliasesMaterialized++
				}
			}
			messages = append(messages, message)
		}
		var styles []AuthorTreatment
		if i == 0 {
			styles = definitions
		}
		var err error
		scripts[i], err = emitUpgradedScript(source, messages, styles)
		if err == nil && i == 0 && len(added) != 0 {
			extra, emitErr := EmitAuthorScriptVersion(added, source.path, 2)
			if emitErr != nil {
				return nil, report, emitErr
			}
			scripts[i], err = ParseAuthorScriptVersion(scripts[i].text+"\n"+extra.text, source.path, 2)
		}
		if err != nil {
			return nil, report, err
		}
	}
	manifest := pack.manifest
	for _, value := range manifest.packValues {
		if value.key != "version" {
			continue
		}
		var err error
		manifest, err = ParsePackManifest(manifest.text[:value.start]+"2"+manifest.text[value.end:], manifest.path)
		if err != nil {
			return nil, report, err
		}
		break
	}
	if usedStyles["hud"] || usedStyles["hud-frame"] {
		fonts := manifest.Fonts()
		fonts.Roles = append(fonts.Roles, PackFontRole{Name: "hud", Primary: fonts.Primary, Fallback: slices.Clone(fonts.Fallback)})
		var err error
		manifest, err = manifest.WithFonts(fonts)
		if err != nil {
			return nil, report, err
		}
	}
	workspace, err := newAuthorWorkspace(manifest.metadata.SourceProfile, manifest.metadata.Coverage, scripts, pack.workspace.progress.text)
	if err != nil {
		return nil, report, err
	}
	next := &AuthorPack{manifest: manifest, fonts: pack.fonts, progressPresent: pack.progressPresent}
	next, err = next.withWorkspace(workspace)
	return next, report, err
}

func emitUpgradedScript(source *AuthorScript, messages []AuthorMessage, definitions []AuthorTreatment) (*AuthorScript, error) {
	emitted, err := EmitAuthorScriptVersion(messages, source.path, 2, definitions...)
	if err != nil {
		return nil, err
	}
	// Keep translator comments at the same message, without treating any old
	// literal angle bracket or backslash as new markup. Only the emitter quotes
	// decoded text; editing source strings with tag substitutions is unsafe.
	comments := func(text string) string {
		var out strings.Builder
		_ = authorLines(text, func(line string, _, _, _ int) error {
			trimmed := strings.TrimSpace(strings.TrimPrefix(line, "\ufeff"))
			if strings.HasPrefix(trimmed, "#") || strings.HasPrefix(trimmed, ";") {
				out.WriteString(line)
				out.WriteByte('\n')
			}
			return nil
		})
		return out.String()
	}
	text := emitted.text
	for i := len(messages) - 1; i >= 0; i-- {
		span := source.spans[i]
		retained := comments(source.text[span.bodyStart:span.end])
		at := emitted.spans[i].bodyStart
		text = text[:at] + retained + text[at:]
	}
	if len(source.spans) != 0 {
		text = comments(source.text[:source.spans[0].bodyStart]) + text
	}
	return ParseAuthorScriptVersion(text, source.path, 2)
}

func v1TreatmentDefinitions(used map[string]bool) []AuthorTreatment {
	definitions := []texttemplate.Treatment{
		{Name: "retail", Band: "native:dialogue.band", Body: "native:dialogue.body", Shadow: "native:dialogue.shadow", Shape: "diagonal"},
		{Name: "hud", Band: "native:hud.band", Body: "native:hud.body", Shadow: "native:hud.shadow", Shape: "diagonal"},
		{Name: "hud-frame", Band: "native:hud.body", Body: "native:hud.body", Shadow: "native:hud.shadow", Shape: "diagonal"},
		{Name: "world", Band: "native:world.band", Body: "native:world.body", Shadow: "native:world.shadow", Shape: "diagonal"},
		{Name: "location", Band: "native:location.band", Body: "native:location.body", Shadow: "native:location.shadow", Shape: "diagonal"},
		{Name: "credits", Band: "native:credits.body", Body: "native:credits.body", Shadow: "none", Shape: "diagonal"},
		{Name: "credits-accent", Band: "native:credits.accent", Body: "native:credits.accent", Shadow: "none", Shape: "diagonal"},
	}
	var result []AuthorTreatment
	for _, definition := range definitions {
		if used[definition.Name] {
			result = append(result, AuthorTreatment{Definition: definition})
		}
	}
	return result
}

func inferV1Appearance(message AuthorMessage, profile string) AuthorMessage {
	presentation := authorContracts.routes[message.ID].presentation(profile)
	if presentation.Shape == "inline" {
		return message // Terms inherit their caller's explicit appearance.
	}
	message.Appearance = AuthorTextAppearance{Layout: presentation.Layout, Numerals: "slanted-ascii",
		Style: texttemplate.Style{Font: "body", Treatment: "retail", Scale: 100}}
	switch {
	case strings.Contains(message.ID, ".hud."):
		message.Appearance.Style.Font = "hud"
		message.Appearance.Style.Treatment = "hud"
		if strings.HasSuffix(message.ID, "_value") {
			message.Appearance.Style.Italic = 2
		}
		if presentation.Layout == "framed_label" {
			message.Appearance.Style.Treatment = "hud-frame"
		}
	case strings.HasPrefix(message.ID, "credits."):
		message.Appearance.Style.Treatment = "credits"
		message.Appearance.Numerals = "upright"
		if v1CreditsInitialAccent(profile, message.ID) {
			message.Operations = accentV1CreditInitial(message.Operations)
		}
	case message.ID == "world_map.location_label":
		message.Appearance.Style.Treatment = "world"
		message.Appearance.Numerals = "upright"
	case strings.HasPrefix(message.ID, "city.") && strings.HasSuffix(message.ID, ".name"):
		message.Appearance.Style.Treatment = "location"
	}
	message.Operations = italicizeV1TableValues(message.Operations, presentation.Layout)
	return message
}

// Measured from each supported release's credits tile descriptors. The US
// runtime uses the US column for translations; regional sources retain their
// own appearance for reference playback.
func v1CreditsInitialAccent(profile, id string) bool {
	if id == "credits.page_00" {
		return profile == "fr"
	}
	if profile == "jp" {
		return slices.Contains([]string{"credits.page_01", "credits.page_02", "credits.page_03", "credits.page_04", "credits.page_05", "credits.page_08", "credits.page_11", "credits.page_12", "credits.page_13", "credits.page_14"}, id)
	}
	return slices.Contains([]string{"credits.page_01", "credits.page_02", "credits.page_03", "credits.page_04", "credits.page_05", "credits.page_09", "credits.page_10", "credits.page_12", "credits.page_13", "credits.page_14"}, id)
}

func accentV1CreditInitial(ops []AuthorOperation) []AuthorOperation {
	for i, op := range ops {
		if op.Op != "text" {
			continue
		}
		for at := 0; at < len(op.Value); {
			end, ok := nextGrapheme(op.Value, at)
			if !ok {
				return ops // Parser already verifies UTF-8.
			}
			r, _ := utf8.DecodeRuneInString(op.Value[at:])
			if r != ' ' && r != '\n' && r != '-' && r != '–' && r != '—' {
				out := append([]AuthorOperation{}, ops[:i]...)
				// The original accent is a prefix ending after the first heading
				// grapheme, including its decorative dash. Preserve that whole range.
				for j := range out {
					if out[j].Op == "text" {
						out[j].Style.Treatment = "credits-accent"
					}
				}
				accent := op
				accent.Value, accent.Style.Treatment = op.Value[:end], "credits-accent"
				out = append(out, accent)
				if end != len(op.Value) {
					after := op
					after.Value = op.Value[end:]
					out = append(out, after)
				}
				return append(out, ops[i+1:]...)
			}
			at = end
		}
	}
	return ops
}

func v1ItalicCell(layout string, line, field, fields int) bool {
	return layout == "master_status" && (line == 1 || field%2 == 1) ||
		layout == "sound_test" && field == 1 ||
		layout == "message_speed" && line == 0 && fields == 10 ||
		(layout == "cities_status" || layout == "score_status") &&
			(line <= 1 && fields > 1 && field+1 == fields ||
				line >= 7 && field > 0 && !(layout == "cities_status" && field == 2))
}

func italicizeV1TableValues(ops []AuthorOperation, layout string) []AuthorOperation {
	if !slices.Contains([]string{"master_status", "cities_status", "score_status", "sound_test", "message_speed"}, layout) {
		return ops
	}
	var out, row []AuthorOperation
	line := 0
	started := false
	flush := func() {
		fields := 1
		for _, op := range row {
			if op.Op == "text" {
				fields += strings.Count(op.Value, "|")
				started = started || strings.TrimSpace(op.Value) != ""
			} else if op.Op == "placeholder" {
				started = true
			}
		}
		field := 0
		for _, op := range row {
			if op.Op == "placeholder" {
				if v1ItalicCell(layout, line, field, fields) {
					op.Style.Italic = 2
				}
				out = append(out, op)
			} else if op.Op == "text" {
				for i, part := range strings.Split(op.Value, "|") {
					if i != 0 {
						out = append(out, AuthorOperation{Op: "text", Value: "|"})
						field++
					}
					if part == "" {
						continue
					}
					piece := op
					piece.Value = part
					if v1ItalicCell(layout, line, field, fields) {
						piece.Style.Italic = 2
					}
					out = append(out, piece)
				}
			} else {
				out = append(out, op)
			}
		}
		row = nil
	}
	for _, op := range ops {
		switch op.Op {
		case "line", "paragraph", "page":
			flush()
			out = append(out, op)
			if started {
				line++
			}
			if op.Op == "paragraph" && started {
				line++
			} else if op.Op == "page" {
				line = 0
				started = false
			}
		default:
			row = append(row, op)
		}
	}
	flush()
	return out
}
