package buildgui

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestAudioPreviewStatusRequiresSuppliedROM(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := httptest.NewRecorder()
	app.ServeHTTP(response,
		httptest.NewRequest(http.MethodGet, "/tok/audio-previews", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("status = %d", response.Code)
	}
	var status audioPreviewStatus
	if err := json.NewDecoder(response.Body).Decode(&status); err != nil {
		t.Fatal(err)
	}
	if status.ROMAvailable {
		t.Fatal("preview extraction should be unavailable without a supplied ROM")
	}
	if len(status.Tracks) != len(assetTracks) {
		t.Fatalf("status has %d tracks, want %d", len(status.Tracks), len(assetTracks))
	}

	response = httptest.NewRecorder()
	app.ServeHTTP(response,
		httptest.NewRequest(http.MethodPost, "/tok/audio-previews", nil))
	if response.Code != http.StatusConflict ||
		!strings.Contains(response.Body.String(), "Build tab") {
		t.Fatalf("unexpected start response: %d %s", response.Code, response.Body.String())
	}
}

func TestAudioPreviewROMDiscoveryChecksBundleAndUtils(t *testing.T) {
	bundle := t.TempDir()
	utils := filepath.Join(bundle, "utils")
	if err := os.Mkdir(utils, 0o755); err != nil {
		t.Fatal(err)
	}
	outer := filepath.Join(bundle, "game.sfc")
	if err := os.WriteFile(outer, []byte("rom"), 0o600); err != nil {
		t.Fatal(err)
	}
	if got := findAudioPreviewROM(utils); got != outer {
		t.Fatalf("outer ROM = %q, want %q", got, outer)
	}
	inner := filepath.Join(utils, "user-rom.sfc")
	if err := os.WriteFile(inner, []byte("new rom"), 0o600); err != nil {
		t.Fatal(err)
	}
	if got := findAudioPreviewROM(utils); got != inner {
		t.Fatalf("inner ROM = %q, want %q", got, inner)
	}
}

func TestAudioPreviewServesOnlyGeneratedTrackPaths(t *testing.T) {
	root := t.TempDir()
	cache := t.TempDir()
	path := filepath.Join(cache, "title-theme.wav")
	content := []byte("RIFF\x00\x00\x00\x00WAVEpreview")
	if err := os.WriteFile(path, content, 0o600); err != nil {
		t.Fatal(err)
	}
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	app.previewMu.Lock()
	app.previewPaths["title-theme"] = path
	app.preview.State = "ready"
	app.preview.Fingerprint = "abc123"
	app.previewMu.Unlock()

	response := httptest.NewRecorder()
	app.ServeHTTP(response,
		httptest.NewRequest(http.MethodGet, "/tok/audio-preview/title-theme.wav", nil))
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "audio/wav" {
		t.Fatalf("preview response = %d %s", response.Code, response.Header().Get("Content-Type"))
	}
	if response.Body.String() != string(content) {
		t.Fatalf("served content = %q", response.Body.String())
	}

	response = httptest.NewRecorder()
	app.ServeHTTP(response,
		httptest.NewRequest(http.MethodGet, "/tok/audio-preview/song-00.wav", nil))
	if response.Code != http.StatusNotFound {
		t.Fatalf("unregistered preview status = %d", response.Code)
	}
}

func TestAudioPreviewCacheIsOutsideGameAssets(t *testing.T) {
	root := t.TempDir()
	cache := filepath.Join(t.TempDir(), "cache")
	resolved, err := audioPreviewCacheRoot(Options{
		ProjectRoot: root, AudioPreviewCacheDir: cache,
	})
	if err != nil {
		t.Fatal(err)
	}
	if resolved != cache {
		t.Fatalf("cache = %q, want %q", resolved, cache)
	}
	if strings.HasPrefix(resolved, filepath.Join(root, "game-assets")) {
		t.Fatalf("ROM-derived previews must not enter game-assets: %s", resolved)
	}
}
