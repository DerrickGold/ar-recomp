package buildgui

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeAssetTestFile(t *testing.T, path string, content []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, content, 0o644); err != nil {
		t.Fatal(err)
	}
}

func fakeVorbisFile() []byte {
	// The upload gate is intentionally a format sniff, not a decoder. The game
	// performs the full Vorbis decode/probe when it loads the saved manifest.
	return append([]byte("OggS\x00\x02test-page\x01vorbis"), bytes.Repeat([]byte{0x5a}, 128)...)
}

func postAssetForm(t *testing.T, app *application, fields map[string]string,
	files map[string][]byte) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	for name, value := range fields {
		if err := writer.WriteField(name, value); err != nil {
			t.Fatal(err)
		}
	}
	for name, content := range files {
		part, err := writer.CreateFormFile(name, name+".ogg")
		if err != nil {
			t.Fatal(err)
		}
		if _, err := part.Write(content); err != nil {
			t.Fatal(err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/tok/assets", &body)
	request.Header.Set("Content-Type", writer.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, request)
	return response
}

// postAssetValues posts a form that needs repeated fields, which the map-based
// helper cannot express.
func postAssetValues(t *testing.T, app *application,
	values url.Values) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	for name, list := range values {
		for _, value := range list {
			if err := writer.WriteField(name, value); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/tok/assets", &body)
	request.Header.Set("Content-Type", writer.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, request)
	return response
}

func TestAssetSaveCopiesFilesAndPreservesCustomManifestContent(t *testing.T) {
	root := t.TempDir()
	manifestPath := liveAssetManifestPath(root)
	manifest := `# player's manifest

[replace:title-logo]
plane = screen
layer = bg1
rect = 11,27,248,122
image = hd/custom-title.png
when = custom-title-gate

[replace:title-swirl]
plane = mode7
canvas_rect = 139,156,376,251
image = hd/custom-title.png
when = custom-swirl-gate

[music:song-00]
src = 18:947F
file = audio/old-fillmore.ogg
gain = 73
when = wram[1234]==1

[music:song-03]
src = 1A:E9E2
file = audio/player-authored.ogg

[replace:player-custom]
plane = screen
layer = obj
rect = 1,2,3,4
image = hd/custom.png
`
	writeAssetTestFile(t, manifestPath, []byte(manifest))
	writeAssetTestFile(t, filepath.Join(root, "game-assets", "hd", "custom-title.png"),
		[]byte("custom title stays on disk"))

	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app,
		map[string]string{"title-change": "1", "title": "on"},
		map[string][]byte{"track-song-00": fakeVorbisFile()})
	if response.Code != http.StatusOK {
		t.Fatalf("save status = %d: %s", response.Code, response.Body.String())
	}

	updatedBytes, err := os.ReadFile(manifestPath)
	if err != nil {
		t.Fatal(err)
	}
	updated := string(updatedBytes)
	for _, preserved := range []string{
		"# player's manifest", "gain = 73", "when = wram[1234]==1",
		"[music:song-03]", "file = audio/player-authored.ogg",
		"[replace:player-custom]", "image = hd/custom.png",
	} {
		if !strings.Contains(updated, preserved) {
			t.Errorf("updated manifest lost %q:\n%s", preserved, updated)
		}
	}
	if strings.Contains(updated, "custom-title-gate") ||
		strings.Contains(updated, "custom-swirl-gate") {
		t.Error("the title gates were not normalized with the bundled title hooks")
	}
	if count := strings.Count(updated, "[music:song-00]"); count != 1 {
		t.Errorf("Fillmore section count = %d, want 1", count)
	}

	// The upload goes where the record ALREADY points, so the path the player
	// wrote survives and the record is not rewritten at all.
	musicRelative, ok := manifestSectionValue(updated, "music:song-00", "file")
	if !ok || musicRelative != "audio/old-fillmore.ogg" {
		t.Fatalf("saved music path = %q, want the record's own audio/old-fillmore.ogg",
			musicRelative)
	}
	musicBytes, err := os.ReadFile(resolveManifestFile(manifestPath, musicRelative))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(musicBytes, fakeVorbisFile()) {
		t.Error("installed music differs from the uploaded file")
	}

	logoRelative, ok := manifestSectionValue(updated, "replace:title-logo", "image")
	if !ok || logoRelative != bundledTitleRelativePath() {
		t.Fatalf("saved title path = %q", logoRelative)
	}
	swirlRelative, _ := manifestSectionValue(updated, "replace:title-swirl", "image")
	if swirlRelative != logoRelative {
		t.Fatalf("title hooks disagree: logo %q, swirl %q", logoRelative, swirlRelative)
	}
	installedTitle, err := os.ReadFile(resolveManifestFile(manifestPath, logoRelative))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(installedTitle, titleLogoPNG) {
		t.Error("installed title differs from the embedded art")
	}
	customTitle, err := os.ReadFile(filepath.Join(root, "game-assets", "hd", "custom-title.png"))
	if err != nil || string(customTitle) != "custom title stays on disk" {
		t.Error("enabling the bundled title overwrote or removed the player's custom art")
	}

	var saved struct {
		Config assetConfiguration `json:"config"`
	}
	if err := json.NewDecoder(response.Body).Decode(&saved); err != nil {
		t.Fatal(err)
	}
	if !saved.Config.Title.Enabled {
		t.Error("saved configuration does not report the title as enabled")
	}
	if !saved.Config.Tracks[1].Configured { // song-00 follows title-theme
		t.Errorf("Fillmore status is not configured: %+v", saved.Config.Tracks[1])
	}
}

func TestAssetSaveCanDisableAndRestoreBundledTitle(t *testing.T) {
	root := t.TempDir()
	manifestPath := liveAssetManifestPath(root)
	writeAssetTestFile(t, manifestPath, []byte("# empty player manifest\n"))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")

	enabled := postAssetForm(t, app,
		map[string]string{"title-change": "1", "title": "on"}, nil)
	if enabled.Code != http.StatusOK {
		t.Fatalf("enable status = %d: %s", enabled.Code, enabled.Body.String())
	}
	titlePath := filepath.Join(root, "game-assets",
		filepath.FromSlash(bundledTitleRelativePath()))
	if !regularFileExists(titlePath) {
		t.Fatal("enabling the title did not materialize its image")
	}

	disabled := postAssetForm(t, app,
		map[string]string{"title-change": "1"}, nil)
	if disabled.Code != http.StatusOK {
		t.Fatalf("disable status = %d: %s", disabled.Code, disabled.Body.String())
	}
	if _, err := os.Stat(titlePath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("disabled title still exists: %v", err)
	}
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	if configuration.Title.Enabled {
		t.Error("disabled title still reports as enabled")
	}
	manifestBytes, err := os.ReadFile(manifestPath)
	if err != nil {
		t.Fatal(err)
	}
	if value, _ := manifestSectionValue(string(manifestBytes),
		"replace:title-logo", "image"); value != bundledTitleRelativePath() {
		t.Errorf("disabled hook no longer points at the recoverable bundled path: %q", value)
	}

	restored := postAssetForm(t, app,
		map[string]string{"title-change": "1", "title": "on"}, nil)
	if restored.Code != http.StatusOK || !regularFileExists(titlePath) {
		t.Fatalf("restore status = %d, file exists = %t: %s",
			restored.Code, regularFileExists(titlePath), restored.Body.String())
	}
}

func TestAssetSaveSeedsLiveManifestFromBundleDefault(t *testing.T) {
	root := t.TempDir()
	defaultManifest := defaultAssetManifestPath(root)
	writeAssetTestFile(t, defaultManifest, []byte(`# shipped default
[music:title-theme]
src = 1A:94B8
file = audio/title.ogg
gain = 88
`))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app, nil,
		map[string][]byte{"track-title-theme": fakeVorbisFile()})
	if response.Code != http.StatusOK {
		t.Fatalf("save status = %d: %s", response.Code, response.Body.String())
	}
	live, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(live), "# shipped default") ||
		!strings.Contains(string(live), "gain = 88") ||
		!strings.Contains(string(live), "file = audio/title.ogg") {
		t.Fatalf("live manifest was not seeded:\n%s", live)
	}
	// A file-only save still has to WRITE the seeded manifest, or the game --
	// which reads the live path only -- would never see the record.
	installed := filepath.Join(root, "game-assets", "audio", "title.ogg")
	if got, readErr := os.ReadFile(installed); readErr != nil ||
		!bytes.Equal(got, fakeVorbisFile()) {
		t.Fatalf("the upload did not land at the declared path: %v", readErr)
	}
	defaults, err := os.ReadFile(defaultManifest)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(defaults), "file = audio/title.ogg") {
		t.Error("saving assets modified the shipped default")
	}
}

func TestAssetSaveRejectsNonVorbisWithoutChangingManifest(t *testing.T) {
	root := t.TempDir()
	manifestPath := liveAssetManifestPath(root)
	const original = "# do not touch\n[music:song-00]\nsrc = 18:947F\nfile = audio/original.ogg\n"
	writeAssetTestFile(t, manifestPath, []byte(original))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app, nil,
		map[string][]byte{"track-song-00": []byte("not an ogg file")})
	if response.Code != http.StatusBadRequest ||
		!strings.Contains(response.Body.String(), "Ogg Vorbis") {
		t.Fatalf("unexpected rejection: %d %s", response.Code, response.Body.String())
	}
	after, err := os.ReadFile(manifestPath)
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != original {
		t.Fatalf("rejected upload changed the manifest:\n%s", after)
	}
}

func TestAssetEndpointsRemainSessionTokenGated(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	for _, request := range []*http.Request{
		httptest.NewRequest(http.MethodGet, "/assets", nil),
		httptest.NewRequest(http.MethodPost, "/assets", strings.NewReader("")),
	} {
		response := httptest.NewRecorder()
		app.ServeHTTP(response, request)
		if response.Code != http.StatusNotFound {
			t.Errorf("%s assets without token = %d", request.Method, response.Code)
		}
	}
}

// Reverting a slot is the half the GUI never had: a replacement could be
// swapped for another file but never taken off, because a page cannot empty a
// file input. Removing the record is what returns the slot to the ROM's music.
func TestAssetSaveRevertsATrackToTheOriginalMusic(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	if response := postAssetForm(t, app, nil,
		map[string][]byte{"track-song-00": fakeVorbisFile()}); response.Code != http.StatusOK {
		t.Fatalf("install status = %d: %s", response.Code, response.Body.String())
	}
	installed, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	var installedFile string
	for _, track := range installed.Tracks {
		if track.ID == "song-00" {
			if !track.Configured {
				t.Fatal("song-00 was not installed")
			}
			installedFile = track.File
		}
	}
	onDisk := resolveManifestFile(liveAssetManifestPath(root), installedFile)

	response := postAssetForm(t, app, map[string]string{"track-remove-song-00": "1"}, nil)
	if response.Code != http.StatusOK {
		t.Fatalf("revert status = %d: %s", response.Code, response.Body.String())
	}
	manifest, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	// The FILE goes; the RECORD stays, so dropping a file back at that path
	// re-engages the slot without the manifest needing an entry put back.
	if !strings.Contains(string(manifest), "[music:song-00]") {
		t.Errorf("the revert removed the slot's record:\n%s", manifest)
	}
	if _, err := os.Stat(onDisk); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("the installed file survived the revert: %v", err)
	}
	reverted, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, track := range reverted.Tracks {
		if track.ID == "song-00" && track.Configured {
			t.Error("song-00 still reports a replacement after the revert")
		}
	}
}

// A revert removes the file the record points at -- that file IS the
// replacement, whoever put it there. What it must never reach is a path outside
// game-assets: a record can name anywhere, and the builder writes and deletes
// only inside the tree it owns. Such a slot is refused rather than half-served.
func TestAssetRevertNeverReachesOutsideGameAssets(t *testing.T) {
	root := t.TempDir()
	outside := filepath.Join(root, "private-mix.ogg")
	if err := os.WriteFile(outside, fakeVorbisFile(), 0o600); err != nil {
		t.Fatal(err)
	}
	const manifest = `[music:song-01]
src = 1C:A988
file = ../private-mix.ogg

[replace:player-custom]
plane = screen
image = hd/custom.png
`
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(manifest))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app, map[string]string{"track-remove-song-01": "1"}, nil)
	if response.Code != http.StatusOK {
		t.Fatalf("revert status = %d: %s", response.Code, response.Body.String())
	}
	if _, err := os.Stat(outside); err != nil {
		t.Errorf("a file outside game-assets was deleted: %v", err)
	}
	after, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != manifest {
		t.Errorf("the manifest was rewritten:\n%s", after)
	}
	// An UPLOAD to the same slot is refused outright rather than installed
	// somewhere the record does not name.
	response = postAssetForm(t, app, nil,
		map[string][]byte{"track-song-01": fakeVorbisFile()})
	if response.Code != http.StatusBadRequest ||
		!strings.Contains(response.Body.String(), "outside game-assets") {
		t.Fatalf("upload to an out-of-tree record = %d %s",
			response.Code, response.Body.String())
	}
}

// Choosing a file and reverting in the same visit must not fight: the upload is
// what the reader picked last, so it wins and the slot ends up installed.
func TestAssetSaveUploadBeatsAConcurrentRevertFlag(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app,
		map[string]string{"track-remove-song-02": "1"},
		map[string][]byte{"track-song-02": fakeVorbisFile()})
	if response.Code != http.StatusOK {
		t.Fatalf("save status = %d: %s", response.Code, response.Body.String())
	}
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, track := range configuration.Tracks {
		if track.ID == "song-02" && !track.Configured {
			t.Error("the uploaded replacement was dropped by the revert flag")
		}
	}
}

func TestRemoveManifestSectionIsANoOpForAbsentRecords(t *testing.T) {
	const manifest = "[music:song-00]\nfile = a.ogg\n"
	if got := removeManifestSection(manifest, "music:song-09"); got != manifest {
		t.Errorf("removing an absent record rewrote the manifest:\n%s", got)
	}
}

// A replacement must be playable in the sessions AFTER the one that uploaded
// it. The row's player was fed only by URL.createObjectURL on a fresh pick, so
// re-opening the builder left an installed replacement unplayable and the only
// way to hear it was to upload the file again.
func TestInstalledReplacementIsPlayableOnALaterVisit(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	if response := postAssetForm(t, app, nil,
		map[string][]byte{"track-song-00": fakeVorbisFile()}); response.Code != http.StatusOK {
		t.Fatalf("install status = %d: %s", response.Code, response.Body.String())
	}

	// A FRESH application, as if the builder had been closed and reopened.
	reopened := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	var url string
	for _, track := range configuration.Tracks {
		if track.ID == "song-00" {
			url = track.URL
		}
	}
	if url == "" {
		t.Fatal("an installed replacement reports no playable URL")
	}
	if !strings.HasPrefix(url, "asset-audio/song-00?v=") {
		t.Fatalf("unexpected playback URL %q", url)
	}
	response := httptest.NewRecorder()
	reopened.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/tok/"+url, nil))
	if response.Code != http.StatusOK {
		t.Fatalf("playback status = %d: %s", response.Code, response.Body.String())
	}
	if !bytes.Equal(response.Body.Bytes(), fakeVorbisFile()) {
		t.Errorf("served %d bytes; not the installed file", response.Body.Len())
	}
	if got := response.Header().Get("Content-Type"); got != "audio/ogg" {
		t.Errorf("content type = %q", got)
	}
}

// The manifest is hand-editable, so its file value is untrusted input to this
// endpoint. Nothing outside game-assets may be turned into an HTTP GET.
func TestInstalledAudioRefusesPathsOutsideGameAssets(t *testing.T) {
	root := t.TempDir()
	secret := filepath.Join(root, "private.ogg")
	if err := os.WriteFile(secret, []byte("OggS secret"), 0o600); err != nil {
		t.Fatal(err)
	}
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(
		"[music:song-00]\nfile = ../private.ogg\n\n[music:song-01]\nfile = "+secret+"\n"))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	for _, id := range []string{"song-00", "song-01", "..%2Fmanifest", "song-00/../../etc"} {
		response := httptest.NewRecorder()
		app.ServeHTTP(response,
			httptest.NewRequest(http.MethodGet, "/tok/asset-audio/"+id, nil))
		if response.Code != http.StatusNotFound {
			t.Errorf("asset-audio/%s served %d; must refuse", id, response.Code)
		}
	}
	// Nor may it be reported as playable in the first place.
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, track := range configuration.Tracks {
		if track.URL != "" {
			t.Errorf("%s offered a URL for a file outside game-assets", track.ID)
		}
	}
}

// The split action authors the records the builder already has the facts for:
// the region byte is a fixed table, so a player should not have to look up
// $18 and hand-write a gate.
func TestSplitActionCreatesAndRemovesGatedStubs(t *testing.T) {
	root := t.TempDir()
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(`# player's notes

[music:song-09]
src = 0E:F69F
file = audio/act2.ogg

[music:hand-authored]
src = 0E:F69F
when = wram[0018]==0x06
file = audio/mine.ogg
`))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")

	response := postAssetForm(t, app, map[string]string{
		"split-change-song-09": "1", "split-song-09": "fillmore",
	}, nil)
	if response.Code != http.StatusOK {
		t.Fatalf("split status = %d: %s", response.Code, response.Body.String())
	}
	manifest, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		"[music:song-09-fillmore]", "src = 0E:F69F",
		"when = wram[0018]==0x01", "file = audio/song-09-fillmore.ogg",
		// nothing else may be disturbed
		"# player's notes", "[music:hand-authored]", "when = wram[0018]==0x06",
	} {
		if !strings.Contains(string(manifest), want) {
			t.Errorf("manifest is missing %q:\n%s", want, manifest)
		}
	}
	if strings.Contains(string(manifest), "[music:song-09-kasandora]") {
		t.Error("an unrequested region was created")
	}

	// It shows up as a fillable variant under its slot, stubbed.
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	var act2 assetTrackStatus
	for _, track := range configuration.Tracks {
		if track.ID == "song-09" {
			act2 = track
		}
	}
	names := map[string]assetVariantStatus{}
	for _, variant := range act2.Variants {
		names[variant.Name] = variant
	}
	if created, ok := names["song-09-fillmore"]; !ok || created.Configured ||
		created.When != "wram[0018]==0x01" {
		t.Fatalf("the created split is not offered as a stub: %#v", act2.Variants)
	}
	enabled := map[string]bool{}
	for _, split := range act2.Splits {
		enabled[split.Slug] = split.Enabled
	}
	if !enabled["fillmore"] || enabled["kasandora"] {
		t.Errorf("split state = %#v", act2.Splits)
	}
	// Only the regions the ROM says play this song, not every region.
	var act2Track assetTrack
	for _, track := range assetTracks {
		if track.ID == "song-09" {
			act2Track = track
		}
	}
	if len(act2.Splits) != len(splitRegions(act2Track)) {
		t.Errorf("offered %d regions, want %d",
			len(act2.Splits), len(splitRegions(act2Track)))
	}

	// Unticking removes the record the builder created, and its file with it.
	installed := filepath.Join(root, "game-assets", "audio", "song-09-fillmore.ogg")
	writeAssetTestFile(t, installed, fakeVorbisFile())
	response = postAssetForm(t, app, map[string]string{"split-change-song-09": "1"}, nil)
	if response.Code != http.StatusOK {
		t.Fatalf("unsplit status = %d: %s", response.Code, response.Body.String())
	}
	manifest, err = os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(manifest), "[music:song-09-fillmore]") {
		t.Errorf("the split record survived:\n%s", manifest)
	}
	if _, err := os.Stat(installed); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("the split's file survived: %v", err)
	}
	// A record outside the builder's <slot>-<region> namespace is never its
	// business, no matter which regions are ticked.
	if !strings.Contains(string(manifest), "[music:hand-authored]") {
		t.Errorf("a hand-authored record was removed:\n%s", manifest)
	}
}

// Splits are only touched when the panel says so, so an unrelated save cannot
// rewrite records the player set up.
func TestSplitsAreUntouchedWithoutTheirDirtyFlag(t *testing.T) {
	root := t.TempDir()
	const manifest = `[music:song-09]
src = 0E:F69F
file = audio/act2.ogg

[music:song-09-fillmore]
src = 0E:F69F
when = wram[0018]==0x01
file = audio/song-09-fillmore.ogg
`
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(manifest))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	// A title save with no split-change flag and no split values at all.
	response := postAssetForm(t, app,
		map[string]string{"title-change": "1", "title": "on"}, nil)
	if response.Code != http.StatusOK {
		t.Fatalf("save status = %d: %s", response.Code, response.Body.String())
	}
	after, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(after), "[music:song-09-fillmore]") {
		t.Errorf("an unrelated save removed a split:\n%s", after)
	}
}

// The split panel offers the regions the ROM says actually play a song, so a
// player cannot create a gate that could never fire. The lists come from
// `tools/act_content.py --songs` reading the per-map script at $05:8000.
func TestSplitRegionsMatchTheROMsLevelSongMap(t *testing.T) {
	byID := map[string]assetTrack{}
	for _, track := range assetTracks {
		byID[track.ID] = track
	}
	for id, want := range map[string][]trackRegion{
		// The act-2 theme is shared by THREE regions, not the two that are
		// obvious from playing Fillmore and Kasandora -- and in the third it
		// is act ONE, which is why the act belongs in the label.
		"song-09": {{0x01, actTwo}, {0x03, actTwo}, {0x05, actOne}},
		// The boss theme closes act 2 of every kingdom; Death Heim has no acts.
		"song-06": {{0x01, actTwo}, {0x02, actTwo}, {0x03, actTwo}, {0x04, actTwo},
			{0x05, actTwo}, {0x06, actTwo}, {0x07, 0}},
		"song-00": {{0x01, actOne}},
		"song-02": {{0x02, actOne | actTwo}, {0x03, actOne}},
		"song-12": {{0x06, actOne}},
		"song-14": {{0x07, 0}},
	} {
		got := byID[id].Regions
		if len(got) != len(want) {
			t.Errorf("%s regions = %v, want %v", id, got, want)
			continue
		}
		for i := range want {
			if got[i] != want[i] {
				t.Errorf("%s regions = %v, want %v", id, got, want)
				break
			}
		}
	}
	// The act a song covers within a region reaches the label, because that is
	// what a player recognises -- the gate itself stays region-level.
	for id, want := range map[string][]string{
		"song-09": {"Fillmore \u2014 Act 2", "Kasandora \u2014 Act 2", "Marahna \u2014 Act 1"},
		"song-02": {"Bloodpool \u2014 Acts 1 & 2", "Kasandora \u2014 Act 1"},
		"song-05": {"Aitos \u2014 Acts 1 & 2", "Marahna \u2014 Act 2"},
	} {
		regions := splitRegions(byID[id])
		if len(regions) != len(want) {
			t.Errorf("%s offers %d regions, want %d", id, len(regions), len(want))
			continue
		}
		for i, label := range want {
			if regions[i].Label != label {
				t.Errorf("%s region %d label = %q, want %q", id, i, regions[i].Label, label)
			}
		}
	}
	// A region with no acts carries no act suffix.
	for _, region := range splitRegions(byID["song-06"]) {
		if region.Slug == "death-heim" && region.Label != "Death Heim" {
			t.Errorf("Death Heim label = %q; it has no acts", region.Label)
		}
	}
	// A song no action map declares offers no split at all: every region in
	// the panel would be a gate that can never fire.
	for _, id := range []string{"title-theme", "song-01", "song-10", "song-11"} {
		if regions := splitRegions(byID[id]); len(regions) != 0 {
			t.Errorf("%s offers %d regions; it is not an action song", id, len(regions))
		}
	}
	// Nor is a song confined to one region: the only split it could produce
	// covers everywhere it plays, which the ungated slot entry already does.
	for _, id := range []string{"song-00", "song-12", "song-13", "song-14"} {
		if len(byID[id].Regions) != 1 {
			t.Fatalf("%s is no longer a single-region song: %v", id, byID[id].Regions)
		}
		if regions := splitRegions(byID[id]); len(regions) != 0 {
			t.Errorf("%s offers a split across its only region %v", id, regions)
		}
	}
	// Everything actually offered is worth choosing between.
	for _, track := range assetTracks {
		if regions := splitRegions(track); len(regions) == 1 {
			t.Errorf("%s offers a single-region split", track.ID)
		}
	}
	// Every listed region must be a real map group.
	valid := map[byte]bool{}
	for _, region := range assetRegions {
		valid[region.Group] = true
	}
	for _, track := range assetTracks {
		for _, played := range track.Regions {
			if !valid[played.Group] {
				t.Errorf("%s names map group $%02X, which is not an action region",
					track.ID, played.Group)
			}
			if played.Acts&^(actOne|actTwo) != 0 {
				t.Errorf("%s region $%02X has act bits %#b outside act 1 and 2",
					track.ID, played.Group, played.Acts)
			}
			// Only Death Heim has no acts; every kingdom map belongs to one.
			if played.Acts == 0 && played.Group != 0x07 {
				t.Errorf("%s region $%02X claims no act", track.ID, played.Group)
			}
		}
	}
}

// The save path reads the same offer the panel does, so a request naming a
// region for a song that is not split-eligible creates nothing.
func TestSplitSaveRefusesASongWithOneRegion(t *testing.T) {
	root := t.TempDir()
	const manifest = `[music:song-00]
src = 18:947F
file = audio/fillmore.ogg
`
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(manifest))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app, map[string]string{
		"split-change-song-00": "1", "split-song-00": "fillmore",
	}, nil)
	if response.Code != http.StatusOK {
		t.Fatalf("save status = %d: %s", response.Code, response.Body.String())
	}
	after, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != manifest {
		t.Errorf("a split was created for a single-region song:\n%s", after)
	}
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, track := range configuration.Tracks {
		if track.ID == "song-00" && len(track.Splits) != 0 {
			t.Errorf("song-00 still offers splits: %#v", track.Splits)
		}
	}
}

// A track name that claims a PLACE must match where the ROM says the song is
// declared, or it sends a player to the wrong level. Checked against the same
// per-map script `tools/act_content.py --songs` reads.
func TestTrackNamesDoNotClaimTheWrongLevel(t *testing.T) {
	regionNames := map[string]byte{}
	for _, region := range assetRegions {
		regionNames[region.Label] = region.Group
	}
	for _, track := range assetTracks {
		group, claimsAPlace := regionNames[track.Name]
		if !claimsAPlace {
			continue
		}
		// Naming a region means playing there and ONLY there.
		if len(track.Regions) != 1 || track.Regions[0].Group != group {
			t.Errorf("%s is named %q but the ROM declares it in %v",
				track.ID, track.Name, track.Regions)
		}
	}
	// The two that failed that rule are back to their slot numbers.
	byID := map[string]assetTrack{}
	for _, track := range assetTracks {
		byID[track.ID] = track
	}
	for id, name := range map[string]string{
		"song-02": "Track 02",
		"song-09": "Track 09",
		// Identified: a town variant theme, matching its slot $01 placement
		// beside "Birth of the People" in all six towns.
		"song-16": "Sacrifices",
	} {
		if byID[id].Name != name {
			t.Errorf("%s name = %q, want %q", id, byID[id].Name, name)
		}
	}
	// And the one the ROM confirms is kept.
	if byID["song-00"].Name != "Fillmore" {
		t.Errorf("song-00 name = %q; the ROM confirms Fillmore act 1 only",
			byID["song-00"].Name)
	}
}

// The split offer carries the exact record a save will write. The page needs it
// to show the new row the moment a box is ticked -- otherwise the reader has to
// save once to reveal the file picker and again to fill it -- and sending the
// real values means the row it shows is that record, not a guess at one.
func TestSplitOfferDescribesTheRecordItWillWrite(t *testing.T) {
	root := t.TempDir()
	writeAssetTestFile(t, liveAssetManifestPath(root),
		[]byte("[music:song-09]\nsrc = 0E:F69F\nfile = audio/act2.ogg\n"))
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	var offers []assetSplitStatus
	for _, track := range configuration.Tracks {
		if track.ID == "song-09" {
			offers = track.Splits
		}
	}
	want := map[string]assetSplitStatus{
		"fillmore": {Slug: "fillmore", Label: "Fillmore \u2014 Act 2",
			Name: "song-09-fillmore", Gate: "wram[0018]==0x01",
			File: "audio/song-09-fillmore.ogg"},
		"marahna": {Slug: "marahna", Label: "Marahna \u2014 Act 1",
			Name: "song-09-marahna", Gate: "wram[0018]==0x05",
			File: "audio/song-09-marahna.ogg"},
	}
	seen := map[string]bool{}
	for _, offer := range offers {
		expected, ok := want[offer.Slug]
		if !ok {
			continue
		}
		seen[offer.Slug] = true
		if offer != expected {
			t.Errorf("%s offer = %#v, want %#v", offer.Slug, offer, expected)
		}
	}
	if len(seen) != len(want) {
		t.Errorf("offers = %#v; missing some of %v", offers, want)
	}

	// And a save must actually produce what the offer promised.
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app, map[string]string{
		"split-change-song-09": "1", "split-song-09": "marahna",
	}, nil)
	if response.Code != http.StatusOK {
		t.Fatalf("split status = %d: %s", response.Code, response.Body.String())
	}
	manifest, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	promised := want["marahna"]
	for _, line := range []string{
		"[music:" + promised.Name + "]", "when = " + promised.Gate,
		"file = " + promised.File,
	} {
		if !strings.Contains(string(manifest), line) {
			t.Errorf("the save did not write %q:\n%s", line, manifest)
		}
	}
}

// A split and the file for it are created by ONE save: the split loop updates
// the manifest before the variant scan reads it, so an upload named for a
// record that does not exist yet still finds its home.
func TestSplitAndItsFileInstallInOneSave(t *testing.T) {
	root := t.TempDir()
	writeAssetTestFile(t, liveAssetManifestPath(root),
		[]byte("[music:song-09]\nsrc = 0E:F69F\nfile = audio/act2.ogg\n"))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app,
		map[string]string{"split-change-song-09": "1", "split-song-09": "fillmore"},
		map[string][]byte{"variant-song-09-fillmore": fakeVorbisFile()})
	if response.Code != http.StatusOK {
		t.Fatalf("save status = %d: %s", response.Code, response.Body.String())
	}
	installed := filepath.Join(root, "game-assets", "audio", "song-09-fillmore.ogg")
	got, err := os.ReadFile(installed)
	if err != nil {
		t.Fatalf("the file was not installed alongside its new record: %v", err)
	}
	if !bytes.Equal(got, fakeVorbisFile()) {
		t.Error("the installed file is not the uploaded one")
	}
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, track := range configuration.Tracks {
		if track.ID != "song-09" {
			continue
		}
		for _, variant := range track.Variants {
			if variant.Name == "song-09-fillmore" && !variant.Configured {
				t.Error("the new split reports itself unfilled after one save")
			}
		}
	}
}

// A gated variant is a record for the same ROM song under the builder's own
// <slot>-<region> name, so it belongs under that slot -- and it must be visible
// while it is still a stub, since being fillable is the whole point of shipping
// one.
func TestVariantsAreGroupedUnderTheirSlotEvenWhenStubbed(t *testing.T) {
	root := t.TempDir()
	writeAssetTestFile(t, filepath.Join(root, "game-assets", "audio", "song-09-kasandora.ogg"),
		fakeVorbisFile())
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(`[music:song-09-fillmore]
src = 0E:F69F
when = wram[0018]==0x01
file = audio/song-09-fillmore.ogg

[music:song-09-kasandora]
src = 0E:F69F
when = wram[0018]==0x03
file = audio/song-09-kasandora.ogg

[music:song-09]
src = 0E:F69F
file = audio/act2.ogg

[music:song-09-unreachable]
src = 0E:F69F
file = ../../escape.ogg
`))
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(configuration.Tracks) != len(assetTracks) {
		t.Fatalf("variants leaked into the slot list: %d rows", len(configuration.Tracks))
	}
	byID := map[string]assetTrackStatus{}
	for _, track := range configuration.Tracks {
		byID[track.ID] = track
	}
	act2 := byID["song-09"]
	if len(act2.Variants) != 2 {
		t.Fatalf("Act 2 slot has %d variants, want 2: %#v", len(act2.Variants), act2.Variants)
	}
	// Region order, gate text carried through verbatim for display.
	if act2.Variants[0].Name != "song-09-fillmore" ||
		act2.Variants[1].Name != "song-09-kasandora" {
		t.Fatalf("variant order = %q, %q", act2.Variants[0].Name, act2.Variants[1].Name)
	}
	if act2.Variants[0].When != "wram[0018]==0x01" {
		t.Errorf("gate text = %q", act2.Variants[0].When)
	}
	// The stub is listed but reports itself unfilled; the supplied one plays.
	if act2.Variants[0].Configured || act2.Variants[0].URL != "" {
		t.Errorf("a stub reported itself installed: %#v", act2.Variants[0])
	}
	if !act2.Variants[1].Configured ||
		!strings.HasPrefix(act2.Variants[1].URL, "asset-audio/song-09-kasandora?v=") {
		t.Errorf("a supplied variant is not playable: %#v", act2.Variants[1])
	}
	// A path outside game-assets can be neither filled nor served, and
	// "unreachable" is not a region slug anyway.
	for _, variant := range act2.Variants {
		if variant.Name == "song-09-unreachable" {
			t.Error("a record pointing outside game-assets was offered")
		}
	}
}

// The builder shows only what it can also REMOVE. A record under any other name
// is one it will not edit, and listing an entry a player cannot get rid of from
// here sends them to the manifest by hand -- worse than not listing it.
func TestHandAuthoredVariantsAreNeitherShownNorTouched(t *testing.T) {
	root := t.TempDir()
	own := filepath.Join(root, "game-assets", "audio", "kassandora-act2.ogg")
	writeAssetTestFile(t, own, fakeVorbisFile())
	const manifest = `[music:kassandora-act2]
src = 0E:F69F
when = wram[0018]==0x03
file = audio/kassandora-act2.ogg

[music:song-09]
src = 0E:F69F
file = audio/act2.ogg
`
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(manifest))
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, track := range configuration.Tracks {
		for _, variant := range track.Variants {
			if variant.Name == "kassandora-act2" {
				t.Fatalf("a record the builder cannot remove was listed under %s", track.ID)
			}
		}
	}
	// Nor can a crafted request reach it: not through the variant path...
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	if response := postAssetForm(t, app,
		map[string]string{"variant-remove-kassandora-act2": "1"},
		map[string][]byte{"variant-kassandora-act2": fakeVorbisFile()}); response.Code != http.StatusOK {
		t.Fatalf("save status = %d: %s", response.Code, response.Body.String())
	}
	if _, err := os.Stat(own); err != nil {
		t.Errorf("the hand-authored file was touched: %v", err)
	}
	after, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != manifest {
		t.Errorf("the manifest was rewritten:\n%s", after)
	}
	// ...nor through the playback route, which now answers only for names the
	// builder itself manages.
	response := httptest.NewRecorder()
	app.ServeHTTP(response,
		httptest.NewRequest(http.MethodGet, "/tok/asset-audio/kassandora-act2", nil))
	if response.Code != http.StatusNotFound {
		t.Errorf("asset-audio served an unmanaged record: %d", response.Code)
	}
}

// Filling a variant places the file where the record ALREADY points and leaves
// the manifest untouched -- the gate is the builder's own, but rewriting a
// record on every file change would churn the file for nothing.
func TestVariantSaveWritesTheFileAndNeverTheManifest(t *testing.T) {
	root := t.TempDir()
	const manifest = `[music:song-09-fillmore]
src = 0E:F69F
when = wram[0018]==0x01
gain = 80
file = audio/song-09-fillmore.ogg

[music:song-09]
src = 0E:F69F
file = audio/act2.ogg
`
	writeAssetTestFile(t, liveAssetManifestPath(root), []byte(manifest))
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")

	response := postAssetForm(t, app, nil,
		map[string][]byte{"variant-song-09-fillmore": fakeVorbisFile()})
	if response.Code != http.StatusOK {
		t.Fatalf("variant save status = %d: %s", response.Code, response.Body.String())
	}
	installed := filepath.Join(root, "game-assets", "audio", "song-09-fillmore.ogg")
	got, err := os.ReadFile(installed)
	if err != nil {
		t.Fatalf("the variant file was not installed: %v", err)
	}
	if !bytes.Equal(got, fakeVorbisFile()) {
		t.Error("the installed variant is not the uploaded file")
	}
	after, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != manifest {
		t.Errorf("a variant save rewrote the manifest:\n%s", after)
	}

	// Reverting removes the file and leaves the record, so the stub survives
	// to be filled again; unticking the region is what removes the record.
	response = postAssetForm(t, app,
		map[string]string{"variant-remove-song-09-fillmore": "1"}, nil)
	if response.Code != http.StatusOK {
		t.Fatalf("variant revert status = %d: %s", response.Code, response.Body.String())
	}
	if _, err := os.Stat(installed); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("the variant file survived its revert: %v", err)
	}
	after, err = os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != manifest {
		t.Errorf("a variant revert rewrote the manifest:\n%s", after)
	}
}

// Whatever the builder lists, it must be able to remove. This is the invariant
// behind restricting variants to the <slot>-<region> namespace: the trap it
// replaces was a record shown in the UI whose only removal path was editing the
// manifest by hand.
func TestEverythingListedIsAlsoRemovable(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")

	// Create one split for every region every song can split into.
	fields := map[string]string{}
	var created []string
	for _, track := range assetTracks {
		regions := splitRegions(track)
		if len(regions) == 0 {
			continue
		}
		fields["split-change-"+track.ID] = "1"
		for _, region := range regions {
			created = append(created, splitSectionName(track.ID, region.Slug))
		}
	}
	form := url.Values{}
	for key, value := range fields {
		form.Set(key, value)
	}
	for _, track := range assetTracks {
		for _, region := range splitRegions(track) {
			form.Add("split-"+track.ID, region.Slug)
		}
	}
	if response := postAssetValues(t, app, form); response.Code != http.StatusOK {
		t.Fatalf("create status = %d: %s", response.Code, response.Body.String())
	}
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	listed := map[string]bool{}
	for _, track := range configuration.Tracks {
		for _, variant := range track.Variants {
			listed[variant.Name] = true
		}
	}
	for _, name := range created {
		if !listed[name] {
			t.Errorf("%s was created but is not listed", name)
		}
	}
	if len(listed) != len(created) {
		t.Errorf("listed %d variants, created %d", len(listed), len(created))
	}

	// Now untick everything: every listed record must go.
	clear := url.Values{}
	for key := range fields {
		clear.Set(key, "1")
	}
	if response := postAssetValues(t, app, clear); response.Code != http.StatusOK {
		t.Fatalf("remove status = %d: %s", response.Code, response.Body.String())
	}
	configuration, err = loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, track := range configuration.Tracks {
		if len(track.Variants) != 0 {
			t.Errorf("%s still lists %#v after unticking every region",
				track.ID, track.Variants)
		}
	}
	manifest, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	for _, name := range created {
		if strings.Contains(string(manifest), "[music:"+name+"]") {
			t.Errorf("%s survived removal", name)
		}
	}
}

// One file per record, overwritten in place. The scheme this replaced wrote
// audio/builder/<id>-<content-hash>, so every re-upload created a NEW file and
// left the previous one behind with nothing to ever collect it -- a slot
// revised a few times quietly accumulated copies of a multi-megabyte track.
func TestRepeatedUploadsDoNotAccumulateFiles(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	audio := filepath.Join(root, "game-assets", "audio")

	for revision := 0; revision < 4; revision++ {
		content := append(fakeVorbisFile(), byte(revision))
		if response := postAssetForm(t, app, nil,
			map[string][]byte{"track-song-00": content}); response.Code != http.StatusOK {
			t.Fatalf("save %d = %d: %s", revision, response.Code, response.Body.String())
		}
		entries, err := os.ReadDir(audio)
		if err != nil {
			t.Fatal(err)
		}
		var files []string
		for _, entry := range entries {
			files = append(files, entry.Name())
		}
		if len(files) != 1 {
			t.Fatalf("after %d uploads the audio folder holds %v", revision+1, files)
		}
		// And it is always the LATEST upload, not a stale first copy.
		got, err := os.ReadFile(filepath.Join(audio, files[0]))
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(got, content) {
			t.Errorf("upload %d did not overwrite in place", revision)
		}
	}
	// Nothing hides in a subfolder either.
	if _, err := os.Stat(filepath.Join(audio, "builder")); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("a builder subfolder was created: %v", err)
	}
	// The record still names the flat path the template declared, so the
	// hand-managed "drop a file with this name" route keeps working.
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, track := range configuration.Tracks {
		if track.ID == "song-00" && track.File != templateAudioPaths["song-00"] {
			t.Errorf("record file = %q, want the template's %q",
				track.File, templateAudioPaths["song-00"])
		}
	}
}
