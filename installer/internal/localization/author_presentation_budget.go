package localization

import (
	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

const (
	MaximumGameAppearances  = 64
	MaximumGameStyleSpans   = 512
	MaximumGameFontVariants = 32
)

type fontGeometry struct {
	font          string
	scale, italic int
}

func inheritedStyle(base, override texttemplate.Style) texttemplate.Style {
	if override.Font != "" {
		base.Font = override.Font
	}
	if override.Treatment != "" {
		base.Treatment = override.Treatment
	}
	if override.Color != "" {
		base.Color = override.Color
	}
	if override.Scale != 0 {
		base.Scale = override.Scale
	}
	if override.Italic != 0 {
		base.Italic = override.Italic
	}
	return base
}

// These are ActRaiser authoring budgets, deliberately separate from the
// portable parser's larger grammar limits. Count the entire message's spans:
// the game may retain earlier authored pages in one scrolling window.
func validateAuthorPresentationBudgets(w *AuthorWorkspace) error {
	for _, script := range w.scripts {
		if script.version < 2 {
			continue
		}
		for _, original := range script.messages {
			message, err := resolvedAuthorMessage(w, original.ID)
			if err != nil {
				return err
			}
			base := inheritedStyle(texttemplate.Style{Font: "body", Scale: 100, Italic: 1}, message.Appearance.Style)
			baseAppearance := base
			baseAppearance.Scale *= 100
			appearances := map[texttemplate.Style]bool{baseAppearance: true}
			variants := map[fontGeometry]bool{}
			var previous texttemplate.Style
			havePrevious := false
			spans := 0
			for _, op := range message.Operations {
				if op.Op == "page" {
					havePrevious = false
					continue
				}
				if op.Op != "text" && op.Op != "placeholder" {
					if op.Op == "line" || op.Op == "paragraph" || op.Op == "preferred_line" {
						havePrevious = false
					}
					continue
				}
				style := inheritedStyle(base, op.Style)
				inlineScale := op.Style.Scale
				if inlineScale == 0 {
					inlineScale = 100
				}
				style.Scale = base.Scale * inlineScale
				appearances[style] = true
				if !havePrevious || previous != style {
					spans++
					previous = style
					havePrevious = true
				}
				geometry := fontGeometry{style.Font, style.Scale, style.Italic}
				variants[geometry] = true
				// Runtime numeric values may contain ASCII digits even when the sample
				// doesn't. Reserve the slanted variant whenever the template requests it.
				if message.Appearance.Numerals == "slanted-ascii" {
					geometry.italic = 2
					variants[geometry] = true
				}
				if len(appearances) > MaximumGameAppearances {
					return authorError(script.path, original.SourceLine, "%s exceeds the game's %d distinct appearances per message", original.ID, MaximumGameAppearances)
				}
				if spans > MaximumGameStyleSpans {
					return authorError(script.path, original.SourceLine, "%s exceeds the game's %d style spans per message", original.ID, MaximumGameStyleSpans)
				}
				if len(variants) > MaximumGameFontVariants {
					return authorError(script.path, original.SourceLine, "%s exceeds the game's %d font/size/italic combinations per message", original.ID, MaximumGameFontVariants)
				}
			}
		}
	}
	return nil
}
