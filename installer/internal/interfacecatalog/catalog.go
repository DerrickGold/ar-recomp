// Package interfacecatalog owns built-in host-interface messages, not game scripts.
// The same validated catalog is embedded in the builder and generated as static
// C for the game overlay. It has no ROM, language-pack or settings dependencies.
package interfacecatalog

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"regexp"
	"slices"
	"strings"
	"unicode/utf8"
)

//go:generate go run ./cmd/generate -output ../../../src/localization/ui_catalog_data.inc

//go:embed messages.json
var source []byte

// Columns are stable and agree with ArUiLocale; en includes European English.
var locales = []string{"en", "fr", "de", "ja"}

type Entry struct {
	Key  string   `json:"key"`
	Text []string `json:"text"`
}

var identifier = regexp.MustCompile(`^[a-z][a-z0-9_.]*$`)
var argument = regexp.MustCompile(`^[a-z_][a-z0-9_]{0,63}$`)

func Locales() []string { return slices.Clone(locales) }

func NormalizeLocale(tag string) string {
	if i := strings.IndexAny(tag, "-_"); i >= 0 {
		tag = tag[:i]
	}
	tag = strings.ToLower(tag)
	if slices.Contains(locales, tag) {
		return tag
	}
	return "en"
}

// Arguments checks the deliberately small, shared named-substitution grammar.
func Arguments(text string) ([]string, error) {
	var names []string
	for len(text) > 0 {
		i := strings.IndexAny(text, "{}")
		if i < 0 {
			break
		}
		if text[i] != '{' {
			return nil, fmt.Errorf("unmatched closing brace")
		}
		text = text[i+1:]
		end := strings.IndexByte(text, '}')
		if end < 0 || !argument.MatchString(text[:end]) {
			return nil, fmt.Errorf("invalid named argument")
		}
		names = append(names, text[:end])
		text = text[end+1:]
	}
	slices.Sort(names)
	names = slices.Compact(names)
	if len(names) > 16 {
		return nil, fmt.Errorf("too many named arguments")
	}
	return names, nil
}

func Decode(data []byte) ([]Entry, error) {
	if !utf8.Valid(data) {
		return nil, fmt.Errorf("catalog is not UTF-8")
	}
	var entries []Entry
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err := d.Decode(&entries); err != nil {
		return nil, err
	}
	if len(entries) == 0 {
		return nil, fmt.Errorf("empty catalog")
	}
	var trailing any
	if err := d.Decode(&trailing); err != io.EOF {
		return nil, fmt.Errorf("trailing catalog data")
	}
	seen := make(map[string]bool)
	for _, entry := range entries {
		if !identifier.MatchString(entry.Key) || len(entry.Key) > 160 || seen[entry.Key] {
			return nil, fmt.Errorf("invalid/duplicate catalog key %q", entry.Key)
		}
		seen[entry.Key] = true
		if len(entry.Text) != len(locales) {
			return nil, fmt.Errorf("%s needs all four languages", entry.Key)
		}
		var expected []string
		for i, text := range entry.Text {
			if len(text) == 0 || len(text) >= 4096 || !utf8.ValidString(text) || strings.ContainsFunc(text, func(r rune) bool { return (r < 32 && r != '\n') || r == 127 }) {
				return nil, fmt.Errorf("%s/%s has invalid text", entry.Key, locales[i])
			}
			names, err := Arguments(text)
			if err != nil {
				return nil, fmt.Errorf("%s/%s: %w", entry.Key, locales[i], err)
			}
			if i == 0 {
				expected = names
			} else if !slices.Equal(expected, names) {
				return nil, fmt.Errorf("%s/%s changes named arguments", entry.Key, locales[i])
			}
		}
	}
	slices.SortFunc(entries, func(a, b Entry) int { return strings.Compare(a.Key, b.Key) })
	return entries, nil
}

func Entries() ([]Entry, error) { return Decode(source) }
