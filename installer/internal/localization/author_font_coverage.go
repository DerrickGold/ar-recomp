package localization

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"slices"
	"strings"
	"unicode/utf8"
)

const (
	MaxFontCoverageScalars = 65536
	maxCoverageLocations   = 4
	maxCoverageMissing     = 256
)

// FontCoverageSource exposes immutable local bytes through a fresh reader.
// Built-ins have no reader: their resolution belongs to the host, not the pack
// parser. Neither authoring nor the browser interprets TrueType/OpenType data.
type FontCoverageSource struct {
	Reference string
	Size      int64
	Open      func() io.ReadCloser
}

type FontCoverageIdentity struct {
	Reference string `json:"reference"`
	SHA256    string `json:"sha256"`
}

type FontCoverageProbeResult struct {
	Provided []bool
	Fonts    []FontCoverageIdentity
}

type FontCoverageProbe func(context.Context, []FontCoverageSource, []rune) (FontCoverageProbeResult, error)

type FontCoverageLocation struct {
	Source    string `json:"source"`
	MessageID string `json:"messageID"`
	Line      int    `json:"line"`
}

type FontCoverageGap struct {
	FontRole  string                 `json:"fontRole,omitempty"`
	Codepoint string                 `json:"codepoint"`
	Character string                 `json:"character"`
	Locations []FontCoverageLocation `json:"locations"`
}

type FontCoverageReport struct {
	PackageID        string                 `json:"packageID"`
	ContentRevision  string                 `json:"contentRevision"`
	FallbackRevision string                 `json:"fallbackRevision,omitempty"`
	Fonts            []FontCoverageIdentity `json:"fonts"`
	Scalars          int                    `json:"scalars"`
	Complete         bool                   `json:"complete"`
	MissingCount     int                    `json:"missingCount"`
	Missing          []FontCoverageGap      `json:"missing"`
	DynamicValues    []string               `json:"dynamicValues"`
}

// CheckFontCoverage checks literal operations, never comments, identifiers or
// locked controls. Aliases are checked at their definition. Live values remain
// explicit unknowns unless supplied as samples. The runtime probe decides which
// default-ignorables/objects need no standalone glyph; Go does not duplicate that
// Unicode/rendering policy. This certifies scalar coverage, not shaping/layout.
func (p *AuthorPack) CheckFontCoverage(ctx context.Context, probe FontCoverageProbe, samples []string) (FontCoverageReport, error) {
	return p.CheckFontCoverageWithFallback(ctx, probe, nil, samples)
}

// The selected font stack also renders missing routes from the native US
// source. Publication/install adapters supply that source so a translation's
// font cannot silently lose letters needed by its fallback messages.
func (p *AuthorPack) CheckFontCoverageWithFallback(ctx context.Context, probe FontCoverageProbe, fallback *AuthorPack, samples []string) (FontCoverageReport, error) {
	report := FontCoverageReport{Missing: []FontCoverageGap{}, DynamicValues: []string{}}
	if probe == nil {
		return report, fmt.Errorf("font coverage requires the game's font backend")
	}
	coverage := fontCoverageInput{locations: make(map[string]map[rune][]FontCoverageLocation), dynamic: make(map[string]bool)}
	for _, script := range p.workspace.scripts {
		for _, message := range script.messages {
			if err := coverage.message(ctx, message, script.path, nil); err != nil {
				return report, err
			}
		}
	}
	if fallback != nil {
		for _, script := range fallback.workspace.scripts {
			for _, message := range script.messages {
				if _, replaced := p.workspace.messageScript[message.ID]; replaced {
					continue
				}
				resolved, err := resolvedAuthorMessage(fallback.workspace, message.ID)
				if err != nil {
					return report, err
				}
				if err := coverage.message(ctx, resolved, "<native fallback>", p.manifest.fonts.Stacks()); err != nil {
					return report, err
				}
			}
		}
	}
	if len(samples) > 16 {
		return report, fmt.Errorf("at most 16 dynamic text samples")
	}
	for i, sample := range samples {
		if len(sample) > 16384 {
			return report, fmt.Errorf("font sample exceeds 16 KiB")
		}
		for _, role := range p.manifest.fonts.Stacks() {
			if err := coverage.collect(ctx, role.Name, sample, FontCoverageLocation{Source: fmt.Sprintf("<sample:%d>", i+1), Line: 1}); err != nil {
				return report, err
			}
		}
	}
	for name := range coverage.dynamic {
		report.DynamicValues = append(report.DynamicValues, name)
	}
	slices.Sort(report.DynamicValues)
	for _, role := range p.manifest.fonts.Stacks() {
		if err := p.checkFontRole(ctx, probe, role, coverage.locations[role.Name], &report); err != nil {
			return report, err
		}
		delete(coverage.locations, role.Name)
	}
	for role := range coverage.locations {
		return report, fmt.Errorf("undeclared font role %q", role)
	}
	report.PackageID = p.manifest.metadata.ID
	report.ContentRevision = fmt.Sprintf("%016x", p.RuntimeRevision())
	if fallback != nil {
		report.FallbackRevision = fmt.Sprintf("%016x", fallback.RuntimeRevision())
	}
	report.Complete = report.MissingCount == 0
	return report, nil
}

// Coverage belongs to a role, never to the union of every font in the pack.
// Otherwise a glyph in the HUD stack could conceal a missing body glyph.
type fontCoverageInput struct {
	locations   map[string]map[rune][]FontCoverageLocation
	dynamic     map[string]bool
	scalarCount int
}

func (c *fontCoverageInput) collect(ctx context.Context, role, text string, location FontCoverageLocation) error {
	if !utf8.ValidString(text) || strings.ContainsRune(text, 0) {
		return fmt.Errorf("font samples must contain valid non-NUL Unicode")
	}
	if role == "" {
		role = "body"
	}
	if c.locations[role] == nil {
		c.locations[role] = make(map[rune][]FontCoverageLocation)
	}
	locations := c.locations[role]
	for _, scalar := range text {
		found, exists := locations[scalar]
		if !exists {
			if c.scalarCount >= MaxFontCoverageScalars {
				return fmt.Errorf("font coverage exceeds %d role/character pairs; check a smaller pack", MaxFontCoverageScalars)
			}
			c.scalarCount++
		}
		if len(found) < maxCoverageLocations && !slices.Contains(found, location) {
			locations[scalar] = append(found, location)
		}
	}
	return ctx.Err()
}

func (c *fontCoverageInput) message(ctx context.Context, message AuthorMessage, path string, available []PackFontRole) error {
	for _, op := range message.Operations {
		if op.Op == "text" {
			role := message.Appearance.Style.Font
			if op.Style.Font != "" {
				role = op.Style.Font
			}
			if available != nil && role != "" && role != "body" && !slices.ContainsFunc(available, func(font PackFontRole) bool { return font.Name == role }) {
				role = "body"
			}
			if err := c.collect(ctx, role, op.Value, FontCoverageLocation{path, message.ID, op.SourceLine}); err != nil {
				return err
			}
		} else if op.Op == "placeholder" && !strings.HasPrefix(op.Name, "icon.") {
			c.dynamic[op.Name] = true
		}
	}
	return nil
}

func (p *AuthorPack) checkFontRole(ctx context.Context, probe FontCoverageProbe, role PackFontRole, locations map[rune][]FontCoverageLocation, report *FontCoverageReport) error {
	scalars := make([]rune, 0, len(locations))
	for scalar := range locations {
		scalars = append(scalars, scalar)
	}
	slices.Sort(scalars)
	var fonts []FontCoverageSource
	for _, ref := range append([]string{role.Primary}, role.Fallback...) {
		source := FontCoverageSource{Reference: ref}
		if !strings.HasPrefix(ref, "builtin:") {
			data := p.fonts[ref]
			source.Size = int64(len(data))
			source.Open = func() io.ReadCloser { return io.NopCloser(bytes.NewReader(data)) }
		}
		fonts = append(fonts, source)
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	result, err := probe(ctx, fonts, scalars)
	if err != nil {
		return err
	}
	if len(result.Provided) != len(scalars) || len(result.Fonts) != len(fonts) {
		return fmt.Errorf("incomplete font-backend response")
	}
	for i, font := range result.Fonts {
		if font.Reference != fonts[i].Reference || len(font.SHA256) != 64 || strings.Trim(font.SHA256, "0123456789abcdef") != "" {
			return fmt.Errorf("invalid font-backend identity")
		}
		if !slices.Contains(report.Fonts, font) {
			report.Fonts = append(report.Fonts, font)
		}
	}
	report.Scalars += len(scalars)
	for i, covered := range result.Provided {
		if covered {
			continue
		}
		report.MissingCount++
		if len(report.Missing) < maxCoverageMissing {
			scalar := scalars[i]
			report.Missing = append(report.Missing, FontCoverageGap{FontRole: role.Name, Codepoint: fmt.Sprintf("U+%04X", scalar), Character: string(scalar), Locations: locations[scalar]})
		}
	}
	return nil
}
