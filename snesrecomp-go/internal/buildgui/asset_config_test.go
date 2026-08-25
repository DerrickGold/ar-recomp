package buildgui

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
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

	musicRelative, ok := manifestSectionValue(updated, "music:song-00", "file")
	if !ok || !strings.HasPrefix(musicRelative, "audio/builder/song-00-") {
		t.Fatalf("saved music path = %q", musicRelative)
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
		!strings.Contains(string(live), "file = audio/builder/title-theme-") {
		t.Fatalf("live manifest was not seeded and updated:\n%s", live)
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
