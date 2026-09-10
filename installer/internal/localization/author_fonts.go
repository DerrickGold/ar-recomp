package localization

import (
	"bytes"
	"fmt"
	"slices"
	"strings"
)

// Archive import and font edits share this envelope check. Actual validity
// and glyph coverage still belong to the game's font backend.
func validAuthorFontPayload(path string, data []byte) bool {
	return (strings.HasSuffix(strings.ToLower(path), ".ttf") || strings.HasSuffix(strings.ToLower(path), ".otf")) && len(data) >= 4 &&
		(bytes.Equal(data[:4], []byte{0, 1, 0, 0}) || string(data[:4]) == "OTTO")
}

// WithFonts replaces only font declarations. Manifest comments, metadata and
// script references remain verbatim; the shared parser validates the result.
func (m *PackManifest) WithFonts(fonts PackFonts) (*PackManifest, error) {
	if _, err := NewPackManifest(m.metadata, fonts, m.sources); err != nil {
		return nil, err
	}
	if fonts.Primary == m.fonts.Primary && slices.Equal(fonts.Fallback, m.fonts.Fallback) {
		return m, nil
	}
	newline := "\n"
	if strings.Contains(m.text, "\r\n") {
		newline = "\r\n"
	} else if strings.Contains(m.text, "\r") && !strings.Contains(m.text, "\n") {
		newline = "\r"
	}
	var out strings.Builder
	position := 0
	for i, row := range m.fontRows {
		out.WriteString(m.text[position:row.start])
		if i == 0 {
			out.WriteString("primary = " + fonts.Primary + newline)
			for _, font := range fonts.Fallback {
				out.WriteString("fallback = " + font + newline)
			}
		}
		position = row.end
	}
	out.WriteString(m.text[position:])
	return ParsePackManifest(out.String(), m.path)
}

// WithFonts is a bounded immutable transaction. Existing font bytes are shared
// with the old snapshot; only newly supplied dependencies are copied. Removing
// a reference removes its payload from subsequent saves/exports, not from the
// old snapshot or the source file on disk. Uploads cannot add undeclared files.
func (p *AuthorPack) WithFonts(fonts PackFonts, uploaded map[string][]byte) (*AuthorPack, error) {
	m, err := p.manifest.WithFonts(fonts)
	if err != nil {
		return nil, err
	}
	if err := validateAuthorPackPaths(m); err != nil {
		return nil, err
	}
	refs := append([]string{fonts.Primary}, fonts.Fallback...)
	for name, data := range uploaded {
		if strings.HasPrefix(name, "builtin:") || !slices.Contains(refs, name) {
			return nil, fmt.Errorf("font upload %q is not a declared local dependency", name)
		}
		if len(data) == 0 || len(data) > MaxPackFontBytes {
			return nil, fmt.Errorf("%s: font must contain 1–%d bytes", name, MaxPackFontBytes)
		}
	}
	retained := make(map[string][]byte)
	for _, name := range refs {
		if strings.HasPrefix(name, "builtin:") {
			continue
		}
		if _, ok := retained[name]; ok {
			continue
		}
		data, supplied := uploaded[name]
		if !supplied {
			data = p.fonts[name]
		}
		if len(data) == 0 {
			return nil, fmt.Errorf("%s: choose a font file before saving this stack", name)
		}
		if !validAuthorFontPayload(name, data) {
			return nil, fmt.Errorf("unsupported font payload %s (use TTF/OTF)", name)
		}
		retained[name] = data
	}
	next := &AuthorPack{manifest: m, workspace: p.workspace, fonts: retained, progressPresent: p.progressPresent}
	if next.byteSize() > MaxAuthorPackBytes {
		return nil, fmt.Errorf("pack exceeds aggregate size limit")
	}
	// Copy after the aggregate check so an oversized request cannot allocate a
	// second pack-sized payload before it is rejected.
	for name, data := range uploaded {
		retained[name] = append([]byte(nil), data...)
	}
	return next, nil
}

func (p *AuthorProject) WithFonts(fonts PackFonts, uploaded map[string][]byte) (*AuthorProject, error) {
	if p.Origin() == "native-source" {
		return nil, fmt.Errorf("native sources are read-only; create a translation to edit fonts")
	}
	pack, err := p.pack.WithFonts(fonts, uploaded)
	if err != nil {
		return nil, err
	}
	next := p.clone()
	next.pack = pack
	return next, nil
}
