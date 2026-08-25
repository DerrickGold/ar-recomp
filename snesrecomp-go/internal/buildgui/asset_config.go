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
	"runtime"
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
}

func boolPointer(value bool) *bool { return &value }

// The complete 17-entry song-image table. Slot 07 is the separately named
// title-theme record; entries whose musical identity is not established stay
// visible under an explicit "Unidentified track NN" label. That lets players
// render/listen to every image and map it back to [music:song-NN] without the UI
// inventing a title.
var assetTracks = []assetTrack{
	{ID: "title-theme", Name: "Title Theme", Src: "1A:94B8", PreviewSource: 0x1a94b8, PreviewSong: 1},
	{ID: "song-00", Name: "Fillmore", Src: "18:947F", PreviewSource: 0x18947f, PreviewSong: 1},
	{ID: "song-01", Name: "Sky Palace", Src: "1C:A988", PreviewSource: 0x1ca988, PreviewSong: 1},
	{ID: "song-02", Name: "Bloodpool", Src: "18:DDCC", PreviewSource: 0x18ddcc, PreviewSong: 1},
	{ID: "song-03", Name: "Unidentified track 03", Src: "1A:E9E2", PreviewSource: 0x1ae9e2, PreviewSong: 1},
	{ID: "song-04", Name: "Unidentified track 04", Src: "1C:A5FB", PreviewSource: 0x1ca5fb, PreviewSong: 1},
	{ID: "song-05", Name: "Unidentified track 05", Src: "1B:8554", PreviewSource: 0x1b8554, PreviewSong: 1},
	{ID: "song-06", Name: "Unidentified track 06", Src: "1B:9470", PreviewSource: 0x1b9470, PreviewSong: 1},
	{ID: "song-08", Name: "Advent", Src: "1C:A7CC", PreviewSource: 0x1ca7cc, PreviewSong: 1, Loop: boolPointer(false)},
	{ID: "song-09", Name: "Act 2 Theme", Src: "0E:F69F", PreviewSource: 0x0ef69f, PreviewSong: 1},
	{ID: "song-10", Name: "Birth of the People", Src: "1B:ABED", PreviewSource: 0x1babed, PreviewSong: 1},
	{ID: "song-11", Name: "Level Up", Src: "1C:AFEB", PreviewSource: 0x1cafeb, PreviewSong: 1},
	{ID: "song-12", Name: "Unidentified track 12", Src: "19:FA4B", PreviewSource: 0x19fa4b, PreviewSong: 1},
	{ID: "song-13", Name: "Unidentified track 13", Src: "17:C027", PreviewSource: 0x17c027, PreviewSong: 1},
	{ID: "song-14", Name: "Unidentified track 14", Src: "18:D4FA", PreviewSource: 0x18d4fa, PreviewSong: 1},
	{ID: "song-15", Name: "Unidentified track 15", Src: "1C:9F3D", PreviewSource: 0x1c9f3d, PreviewSong: 1},
	{ID: "song-16", Name: "Kasandora", Src: "1A:EF63", PreviewSource: 0x1aef63, PreviewSong: 1},
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
		configuration.Tracks = append(configuration.Tracks, assetTrackStatus{
			ID: track.ID, Name: track.Name, Src: track.Src,
			Configured: configured, File: file,
		})
	}
	return configuration, nil
}

func renderAssetTrackRows() string {
	var output strings.Builder
	for _, track := range assetTracks {
		id := html.EscapeString(track.ID)
		name := html.EscapeString(track.Name)
		src := html.EscapeString(track.Src)
		fmt.Fprintf(&output, `<div class="asset-row" data-track="%s">
  <div class="asset-copy"><label for="track-%s">%s</label><span>Manifest [music:%s] &middot; ROM source %s</span></div>
  <div class="asset-picker"><input id="track-%s" name="track-%s" type="file" accept=".ogg,.oga,audio/ogg"><span class="asset-current" id="track-state-%s">Not installed</span>
    <div class="audio-compare">
      <div><span>Original ROM</span><audio class="original-audio" controls preload="metadata" hidden></audio></div>
      <div><span>Selected replacement</span><audio class="replacement-audio" controls preload="metadata" hidden></audio></div>
    </div>
  </div>
</div>`, id, id, name, id, src, id, id, id)
	}
	return output.String()
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

func stageTrackUpload(root string, track assetTrack, header *multipart.FileHeader) (stagedAsset, error) {
	input, err := header.Open()
	if err != nil {
		return stagedAsset{}, err
	}
	defer input.Close()
	directory := filepath.Join(root, "game-assets", "audio", "builder")
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return stagedAsset{}, fmt.Errorf("create audio asset directory: %w", err)
	}
	temporary, err := os.CreateTemp(directory, ".snesbuild-track-*")
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
	digest := sha256.New()
	written, err := io.Copy(io.MultiWriter(temporary, digest),
		io.LimitReader(input, maxTrackBytes+1))
	if err != nil {
		return stagedAsset{}, fmt.Errorf("copy %s: %w", track.Name, err)
	}
	if written == 0 {
		return stagedAsset{}, fmt.Errorf("%s is empty", track.Name)
	}
	if written > maxTrackBytes {
		return stagedAsset{}, fmt.Errorf("%s exceeds the %d MiB safety limit",
			track.Name, maxTrackBytes>>20)
	}
	if err := temporary.Sync(); err != nil {
		return stagedAsset{}, err
	}
	if _, err := temporary.Seek(0, io.SeekStart); err != nil {
		return stagedAsset{}, err
	}
	headerBytes := make([]byte, min(4096, int(written)))
	if _, err := io.ReadFull(temporary, headerBytes); err != nil {
		return stagedAsset{}, err
	}
	if !bytes.HasPrefix(headerBytes, []byte("OggS")) ||
		!bytes.Contains(headerBytes, []byte{0x01, 'v', 'o', 'r', 'b', 'i', 's'}) {
		return stagedAsset{}, fmt.Errorf("%s must be an Ogg Vorbis file", track.Name)
	}
	if err := temporary.Close(); err != nil {
		return stagedAsset{}, err
	}
	hash := hex.EncodeToString(digest.Sum(nil)[:8])
	leaf := track.ID + "-" + hash + ".ogg"
	relative := filepath.ToSlash(filepath.Join("audio", "builder", leaf))
	failed = false
	return stagedAsset{
		Temporary: temporaryPath,
		Final:     filepath.Join(directory, leaf),
		Relative:  relative,
	}, nil
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
	changed := false
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
	}

	for _, track := range assetTracks {
		files := request.MultipartForm.File["track-"+track.ID]
		if len(files) == 0 || (files[0].Filename == "" && files[0].Size == 0) {
			continue
		}
		if len(files) != 1 {
			writeJSONError(response, http.StatusBadRequest,
				"select only one file for "+track.Name)
			return
		}
		asset, stageErr := stageTrackUpload(app.options.ProjectRoot, track, files[0])
		if stageErr != nil {
			writeJSONError(response, http.StatusBadRequest, stageErr.Error())
			return
		}
		staged = append(staged, asset)
		values := []manifestValue{
			{Key: "src", Value: track.Src},
			{Key: "file", Value: asset.Relative},
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
	}

	if !changed {
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
	if err := writeAtomicFile(manifestPath, []byte(manifest)); err != nil {
		writeJSONError(response, http.StatusInternalServerError,
			"update asset manifest: "+err.Error())
		return
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
