package localizationkit

import (
	"fmt"
	"slices"
	"strings"
	"unicode/utf8"
)

const (
	MaxPackManifestBytes = 256 << 10
	MaxPackFontBytes     = 64 << 20
	MaxPackPathBytes     = 511
)

// PackMetadata deliberately separates identity, display name and locale.
// A locale is never an installation key. SourceProfile selects semantic
// contracts, not the language of a community translation.
type PackMetadata struct {
	ID            string `json:"id"`
	Locale        string `json:"locale"`
	Name          string `json:"name"`
	Autonym       string `json:"autonym"`
	Author        string `json:"author"`
	License       string `json:"license"`
	Direction     string `json:"direction"`
	Target        string `json:"target"`
	SourceProfile string `json:"source_profile"`
	Fallback      string `json:"fallback"`
	Coverage      string `json:"coverage"`
	Description   string `json:"description,omitempty"`
}

type PackFonts struct {
	Primary  string   `json:"primary"`
	Fallback []string `json:"fallback"`
}

type manifestValueSpan struct {
	key        string
	start, end int
}

// PackManifest retains comments and physical formatting for metadata edits.
// It checks syntax and portable paths only; LoadAuthorPack verifies referenced
// files and semantic contracts. Font availability/rendering is a later gate.
type PackManifest struct {
	text, path string
	metadata   PackMetadata
	fonts      PackFonts
	sources    []string
	packValues []manifestValueSpan
	packEnd    int
}

func (m *PackManifest) Text() string           { return m.text }
func (m *PackManifest) Metadata() PackMetadata { return m.metadata }
func (m *PackManifest) Fonts() PackFonts {
	return PackFonts{m.fonts.Primary, append([]string{}, m.fonts.Fallback...)}
}
func (m *PackManifest) Sources() []string { return append([]string{}, m.sources...) }

// PortablePackPath is shared by manifest, reader and future archive/install
// adapters. Reject path syntax with platform-specific meanings (including DOS
// devices and NTFS alternate streams), not just traversal on the current OS.
func PortablePackPath(path string) bool {
	if path == "" || len(path) > MaxPackPathBytes || !utf8.ValidString(path) || strings.Trim(path, " \t") != path {
		return false
	}
	for _, c := range path {
		if c < 0x20 || c == 0x7f || strings.ContainsRune(`\<>:"|?*`, c) {
			return false
		}
	}
	for _, part := range strings.Split(path, "/") {
		if part == "" || part == "." || part == ".." || strings.HasSuffix(part, ".") || strings.HasSuffix(part, " ") {
			return false
		}
		base, _, _ := strings.Cut(part, ".")
		base = strings.ToUpper(strings.TrimRight(base, " "))
		if slices.Contains([]string{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}, base) {
			return false
		}
		if (strings.HasPrefix(base, "COM") || strings.HasPrefix(base, "LPT")) && slices.Contains([]string{"1", "2", "3", "4", "5", "6", "7", "8", "9", "¹", "²", "³"}, base[3:]) {
			return false
		}
	}
	return true
}

func packFontReference(path string) bool {
	if strings.HasPrefix(path, "builtin:") {
		return len(path) <= MaxPackPathBytes && authorIdentifier(path[8:])
	}
	return PortablePackPath(path)
}

func packLocale(locale string) bool {
	if len(locale) > 32 {
		return false
	}
	parts := strings.Split(locale, "-")
	if len(parts[0]) < 2 || len(parts[0]) > 8 {
		return false
	}
	for i, part := range parts {
		if len(part) == 0 || len(part) > 8 {
			return false
		}
		for _, c := range []byte(part) {
			if c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' {
				continue
			}
			if i == 0 || c < '0' || c > '9' {
				return false
			}
		}
	}
	return true
}

func metadataValues(m PackMetadata) map[string]string {
	return map[string]string{"id": m.ID, "locale": m.Locale, "name": m.Name, "autonym": m.Autonym,
		"author": m.Author, "license": m.License, "direction": m.Direction, "target": m.Target,
		"source_profile": m.SourceProfile, "fallback": m.Fallback, "coverage": m.Coverage, "description": m.Description}
}

var packMetadataKeys = []string{"id", "locale", "name", "autonym", "author", "license", "direction", "target", "source_profile", "fallback", "coverage", "description"}

func validatePackMetadata(m PackMetadata) error {
	values := metadataValues(m)
	for _, key := range packMetadataKeys {
		value := values[key]
		if key == "description" && value == "" {
			continue
		}
		if len(value) > MaxPackManifestBytes {
			return fmt.Errorf("%s exceeds manifest size limit", key)
		}
		if value == "" || !utf8.ValidString(value) || strings.ContainsAny(value, "\r\n\x00") || strings.Trim(value, " \t") != value {
			return fmt.Errorf("%s must be nonempty, single-line UTF-8 without outer whitespace", key)
		}
		limit := 0
		switch key {
		case "id":
			limit = 96
		case "locale":
			limit = 32
		case "name", "autonym", "author":
			limit = 192
		case "license":
			limit = 128
		}
		if limit != 0 && len(value) > limit {
			return fmt.Errorf("%s is too long", key)
		}
	}
	if !authorIdentifier(m.ID) {
		return fmt.Errorf("invalid package id %q", m.ID)
	}
	if !packLocale(m.Locale) {
		return fmt.Errorf("invalid BCP-47 locale %q", m.Locale)
	}
	if !slices.Contains([]string{"ltr", "rtl", "auto"}, m.Direction) {
		return fmt.Errorf("direction must be ltr, rtl, or auto")
	}
	if !slices.Contains([]string{"us-runtime", "reference-only"}, m.Target) {
		return fmt.Errorf("target must be us-runtime or reference-only")
	}
	if !authorProfile(m.SourceProfile) {
		return fmt.Errorf("unsupported source_profile %q", m.SourceProfile)
	}
	if m.Target == "us-runtime" && m.SourceProfile != "us" {
		return fmt.Errorf("us-runtime packs must use the U.S. semantic contract")
	}
	if m.Fallback != "native-us" {
		return fmt.Errorf("v1 fallback must be native-us")
	}
	if m.Coverage != "partial" && m.Coverage != "complete" {
		return fmt.Errorf("coverage must be partial or complete")
	}
	return nil
}

func ParsePackManifest(text, path string) (*PackManifest, error) {
	if len(text) > MaxPackManifestBytes {
		return nil, authorError(path, 0, "manifest exceeds size limit")
	}
	if !utf8.ValidString(text) || strings.IndexByte(text, 0) >= 0 {
		return nil, authorError(path, 0, "manifest is not valid UTF-8 text")
	}
	m := &PackManifest{text: text, path: path, fonts: PackFonts{Fallback: []string{}}, packEnd: len(text)}
	sections, seen := make(map[string]bool), make(map[string]bool)
	values := make(map[string]string)
	section := ""
	err := authorLines(text, func(physical string, number, start, next int) error {
		if number == 1 && strings.HasPrefix(physical, "\ufeff") {
			physical = physical[3:]
			start += 3
		}
		line := strings.Trim(physical, " \t")
		fail := func(format string, args ...any) error { return authorError(path, number, format, args...) }
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			return nil
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			name := strings.Trim(line[1:len(line)-1], " \t")
			if !slices.Contains([]string{"pack", "fonts", "scripts"}, name) {
				return fail("unknown manifest section [%s]", name)
			}
			if sections[name] {
				return fail("duplicate manifest section [%s]", name)
			}
			if section == "pack" {
				m.packEnd = start
			}
			section, sections[name] = name, true
			return nil
		}
		if section == "" {
			return fail("key appears before a section")
		}
		key, value, found := strings.Cut(line, "=")
		key, value = strings.Trim(key, " \t"), strings.Trim(value, " \t")
		if !found || key == "" || value == "" {
			return fail("expected nonempty key = value")
		}
		repeated := section == "fonts" && key == "fallback" || section == "scripts" && key == "source"
		if seen[section+"/"+key] && !repeated {
			return fail("duplicate key %q", key)
		}
		seen[section+"/"+key] = true
		switch section {
		case "pack":
			if key != "format" && key != "version" && !slices.Contains(packMetadataKeys, key) {
				return fail("unknown key %q in [pack]", key)
			}
			values[key] = value
			equals := strings.IndexByte(physical, '=')
			valueStart := equals + 1 + len(physical[equals+1:]) - len(strings.TrimLeft(physical[equals+1:], " \t"))
			m.packValues = append(m.packValues, manifestValueSpan{key, start + valueStart, start + valueStart + len(value)})
		case "fonts":
			if key != "primary" && key != "fallback" {
				return fail("unknown key %q in [fonts]", key)
			}
			if !packFontReference(value) {
				return fail("font reference must be builtin or a portable relative path")
			}
			if key == "primary" {
				m.fonts.Primary = value
			} else {
				if len(m.fonts.Fallback) >= 8 {
					return fail("too many fallback fonts")
				}
				if slices.Contains(m.fonts.Fallback, value) {
					return fail("duplicate fallback font %q", value)
				}
				m.fonts.Fallback = append(m.fonts.Fallback, value)
			}
		case "scripts":
			if key != "source" {
				return fail("unknown key %q in [scripts]", key)
			}
			if !PortablePackPath(value) || !strings.HasSuffix(value, ".artext") {
				return fail("script source must be a portable relative .artext path")
			}
			if len(m.sources) >= 64 {
				return fail("too many script sources")
			}
			if slices.Contains(m.sources, value) {
				return fail("duplicate script source %q", value)
			}
			m.sources = append(m.sources, value)
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	if len(sections) != 3 {
		return nil, authorError(path, 0, "missing required manifest section")
	}
	if values["format"] != "actraiser-language-pack" || values["version"] != "1" {
		return nil, authorError(path, 0, "unsupported language-pack format or version")
	}
	m.metadata = PackMetadata{ID: values["id"], Locale: values["locale"], Name: values["name"], Autonym: values["autonym"], Author: values["author"], License: values["license"], Direction: values["direction"], Target: values["target"], SourceProfile: values["source_profile"], Fallback: values["fallback"], Coverage: values["coverage"], Description: values["description"]}
	if err := validatePackMetadata(m.metadata); err != nil {
		return nil, authorError(path, 0, "%s", err)
	}
	if m.fonts.Primary == "" || len(m.sources) == 0 {
		return nil, authorError(path, 0, "missing primary font or script sources")
	}
	return m, nil
}

// NewPackManifest emits a deterministic v1 manifest and uses the same parser
// as the editor and reader. User values cannot inject keys, sections or paths.
func NewPackManifest(metadata PackMetadata, fonts PackFonts, sources []string) (*PackManifest, error) {
	if err := validatePackMetadata(metadata); err != nil {
		return nil, err
	}
	if !packFontReference(fonts.Primary) {
		return nil, fmt.Errorf("invalid primary font")
	}
	if len(fonts.Fallback) > 8 || len(sources) == 0 || len(sources) > 64 {
		return nil, fmt.Errorf("too many fonts or invalid source count")
	}
	for _, font := range fonts.Fallback {
		if !packFontReference(font) {
			return nil, fmt.Errorf("invalid fallback font")
		}
	}
	for _, path := range sources {
		if !PortablePackPath(path) || !strings.HasSuffix(path, ".artext") {
			return nil, fmt.Errorf("invalid script path")
		}
	}
	var text strings.Builder
	text.WriteString("[pack]\nformat = actraiser-language-pack\nversion = 1\n")
	values := metadataValues(metadata)
	for _, key := range packMetadataKeys {
		if values[key] != "" {
			fmt.Fprintf(&text, "%s = %s\n", key, values[key])
		}
	}
	fmt.Fprintf(&text, "\n[fonts]\nprimary = %s\n", fonts.Primary)
	for _, font := range fonts.Fallback {
		fmt.Fprintf(&text, "fallback = %s\n", font)
	}
	text.WriteString("\n[scripts]\n")
	for _, path := range sources {
		fmt.Fprintf(&text, "source = %s\n", path)
	}
	return ParsePackManifest(text.String(), "pack.ini")
}

// WithMetadata changes value spans only, preserving unrelated keys/comments
// and all script/font references. Empty optional descriptions remove that row.
func (m *PackManifest) WithMetadata(metadata PackMetadata) (*PackManifest, error) {
	if err := validatePackMetadata(metadata); err != nil {
		return nil, err
	}
	values := metadataValues(metadata)
	text := m.text
	if !slices.ContainsFunc(m.packValues, func(span manifestValueSpan) bool { return span.key == "description" }) && metadata.Description != "" {
		addition := "description = " + metadata.Description + "\n"
		if m.packEnd > 0 && !strings.ContainsAny(text[m.packEnd-1:m.packEnd], "\r\n") {
			addition = "\n" + addition
		}
		text = text[:m.packEnd] + addition + text[m.packEnd:]
	}
	for i := len(m.packValues) - 1; i >= 0; i-- {
		span := m.packValues[i]
		value, ok := values[span.key]
		if !ok {
			continue
		}
		if span.key == "description" && value == "" {
			start := strings.LastIndexAny(text[:span.start], "\r\n") + 1
			end := span.end
			for end < len(text) && text[end] != '\r' && text[end] != '\n' {
				end++
			}
			if end < len(text) {
				first := text[end]
				end++
				if first == '\r' && end < len(text) && text[end] == '\n' {
					end++
				}
			}
			text = text[:start] + text[end:]
		} else {
			text = text[:span.start] + value + text[span.end:]
		}
	}
	return ParsePackManifest(text, m.path)
}
