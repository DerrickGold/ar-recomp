package buildgui

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"html"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
)

const (
	maxAssetManifestBytes = 4 << 20
	maxTrackBytes         = 256 << 20
	maxAssetRequestBytes  = 1 << 30
)

type assetTrack struct {
	ID            string
	Name          string
	Src           string
	PreviewSource uint32
	PreviewSong   byte
	Loop          *bool
	// Regions are the action map groups ($18) whose maps DECLARE this song,
	// read straight out of the ROM by `tools/act_content.py --songs`: the
	// per-map asset script at $05:8000 carries a song-change command per map
	// whose pointer is the same SPC image source a [music:] entry names, so
	// the level/song map is a static fact rather than something to observe by
	// playing every stage.
	//
	// EMPTY MEANS NO SPLIT IS OFFERED, which covers two cases that both come
	// out the same way: a song only the non-action group ($18=$00 -- title,
	// world map, sim) uses, and one no map script declares at all because it
	// is started from an event site instead. Offering regions for either would
	// be inviting a gate that can never fire.
	Regions []trackRegion
}

// trackRegion is one region a song plays in, and which of that region's two
// acts it covers. The gate the builder writes is region-level either way --
// $19 advances within an act, so an act-level gate would stop matching a room
// later -- but the act is what a player recognises, so it belongs in the label.
type trackRegion struct {
	Group byte
	Acts  byte // bit0 = act 1, bit1 = act 2; 0 = the region has no acts
}

const (
	actOne = 1 << 0
	actTwo = 1 << 1
)

// actSuffix names the acts a song covers within one region.
func actSuffix(acts byte) string {
	switch acts {
	case actOne:
		return " \u2014 Act 1"
	case actTwo:
		return " \u2014 Act 2"
	case actOne | actTwo:
		return " \u2014 Acts 1 & 2"
	}
	return ""
}

// splitRegions is the region set offered for one song: the regions the ROM says
// actually play it, and nothing else.
//
// A song confined to ONE region is not offered at all. Splitting it could only
// produce a gated record covering every place it plays, which is what the
// slot's own ungated entry already does -- the same audio, reached through an
// extra manifest record and an extra file, with a condition that can never
// distinguish anything. Fillmore's theme, both Northwall themes and Death
// Heim's are the cases: one region each.
//
// The save path reads this too, so a crafted request cannot create a split the
// panel would not offer.
func splitRegions(track assetTrack) []assetRegion {
	if len(track.Regions) < 2 {
		return nil
	}
	var offered []assetRegion
	for _, region := range assetRegions {
		for _, played := range track.Regions {
			if region.Group != played.Group {
				continue
			}
			region.Label += actSuffix(played.Acts)
			offered = append(offered, region)
			break
		}
	}
	return offered
}

func boolPointer(value bool) *bool { return &value }

// The complete 17-entry song-image table. Slot 07 is the separately named
// title-theme record; an entry whose musical identity is not established is
// listed as "Track NN", after its song-table slot. That lets players
// render/listen to every image and map it back to [music:song-NN] without the
// UI inventing a title -- and without labelling a third of the list
// "Unidentified", which described the project's knowledge rather than the
// track and read as though something were missing.
//
// A name here must survive the ROM. `tools/act_content.py --songs` reads which
// maps declare each song, and two former names claimed a PLACE the game
// contradicts: song-02 ("Bloodpool") also carries Kasandora's act 1, and
// song-09 ("Act 2 Theme") is act 1 in Marahna. Both are back to their slot
// numbers rather than pointing a player at the wrong level. Names that describe
// the MUSIC rather than a location -- "Advent", "Birth of the People",
// "Sacrifices", "Level Up" -- make no claim the map table can refute, and
// song-00 ("Fillmore") stays because the ROM confirms it exactly: Fillmore act
// 1, nowhere else.
//
// song-16 was "Kasandora", which the map table refutes -- it is declared in all
// six towns and the Temple, never in the Kasandora ACT region. It is
// "Sacrifices", a town variant theme, which the same table corroborates: the
// towns each declare four songs at slots $00-$03, and this one is slot $01
// beside "Birth of the People" at slot $00.
var assetTracks = []assetTrack{
	{ID: "title-theme", Name: "Title Theme", Src: "1A:94B8", PreviewSource: 0x1a94b8, PreviewSong: 1},
	{ID: "song-00", Name: "Fillmore", Src: "18:947F", PreviewSource: 0x18947f, PreviewSong: 1, Regions: []trackRegion{{0x01, actOne}}},
	{ID: "song-01", Name: "Sky Palace", Src: "1C:A988", PreviewSource: 0x1ca988, PreviewSong: 1},
	{ID: "song-02", Name: "Track 02", Src: "18:DDCC", PreviewSource: 0x18ddcc, PreviewSong: 1, Regions: []trackRegion{{0x02, actOne | actTwo}, {0x03, actOne}}},
	{ID: "song-03", Name: "Track 03", Src: "1A:E9E2", PreviewSource: 0x1ae9e2, PreviewSong: 1, Regions: []trackRegion{{0x04, actOne}, {0x05, actOne}, {0x06, actOne}}},
	{ID: "song-04", Name: "Track 04", Src: "1C:A5FB", PreviewSource: 0x1ca5fb, PreviewSong: 1},
	{ID: "song-05", Name: "Track 05", Src: "1B:8554", PreviewSource: 0x1b8554, PreviewSong: 1, Regions: []trackRegion{{0x04, actOne | actTwo}, {0x05, actTwo}}},
	{ID: "song-06", Name: "Track 06", Src: "1B:9470", PreviewSource: 0x1b9470, PreviewSong: 1, Regions: []trackRegion{{0x01, actTwo}, {0x02, actTwo}, {0x03, actTwo}, {0x04, actTwo}, {0x05, actTwo}, {0x06, actTwo}, {0x07, 0}}},
	{ID: "song-08", Name: "Advent", Src: "1C:A7CC", PreviewSource: 0x1ca7cc, PreviewSong: 1, Loop: boolPointer(false)},
	{ID: "song-09", Name: "Track 09", Src: "0E:F69F", PreviewSource: 0x0ef69f, PreviewSong: 1, Regions: []trackRegion{{0x01, actTwo}, {0x03, actTwo}, {0x05, actOne}}},
	{ID: "song-10", Name: "Birth of the People", Src: "1B:ABED", PreviewSource: 0x1babed, PreviewSong: 1},
	{ID: "song-11", Name: "Level Up", Src: "1C:AFEB", PreviewSource: 0x1cafeb, PreviewSong: 1},
	{ID: "song-12", Name: "Track 12", Src: "19:FA4B", PreviewSource: 0x19fa4b, PreviewSong: 1, Regions: []trackRegion{{0x06, actOne}}},
	{ID: "song-13", Name: "Track 13", Src: "17:C027", PreviewSource: 0x17c027, PreviewSong: 1, Regions: []trackRegion{{0x06, actTwo}}},
	{ID: "song-14", Name: "Track 14", Src: "18:D4FA", PreviewSource: 0x18d4fa, PreviewSong: 1, Regions: []trackRegion{{0x07, 0}}},
	{ID: "song-15", Name: "Track 15", Src: "1C:9F3D", PreviewSource: 0x1c9f3d, PreviewSong: 1},
	{ID: "song-16", Name: "Sacrifices", Src: "1A:EF63", PreviewSource: 0x1aef63, PreviewSong: 1},
}

// assetRegion is one action region, keyed by the map-group byte the game keeps
// at WRAM $18. Mirrors ActRaiserMapGroup in src/actraiser_game.h -- the game is
// the authority; this table exists so the builder can WRITE a gate rather than
// asking a player to look the byte up and type it.
//
// The region byte is the only level identity worth gating a song on. $19 (the
// room) advances WITHIN an act, and a gate is sampled once when the song
// starts, so a room-level gate stops matching the moment the song restarts a
// room later; $18 holds for a whole act. That is also why the split is offered
// per region and not per act: a region's act 1 and act 2 have different songs
// anyway, so the region byte already separates them.
type assetRegion struct {
	Slug  string
	Label string
	Group byte
}

var assetRegions = []assetRegion{
	{Slug: "fillmore", Label: "Fillmore", Group: 0x01},
	{Slug: "bloodpool", Label: "Bloodpool", Group: 0x02},
	{Slug: "kasandora", Label: "Kasandora", Group: 0x03},
	{Slug: "aitos", Label: "Aitos", Group: 0x04},
	{Slug: "marahna", Label: "Marahna", Group: 0x05},
	{Slug: "northwall", Label: "Northwall", Group: 0x06},
	{Slug: "death-heim", Label: "Death Heim", Group: 0x07},
}

// splitSectionName is the namespace the builder owns. A record under this exact
// name is one the split action created and may remove again; anything else in
// the manifest is hand-authored and is only ever read.
func splitSectionName(trackID, slug string) string {
	return trackID + "-" + slug
}

func splitStubFile(trackID, slug string) string {
	return "audio/" + splitSectionName(trackID, slug) + ".ogg"
}

func splitGate(group byte) string {
	return fmt.Sprintf("wram[%04X]==0x%02X", 0x0018, group)
}

// assetSplitStatus is one region offered for a slot, and whether that slot
// currently has a record for it.
type assetSplitStatus struct {
	Slug    string `json:"slug"`
	Label   string `json:"label"`
	Enabled bool   `json:"enabled"`
	// Name, Gate and File are exactly what a save WILL write for this region.
	// Sent up front so the page can show the row the moment the box is ticked,
	// instead of making the reader save once to reveal the picker and again to
	// fill it -- and so the row it shows is the record that will exist, not the
	// page's own guess at one.
	Name string `json:"name"`
	Gate string `json:"gate"`
	File string `json:"file"`
}

type assetTitleStatus struct {
	Enabled bool   `json:"enabled"`
	File    string `json:"file,omitempty"`
}

type assetTrackStatus struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Src        string `json:"src"`
	Configured bool   `json:"configured"`
	File       string `json:"file,omitempty"`
	// URL plays the replacement that is ALREADY installed. Without it a
	// replacement could only be heard in the session that uploaded it: the
	// row's player is fed by URL.createObjectURL on the file the reader just
	// picked, so re-opening the builder left the audio unplayable and the only
	// way to hear what was installed was to upload it again.
	URL string `json:"url,omitempty"`
	// Variants are the OTHER manifest records that name this same ROM song --
	// hand-authored gated alternates like a per-region act theme. They are
	// listed under the slot they belong to because that is what they are: the
	// song is identified by src, and the section name is only a label.
	Variants []assetVariantStatus `json:"variants,omitempty"`
	// Splits is the per-region offer: which levels this song can be given its
	// own track in, and which already have one.
	Splits []assetSplitStatus `json:"splits,omitempty"`
}

// assetVariantStatus is a gated alternate for one slot. The builder supplies
// its FILE and nothing else: `when` gates are hand-authored, an entry is
// dropped outright if it loses its `src` or `file`, and rewriting a record
// somebody wrote by hand to express a condition is not the builder's business.
// So a variant is managed exactly the way the manifest header tells a user to
// manage one by hand -- by putting a file at the path the record already names.
type assetVariantStatus struct {
	Name       string `json:"name"`
	When       string `json:"when,omitempty"`
	File       string `json:"file"`
	Configured bool   `json:"configured"`
	URL        string `json:"url,omitempty"`
}

type assetConfiguration struct {
	Title        assetTitleStatus   `json:"title"`
	Tracks       []assetTrackStatus `json:"tracks"`
	ManifestPath string             `json:"manifestPath"`
}

type manifestValue struct {
	Key   string
	Value string
}

type manifestSection struct {
	Name       string
	Start, End int
}

type stagedAsset struct {
	Temporary string
	Final     string
	Relative  string
}

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
		return "# Asset replacement manifest (managed by snesbuild).\n", live, nil
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

// installedAudioURL is versioned by the file itself for the same reason the ROM
// previews are: saving a new replacement reuses the track id, so a URL keyed
// only on that id would let a browser serve the previous file's cached bytes.
func installedAudioURL(root, manifestPath, id, value string) string {
	path, ok := installedAudioPath(root, manifestPath, value)
	if !ok {
		return ""
	}
	return "asset-audio/" + id + "?v=" + previewVersionToken(path)
}

// managedTrackID reports whether a name is one the builder itself manages: a
// song-table slot, or a split it can write under <slot>-<region>. Everything
// the builder will not edit it also will not serve -- one answer to "is this
// mine", used by the rows, the save path and the playback route alike.
func managedTrackID(id string) bool {
	for _, track := range assetTracks {
		if track.ID == id {
			return true
		}
		for _, region := range splitRegions(track) {
			if splitSectionName(track.ID, region.Slug) == id {
				return true
			}
		}
	}
	return false
}

func builtInTrackIDs() map[string]bool {
	known := make(map[string]bool, len(assetTracks))
	for _, track := range assetTracks {
		known[track.ID] = true
	}
	return known
}

// manifestVariants finds the gated alternates for one slot, and ONLY the ones
// the builder owns: records under the <slot>-<region> names the split action
// writes.
//
// It used to match on src instead -- any record naming the same ROM song was
// listed under it. That surfaced hand-authored records the builder deliberately
// will not edit, so a player saw an entry it could not remove and had to go
// find the manifest by hand. Showing something unmanageable is worse than not
// showing it: a record outside this namespace is now left alone AND left out,
// which is the same policy the rest of the builder follows.
//
// A record with no reachable file path is skipped: it cannot be filled here.
func manifestVariants(root, manifest, manifestPath string,
	track assetTrack) []assetVariantStatus {
	var variants []assetVariantStatus
	for _, region := range splitRegions(track) {
		name := splitSectionName(track.ID, region.Slug)
		section := "music:" + name
		if _, exists := findManifestSection(manifest, section); !exists {
			continue
		}
		file, hasFile := manifestSectionValue(manifest, section, "file")
		if !hasFile {
			continue
		}
		path, reachable := manifestAudioTarget(root, manifestPath, file)
		if !reachable {
			continue
		}
		gate, _ := manifestSectionValue(manifest, section, "when")
		variant := assetVariantStatus{
			Name: name, When: gate, File: file,
			Configured: regularFileExists(path),
		}
		if variant.Configured {
			variant.URL = "asset-audio/" + name + "?v=" + previewVersionToken(path)
		}
		variants = append(variants, variant)
	}
	return variants
}

func loadAssetConfiguration(root string) (assetConfiguration, error) {
	manifest, manifestPath, err := readAssetManifest(root)
	if err != nil {
		return assetConfiguration{}, err
	}
	configuration := assetConfiguration{
		ManifestPath: filepath.ToSlash(filepath.Join("game-assets", "manifest.ini")),
	}
	logo, logoFound := manifestSectionValue(manifest, "replace:title-logo", "image")
	swirl, swirlFound := manifestSectionValue(manifest, "replace:title-swirl", "image")
	configuration.Title.File = logo
	configuration.Title.Enabled = logoFound && swirlFound &&
		regularFileExists(resolveManifestFile(manifestPath, logo)) &&
		regularFileExists(resolveManifestFile(manifestPath, swirl))

	for _, track := range assetTracks {
		file, found := manifestSectionValue(manifest, "music:"+track.ID, "file")
		configured := found && regularFileExists(resolveManifestFile(manifestPath, file))
		status := assetTrackStatus{
			ID: track.ID, Name: track.Name, Src: track.Src,
			Configured: configured, File: file,
		}
		if configured {
			status.URL = installedAudioURL(root, manifestPath, track.ID, file)
		}
		status.Variants = manifestVariants(root, manifest, manifestPath, track)
		for _, region := range splitRegions(track) {
			_, exists := findManifestSection(manifest,
				"music:"+splitSectionName(track.ID, region.Slug))
			status.Splits = append(status.Splits, assetSplitStatus{
				Slug: region.Slug, Label: region.Label, Enabled: exists,
				Name: splitSectionName(track.ID, region.Slug),
				Gate: splitGate(region.Group),
				File: splitStubFile(track.ID, region.Slug),
			})
		}
		configuration.Tracks = append(configuration.Tracks, status)
	}
	return configuration, nil
}

// assetTrackRowTemplate carries a revert control per slot. A file input can be
// filled but never emptied by a page, so "no replacement" needs its own
// affordance: without one an installed track could only ever be swapped for a
// different file, never returned to the ROM's own music. The hidden companion
// field is what the save actually reads -- a button press alone would be lost
// on submit.
const assetTrackRowTemplate = `<div class="asset-row" data-track="{ID}" style="--tint-h:{HUE}">
  <div class="asset-copy"><label for="track-{ID}">{NAME}</label><span>Manifest [music:{ID}] &middot; ROM source {SRC}</span></div>
  <div class="asset-picker"><input id="track-{ID}" name="track-{ID}" type="file" accept=".ogg,.oga,audio/ogg">
    <div class="asset-row-foot">
      <span class="asset-current" id="track-state-{ID}">Not installed</span>
      <button type="button" class="asset-clear" hidden>Use original</button>
      <button type="button" class="split-toggle" aria-expanded="false">Split by level</button>
    </div>
    <input class="asset-remove" name="track-remove-{ID}" type="hidden" value="0">
    <input class="split-change" name="split-change-{ID}" type="hidden" value="0">
    <div class="asset-split" hidden>
      <p class="split-note">Give this song its own track in chosen levels. Each one becomes a
      gated entry below, ready for a file. Only the levels whose maps actually play this song
      are listed &mdash; that comes from the ROM&rsquo;s own per-map script, so the choice
      cannot name a level the gate could never fire in.</p>
      <div class="split-regions"></div>
    </div>
    <div class="audio-compare">
      <div><span>Original ROM</span><audio class="original-audio" controls preload="metadata" hidden></audio></div>
      <div><span class="replacement-caption">Selected replacement</span><audio class="replacement-audio" controls preload="metadata" hidden></audio></div>
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

func bundledTitleRelativePath() string {
	digest := sha256.Sum256(titleLogoPNG)
	return filepath.ToSlash(filepath.Join("hd", "builder",
		"title-logo-"+hex.EncodeToString(digest[:8])+".png"))
}

func stageBytes(root, relative string, content []byte) (stagedAsset, error) {
	final := filepath.Join(root, "game-assets", filepath.FromSlash(relative))
	directory := filepath.Dir(final)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return stagedAsset{}, fmt.Errorf("create asset directory: %w", err)
	}
	temporary, err := os.CreateTemp(directory, ".snesbuild-asset-*")
	if err != nil {
		return stagedAsset{}, err
	}
	temporaryPath := temporary.Name()
	failed := true
	defer func() {
		_ = temporary.Close()
		if failed {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err := temporary.Chmod(0o644); err != nil {
		return stagedAsset{}, err
	}
	if _, err := temporary.Write(content); err != nil {
		return stagedAsset{}, err
	}
	if err := temporary.Sync(); err != nil {
		return stagedAsset{}, err
	}
	if err := temporary.Close(); err != nil {
		return stagedAsset{}, err
	}
	failed = false
	return stagedAsset{Temporary: temporaryPath, Final: final, Relative: relative}, nil
}

// stageAudioUpload puts an uploaded file at the path a manifest record ALREADY
// names. That is the same "drop a file with the matching name" workflow the
// manifest header documents, performed through the GUI, and it is how every
// music replacement is installed -- slots and gated variants alike.
//
// It replaced a content-addressed scheme that wrote audio/builder/<id>-<hash>
// and repointed the record at it. Two things were wrong with that. Every
// re-upload wrote a NEW filename, so superseded copies of a track accumulated
// under audio/builder/ with nothing to ever collect them. And it abandoned the
// path the manifest declared, so the drop-in workflow the header advertises
// stopped working for a slot the moment the GUI touched it. Writing where the
// record points means one file per record, overwritten in place, and the two
// ways of managing a replacement stay interchangeable.
func stageAudioUpload(root, target string,
	header *multipart.FileHeader) (stagedAsset, error) {
	content, err := readVorbisUpload(header, filepath.Base(target))
	if err != nil {
		return stagedAsset{}, err
	}
	directory := filepath.Dir(target)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return stagedAsset{}, fmt.Errorf("create audio asset directory: %w", err)
	}
	temporary, err := os.CreateTemp(directory, ".snesbuild-variant-*")
	if err != nil {
		return stagedAsset{}, err
	}
	temporaryPath := temporary.Name()
	failed := true
	defer func() {
		_ = temporary.Close()
		if failed {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err := temporary.Chmod(0o644); err != nil {
		return stagedAsset{}, err
	}
	if _, err := temporary.Write(content); err != nil {
		return stagedAsset{}, err
	}
	if err := temporary.Sync(); err != nil {
		return stagedAsset{}, err
	}
	if err := temporary.Close(); err != nil {
		return stagedAsset{}, err
	}
	failed = false
	return stagedAsset{Temporary: temporaryPath, Final: target}, nil
}

// templateAudioPaths is the file path the shipped template declares for each
// slot, so a record a user deleted can be recreated naming the same friendly
// path it originally had rather than a second invented convention.
var templateAudioPaths = func() map[string]string {
	paths := make(map[string]string, len(assetTracks))
	template := string(assetManifestTemplate)
	for _, track := range assetTracks {
		if value, found := manifestSectionValue(template, "music:"+track.ID, "file"); found {
			paths[track.ID] = value
		}
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

// readVorbisUpload applies the same bounded read and format sniff the slot
// uploads use. The game performs the real decode when it loads the manifest;
// this only refuses a file that is obviously not what was asked for.
func readVorbisUpload(header *multipart.FileHeader, label string) ([]byte, error) {
	input, err := header.Open()
	if err != nil {
		return nil, err
	}
	defer input.Close()
	content, err := io.ReadAll(io.LimitReader(input, maxTrackBytes+1))
	if err != nil {
		return nil, fmt.Errorf("copy %s: %w", label, err)
	}
	if len(content) == 0 {
		return nil, fmt.Errorf("%s is empty", label)
	}
	if int64(len(content)) > maxTrackBytes {
		return nil, fmt.Errorf("%s exceeds the %d MiB safety limit",
			label, maxTrackBytes>>20)
	}
	head := content[:min(4096, len(content))]
	if !bytes.HasPrefix(head, []byte("OggS")) ||
		!bytes.Contains(head, []byte{0x01, 'v', 'o', 'r', 'b', 'i', 's'}) {
		return nil, fmt.Errorf("%s must be an Ogg Vorbis file", label)
	}
	return content, nil
}

func replaceStagedFile(asset stagedAsset) error {
	if err := os.Rename(asset.Temporary, asset.Final); err == nil {
		return nil
	} else if runtime.GOOS != "windows" {
		return err
	}
	// Windows does not replace an existing file with Rename. The content-based
	// name usually makes this unnecessary, but retaining the old file as a
	// backup keeps the fallback lossless if someone pre-created that exact path.
	if _, err := os.Stat(asset.Final); err != nil {
		return os.Rename(asset.Temporary, asset.Final)
	}
	backupFile, err := os.CreateTemp(filepath.Dir(asset.Final), ".snesbuild-backup-*")
	if err != nil {
		return err
	}
	backup := backupFile.Name()
	_ = backupFile.Close()
	_ = os.Remove(backup)
	if err := os.Rename(asset.Final, backup); err != nil {
		return err
	}
	if err := os.Rename(asset.Temporary, asset.Final); err != nil {
		_ = os.Rename(backup, asset.Final)
		return err
	}
	_ = os.Remove(backup)
	return nil
}

func writeAtomicFile(path string, content []byte) error {
	mode := os.FileMode(0o644)
	if info, err := os.Stat(path); err == nil && info.Mode().IsRegular() {
		mode = info.Mode().Perm()
	}
	directory := filepath.Dir(path)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(directory, ".manifest-*")
	if err != nil {
		return err
	}
	asset := stagedAsset{Temporary: temporary.Name(), Final: path}
	defer func() {
		_ = temporary.Close()
		_ = os.Remove(asset.Temporary)
	}()
	if err := temporary.Chmod(mode); err != nil {
		return err
	}
	if _, err := temporary.Write(content); err != nil {
		return err
	}
	if err := temporary.Sync(); err != nil {
		return err
	}
	if err := temporary.Close(); err != nil {
		return err
	}
	return replaceStagedFile(asset)
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

// serveInstalledAudio plays back a replacement that is already installed. The
// id is looked up in the manifest on every request rather than cached: the
// manifest can change under a long-lived builder session -- by hand or by a
// save in another tab -- and a stale map would serve the file that slot USED to
// name.
func (app *application) serveInstalledAudio(response http.ResponseWriter,
	request *http.Request, id string) {
	if !safeManifestTrackID.MatchString(id) || !managedTrackID(id) {
		http.NotFound(response, request)
		return
	}
	app.assetMu.Lock()
	manifest, manifestPath, err := readAssetManifest(app.options.ProjectRoot)
	app.assetMu.Unlock()
	if err != nil {
		http.NotFound(response, request)
		return
	}
	value, found := manifestSectionValue(manifest, "music:"+id, "file")
	if !found {
		http.NotFound(response, request)
		return
	}
	path, ok := installedAudioPath(app.options.ProjectRoot, manifestPath, value)
	if !ok {
		http.NotFound(response, request)
		return
	}
	file, err := os.Open(path)
	if err != nil {
		http.NotFound(response, request)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		http.NotFound(response, request)
		return
	}
	response.Header().Set("Content-Type", "audio/ogg")
	response.Header().Set("Cache-Control", "no-store")
	response.Header().Set("X-Content-Type-Options", "nosniff")
	http.ServeContent(response, request, id+".ogg", info.ModTime(), file)
}

// stageVariantChanges reads the variant half of an asset save: uploads are
// staged at the path each record already names, and removals are queued for the
// same path. Reports whether anything was requested.
func (app *application) stageVariantChanges(request *http.Request,
	manifest, manifestPath string, staged *[]stagedAsset,
	reverted *[]string) (bool, error) {
	touched := false
	// Same namespace the rows come from, so a crafted request cannot reach a
	// record the page would not have shown.
	for _, track := range assetTracks {
		for _, region := range splitRegions(track) {
			name := splitSectionName(track.ID, region.Slug)
			section := "music:" + name
			if _, exists := findManifestSection(manifest, section); !exists {
				continue
			}
			file, hasFile := manifestSectionValue(manifest, section, "file")
			if !hasFile {
				continue
			}
			target, reachable := manifestAudioTarget(
				app.options.ProjectRoot, manifestPath, file)
			if !reachable {
				continue
			}
			files := request.MultipartForm.File["variant-"+name]
			hasUpload := len(files) > 0 &&
				!(files[0].Filename == "" && files[0].Size == 0)
			if !hasUpload {
				if request.FormValue("variant-remove-"+name) == "1" {
					*reverted = append(*reverted, target)
					touched = true
				}
				continue
			}
			if len(files) != 1 {
				return touched, fmt.Errorf("select only one file for %s", name)
			}
			asset, err := stageAudioUpload(
				app.options.ProjectRoot, target, files[0])
			if err != nil {
				return touched, err
			}
			*staged = append(*staged, asset)
			touched = true
		}
	}
	return touched, nil
}

func (app *application) writeAssets(response http.ResponseWriter) {
	configuration, err := loadAssetConfiguration(app.options.ProjectRoot)
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(response, http.StatusOK, configuration)
}

func (app *application) saveAssets(response http.ResponseWriter, request *http.Request) {
	app.assetMu.Lock()
	defer app.assetMu.Unlock()

	request.Body = http.MaxBytesReader(response, request.Body, maxAssetRequestBytes)
	if err := request.ParseMultipartForm(8 << 20); err != nil {
		writeJSONError(response, http.StatusBadRequest,
			"could not read the selected asset files")
		return
	}
	if request.MultipartForm == nil {
		writeJSONError(response, http.StatusBadRequest,
			"asset saves must use a multipart form")
		return
	}
	defer request.MultipartForm.RemoveAll()

	manifest, manifestPath, err := readAssetManifest(app.options.ProjectRoot)
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	// `changed` means the MANIFEST text needs rewriting; `touched` means the
	// save did something at all. They came apart once a file could be
	// installed or removed at the path a record already names, leaving the
	// manifest untouched -- without the second flag such a save fell through
	// the "nothing was selected" branch and silently did nothing.
	changed := false
	touched := false
	var staged []stagedAsset
	defer func() {
		for _, asset := range staged {
			_ = os.Remove(asset.Temporary)
		}
	}()

	if request.FormValue("title-change") == "1" {
		relative := bundledTitleRelativePath()
		logo, swirl := titleManifestValues(relative)
		manifest = upsertManifestSection(manifest, "replace:title-logo", logo)
		manifest = upsertManifestSection(manifest, "replace:title-swirl", swirl)
		if request.FormValue("title") == "on" {
			asset, stageErr := stageBytes(app.options.ProjectRoot, relative, titleLogoPNG)
			if stageErr != nil {
				writeJSONError(response, http.StatusInternalServerError,
					"prepare bundled title art: "+stageErr.Error())
				return
			}
			staged = append(staged, asset)
		}
		changed = true
		touched = true
	}

	var reverted []string
	for _, track := range assetTracks {
		relative, target, reachable := slotAudioTarget(
			app.options.ProjectRoot, manifest, manifestPath, track)
		files := request.MultipartForm.File["track-"+track.ID]
		if len(files) == 0 || (files[0].Filename == "" && files[0].Size == 0) {
			// Reverting to the original ROM music. Checked only when no
			// replacement was also chosen, so a slot that is cleared and then
			// re-filled in one visit ends up with the new file rather than
			// racing its own removal.
			if request.FormValue("track-remove-"+track.ID) != "1" {
				continue
			}
			// The FILE goes; the RECORD stays, naming the same path. That is
			// what keeps the hand-managed route working: the slot is back to
			// authentic audio now, and dropping a file at that path re-engages
			// it without the manifest needing an entry put back by hand.
			// Removing the record instead left the shipped stub gone for good,
			// since the template never re-seeds an existing manifest.
			if reachable {
				reverted = append(reverted, target)
				touched = true
			}
			continue
		}
		if len(files) != 1 {
			writeJSONError(response, http.StatusBadRequest,
				"select only one file for "+track.Name)
			return
		}
		if !reachable {
			writeJSONError(response, http.StatusBadRequest,
				track.Name+" points outside game-assets; edit its manifest "+
					"record to a path inside it, or manage that file by hand")
			return
		}
		asset, stageErr := stageAudioUpload(app.options.ProjectRoot, target, files[0])
		if stageErr != nil {
			writeJSONError(response, http.StatusBadRequest, stageErr.Error())
			return
		}
		staged = append(staged, asset)
		touched = true
		// The manifest is touched only when it does not already say this. A
		// record that names the target path is left exactly as the user wrote
		// it, gain/loop/gate keys and all -- re-upserting identical values
		// would rewrite the file on every upload for nothing.
		if current, found := manifestSectionValue(
			manifest, "music:"+track.ID, "file"); !found || current != relative {
			values := []manifestValue{
				{Key: "src", Value: track.Src},
				{Key: "file", Value: relative},
			}
			if track.Loop != nil {
				loop := "0"
				if *track.Loop {
					loop = "1"
				}
				values = append(values, manifestValue{Key: "loop", Value: loop})
			}
			manifest = upsertManifestSection(manifest, "music:"+track.ID, values)
			changed = true
			touched = true
		}
	}

	// Splits: the builder authors these records, because the region byte is a
	// fixed table it already knows. Only sections in its own <slot>-<region>
	// namespace are created or removed; a hand-authored variant beside them is
	// never touched.
	for _, track := range assetTracks {
		if request.FormValue("split-change-"+track.ID) != "1" {
			continue
		}
		wanted := make(map[string]bool)
		for _, slug := range request.Form["split-"+track.ID] {
			wanted[slug] = true
		}
		for _, region := range splitRegions(track) {
			name := "music:" + splitSectionName(track.ID, region.Slug)
			_, exists := findManifestSection(manifest, name)
			if wanted[region.Slug] == exists {
				continue
			}
			if !exists {
				// Created as a STUB: the record and its gate, naming a file
				// that is not there yet. That is the manifest's own idiom for
				// an available hook, and it makes the new row appear straight
				// away with a picker on it.
				manifest = upsertManifestSection(manifest, name, []manifestValue{
					{Key: "src", Value: track.Src},
					{Key: "when", Value: splitGate(region.Group)},
					{Key: "file", Value: splitStubFile(track.ID, region.Slug)},
				})
				changed = true
				touched = true
				continue
			}
			if file, found := manifestSectionValue(manifest, name, "file"); found {
				if path, ok := manifestAudioTarget(
					app.options.ProjectRoot, manifestPath, file); ok {
					reverted = append(reverted, path)
				}
			}
			manifest = removeManifestSection(manifest, name)
			changed = true
			touched = true
		}
	}

	// Variants: files only, manifest never. Collected before the manifest write
	// so a rejected upload aborts the whole save, and installed after it for the
	// same reason the slot uploads are -- nothing should reference a file that
	// is not there yet.
	variantsTouched, variantErr := app.stageVariantChanges(
		request, manifest, manifestPath, &staged, &reverted)
	if variantErr != nil {
		writeJSONError(response, http.StatusBadRequest, variantErr.Error())
		return
	}
	touched = touched || variantsTouched

	// A save that only installed files leaves the manifest text alone -- but if
	// the live manifest does not exist yet, "alone" would mean never writing
	// it, and the game reads only the live path. Seed it, so a first save on a
	// fresh install cannot leave a file on disk that nothing points at.
	if touched && !regularFileExists(manifestPath) {
		changed = true
	}

	if !changed && !touched {
		configuration, loadErr := loadAssetConfiguration(app.options.ProjectRoot)
		if loadErr != nil {
			writeJSONError(response, http.StatusInternalServerError, loadErr.Error())
			return
		}
		writeJSON(response, http.StatusOK, map[string]any{
			"message": "No asset changes were selected.", "config": configuration,
		})
		return
	}

	// Assets land first, then the manifest starts referring to them. A failed
	// manifest write can leave an unused content-addressed file, but never a
	// manifest entry whose newly selected file is absent.
	for index := range staged {
		if err := replaceStagedFile(staged[index]); err != nil {
			writeJSONError(response, http.StatusInternalServerError,
				"install replacement asset: "+err.Error())
			return
		}
		staged[index].Temporary = ""
	}
	// Only when a managed record actually changed. A variant-only save must
	// leave the file byte-for-byte alone: rewriting it would reformat records
	// the builder does not own for no reason at all.
	if changed {
		if err := writeAtomicFile(manifestPath, []byte(manifest)); err != nil {
			writeJSONError(response, http.StatusInternalServerError,
				"update asset manifest: "+err.Error())
			return
		}
	}

	// Reverted files go only AFTER the manifest no longer references them, so an
	// interrupted save can leave an orphaned file but never a record pointing at
	// a file that is gone.
	for _, path := range reverted {
		if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
			writeJSONError(response, http.StatusInternalServerError,
				"remove the replaced music file: "+err.Error())
			return
		}
	}

	// Turning the included title off means the manifest keeps a valid hook but
	// its builder-owned image is absent. The embedded bytes make this deletion
	// recoverable: checking the toggle and saving restores the exact file.
	if request.FormValue("title-change") == "1" && request.FormValue("title") != "on" {
		managedTitle := filepath.Join(app.options.ProjectRoot, "game-assets",
			filepath.FromSlash(bundledTitleRelativePath()))
		if err := os.Remove(managedTitle); err != nil && !errors.Is(err, os.ErrNotExist) {
			writeJSONError(response, http.StatusInternalServerError,
				"disable bundled title art: "+err.Error())
			return
		}
	}

	configuration, err := loadAssetConfiguration(app.options.ProjectRoot)
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"message": "Assets saved. Changes apply the next time the game starts.",
		"config":  configuration,
	})
}
