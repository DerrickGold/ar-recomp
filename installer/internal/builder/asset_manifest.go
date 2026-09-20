package builder

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

type manifestValue struct {
	Key   string
	Value string
}

type manifestSection struct {
	Name       string
	Start, End int
}

const maxAssetManifestBytes = 4 << 20

func liveAssetManifestPath(root string) string {
	return filepath.Join(root, "game-assets", "manifest.ini")
}

func defaultAssetManifestPath(root string) string {
	return filepath.Join(root, "defaults", "game-assets", "manifest.ini")
}

// readAssetManifest reads the live file when it exists. A bundle has only a
// shipped default before its first game launch, so that copy becomes the seed
// for the first save. It is never edited in place.
func readAssetManifest(root string) (string, string, error) {
	live := liveAssetManifestPath(root)
	content, absent, err := readBoundedRegularFile(live, maxAssetManifestBytes)
	if err != nil {
		return "", live, fmt.Errorf("read asset manifest: %w", err)
	}
	if !absent {
		return string(content), live, nil
	}
	defaults := defaultAssetManifestPath(root)
	content, absent, err = readBoundedRegularFile(defaults, maxAssetManifestBytes)
	if err != nil {
		return "", live, fmt.Errorf("read default asset manifest: %w", err)
	}
	if absent {
		return "# Asset replacement manifest (managed by actraiser-builder).\n", live, nil
	}
	return string(content), live, nil
}

func readBoundedRegularFile(path string, limit int64) ([]byte, bool, error) {
	info, err := os.Stat(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, true, nil
	}
	if err != nil {
		return nil, false, err
	}
	if !info.Mode().IsRegular() {
		return nil, false, fmt.Errorf("%s is not a regular file", path)
	}
	if info.Size() > limit {
		return nil, false, fmt.Errorf("%s exceeds the %d MiB safety limit",
			path, limit>>20)
	}
	content, err := os.ReadFile(path)
	return content, false, err
}

func manifestSections(text string) []manifestSection {
	var sections []manifestSection
	for offset := 0; offset < len(text); {
		lineEnd := strings.IndexByte(text[offset:], '\n')
		if lineEnd < 0 {
			lineEnd = len(text)
		} else {
			lineEnd += offset + 1
		}
		line := strings.TrimSpace(text[offset:lineEnd])
		if strings.HasPrefix(line, "[") {
			if close := strings.IndexByte(line, ']'); close > 1 {
				if len(sections) > 0 {
					sections[len(sections)-1].End = offset
				}
				sections = append(sections, manifestSection{
					Name: strings.TrimSpace(line[1:close]), Start: offset, End: len(text),
				})
			}
		}
		offset = lineEnd
	}
	return sections
}

func findManifestSection(text, name string) (manifestSection, bool) {
	for _, section := range manifestSections(text) {
		if section.Name == name {
			return section, true
		}
	}
	return manifestSection{}, false
}

func manifestSectionValue(text, sectionName, key string) (string, bool) {
	section, ok := findManifestSection(text, sectionName)
	if !ok {
		return "", false
	}
	var value string
	found := false
	for _, line := range strings.Split(text[section.Start:section.End], "\n") {
		line = strings.TrimSpace(strings.TrimSuffix(line, "\r"))
		if line == "" || strings.HasPrefix(line, "#") ||
			strings.HasPrefix(line, ";") || strings.HasPrefix(line, "[") {
			continue
		}
		candidateKey, candidateValue, hasEquals := strings.Cut(line, "=")
		if hasEquals && strings.TrimSpace(candidateKey) == key {
			value = strings.TrimSpace(candidateValue)
			found = true
		}
	}
	return value, found
}

func manifestNewline(text string) string {
	if strings.Contains(text, "\r\n") {
		return "\r\n"
	}
	return "\n"
}

func updateManifestSectionBlock(block string, values []manifestValue, newline string) string {
	seen := make(map[string]bool, len(values))
	lines := strings.SplitAfter(block, "\n")
	var output strings.Builder
	for _, line := range lines {
		withoutEnding := strings.TrimSuffix(line, "\n")
		withoutEnding = strings.TrimSuffix(withoutEnding, "\r")
		trimmed := strings.TrimSpace(withoutEnding)
		key, _, hasEquals := strings.Cut(trimmed, "=")
		matched := false
		if hasEquals && !strings.HasPrefix(trimmed, "#") &&
			!strings.HasPrefix(trimmed, ";") {
			key = strings.TrimSpace(key)
			for _, value := range values {
				if key == value.Key {
					indent := withoutEnding[:len(withoutEnding)-len(strings.TrimLeft(withoutEnding, " \t"))]
					ending := ""
					if strings.HasSuffix(line, "\r\n") {
						ending = "\r\n"
					} else if strings.HasSuffix(line, "\n") {
						ending = "\n"
					}
					output.WriteString(indent + value.Key + " = " + value.Value + ending)
					seen[value.Key] = true
					matched = true
					break
				}
			}
		}
		if !matched {
			output.WriteString(line)
		}
	}
	for _, value := range values {
		if seen[value.Key] {
			continue
		}
		if output.Len() > 0 && !strings.HasSuffix(output.String(), "\n") {
			output.WriteString(newline)
		}
		output.WriteString(value.Key + " = " + value.Value + newline)
	}
	return output.String()
}

// upsertManifestSection replaces every duplicate of a managed record with one
// updated copy. The first copy's comments and optional tuning keys survive;
// unknown sections and all text outside the record are byte-for-byte preserved.
func upsertManifestSection(text, name string, values []manifestValue) string {
	newline := manifestNewline(text)
	sections := manifestSections(text)
	var matches []manifestSection
	for _, section := range sections {
		if section.Name == name {
			matches = append(matches, section)
		}
	}
	if len(matches) == 0 {
		if text != "" && !strings.HasSuffix(text, "\n") {
			text += newline
		}
		if text != "" && !strings.HasSuffix(text, newline+newline) {
			text += newline
		}
		var block strings.Builder
		block.WriteString("[" + name + "]" + newline)
		for _, value := range values {
			block.WriteString(value.Key + " = " + value.Value + newline)
		}
		return text + block.String()
	}

	updated := updateManifestSectionBlock(
		text[matches[0].Start:matches[0].End], values, newline)
	var output strings.Builder
	cursor := 0
	for index, match := range matches {
		output.WriteString(text[cursor:match.Start])
		if index == 0 {
			output.WriteString(updated)
		}
		cursor = match.End
	}
	output.WriteString(text[cursor:])
	return output.String()
}

// removeManifestSection deletes every copy of a managed record, which is how a
// music slot returns to the original ROM audio: the game falls back to the ROM
// when no [music:<id>] record names a file. Text outside the record -- other
// sections, comments, hand-authored tuning -- is preserved byte for byte, and a
// name that is not present is a no-op.
func removeManifestSection(text, name string) string {
	var output strings.Builder
	cursor := 0
	removed := false
	for _, section := range manifestSections(text) {
		if section.Name != name {
			continue
		}
		output.WriteString(text[cursor:section.Start])
		cursor = section.End
		removed = true
	}
	if !removed {
		return text
	}
	output.WriteString(text[cursor:])
	return output.String()
}

func resolveManifestFile(manifestPath, value string) string {
	if filepath.IsAbs(value) {
		return filepath.Clean(value)
	}
	return filepath.Clean(filepath.Join(filepath.Dir(manifestPath), filepath.FromSlash(value)))
}

func regularFileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

// A manifest section name becomes a form field name, a DOM attribute and part
// of an HTTP route. Restricting the charset keeps all three well-formed; a
// record named anything else is left in the manifest untouched and simply is
// not offered here, which is safer than rendering it.
var safeManifestTrackID = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`)

// manifestAudioTarget resolves a manifest music value to a path the builder is
// willing to READ OR WRITE, which means one inside game-assets. A manifest is
// hand-editable and its value may be absolute or climb out with "..", and
// neither a token-guarded loopback GET nor a file install is somewhere to turn
// an arbitrary path loose. Existence is deliberately NOT checked: the path a
// record names before its file arrives is exactly what an install needs.
func manifestAudioTarget(root, manifestPath, value string) (string, bool) {
	if value == "" {
		return "", false
	}
	assets, err := filepath.Abs(filepath.Join(root, "game-assets"))
	if err != nil {
		return "", false
	}
	path, err := filepath.Abs(resolveManifestFile(manifestPath, value))
	if err != nil {
		return "", false
	}
	relative, err := filepath.Rel(assets, path)
	if err != nil || relative == ".." ||
		strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
		return "", false
	}
	return path, true
}

func installedAudioPath(root, manifestPath, value string) (string, bool) {
	path, ok := manifestAudioTarget(root, manifestPath, value)
	if !ok || !regularFileExists(path) {
		return "", false
	}
	return path, true
}

// templateAudioPaths is the file path the shipped template declares for each
// slot, so a record a user deleted can be recreated naming the same friendly
// path it originally had rather than a second invented convention.
var templateAudioPaths = func() map[string]string {
	paths := make(map[string]string, len(assetTracks))
	for _, track := range assetTracks {
		paths[track.ID] = track.DefaultFile
	}
	return paths
}()

// slotAudioTarget resolves where a slot's replacement belongs: the path its
// record names, the template's path when the record is gone, and a last-resort
// name derived from the slot id. Returns the manifest-relative value and the
// absolute path, or false when the record points somewhere this builder will
// not write.
func slotAudioTarget(root, manifest, manifestPath string,
	track assetTrack) (string, string, bool) {
	relative, found := manifestSectionValue(manifest, "music:"+track.ID, "file")
	if !found || strings.TrimSpace(relative) == "" {
		relative = templateAudioPaths[track.ID]
	}
	if relative == "" {
		relative = "audio/" + track.ID + ".ogg"
	}
	absolute, ok := manifestAudioTarget(root, manifestPath, relative)
	if !ok {
		return "", "", false
	}
	return relative, absolute, true
}

func titleManifestValues(relative string) ([]manifestValue, []manifestValue) {
	logo := []manifestValue{
		{Key: "plane", Value: "screen"},
		{Key: "layer", Value: "bg1"},
		{Key: "rect", Value: "11,27,248,122"},
		{Key: "image", Value: relative},
		{Key: "when", Value: "wram[0018]==0x00, wram[0019]==0x00, mode==7, m7==identity"},
	}
	swirl := []manifestValue{
		{Key: "plane", Value: "mode7"},
		{Key: "canvas_rect", Value: "139,156,376,251"},
		{Key: "image", Value: relative},
		{Key: "when", Value: "wram[0018]==0x00, wram[0019]==0x00, mode==7, m7!=identity"},
	}
	return logo, swirl
}
