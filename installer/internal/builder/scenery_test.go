package builder

import (
	"bytes"
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
)

func sceneryGet(app *application, path string) *httptest.ResponseRecorder {
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/secret/"+path, nil))
	return w
}

func TestImportedMasterSceneryRefreshesWithoutSourceROM(t *testing.T) {
	path := os.Getenv("AR2_MASTER_ROM")
	if path == "" {
		t.Skip("optional local ActRaiser 2 ROM")
	}
	rom, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	app := localizationTestApp(t)
	if err := os.MkdirAll(app.options.ProjectRoot, 0755); err != nil {
		t.Fatal(err)
	}
	read := func() string {
		t.Helper()
		var status struct {
			Available bool `json:"available"`
			Imports   map[string]struct {
				URL string `json:"atlasURL"`
			} `json:"imports"`
		}
		w := sceneryGet(app, "scene-assets")
		if err := json.Unmarshal(w.Body.Bytes(), &status); err != nil || w.Code != 200 || status.Available {
			t.Fatal("AR2 art must work without an AR1 ROM/cache", w.Code, err)
		}
		return status.Imports["ar2-master"].URL
	}
	if read() != "" {
		t.Fatal("uninstalled scenery exposed")
	}
	if w := uploadGameContent(t, app, "/secret/game-imports", rom); w.Code != 200 {
		t.Fatal(w.Body.String())
	}
	url := read()
	if url == "" {
		t.Fatal("successful import did not unlock scenery immediately")
	}
	atlas := sceneryGet(app, url)
	if atlas.Code != 200 || atlas.Header().Get("Content-Type") != "image/png" {
		t.Fatal("missing character atlas")
	}
	if second := sceneryGet(app, url); !bytes.Equal(second.Body.Bytes(), atlas.Body.Bytes()) {
		t.Fatal("cached image changed")
	}
	for _, bad := range []string{"scene-atlas.png?content=ar2-master&v=old", "scene-atlas.png?content=../../user-rom.sfc", "scene-assets?content=ar2-master"} {
		if w := sceneryGet(app, bad); w.Code != 404 {
			t.Fatal("unrecognized asset route accepted", bad)
		}
	}
	pack := filepath.Join(app.options.ProjectRoot, "game-assets", "imports", ar2master.Filename)
	if err := os.WriteFile(pack, []byte("truncated"), 0600); err != nil {
		t.Fatal(err)
	}
	if read() != "" || sceneryGet(app, url).Code != 404 {
		t.Fatal("damaged pack retained its theme")
	}
	if w := uploadGameContent(t, app, "/secret/game-imports", rom); w.Code != 200 {
		t.Fatal(w.Body.String())
	}
	if read() != url {
		t.Fatal("repaired content failed to restore its theme")
	}
	if err := os.Remove(pack); err != nil {
		t.Fatal(err)
	}
	if read() != "" {
		t.Fatal("removed pack retained its theme")
	}
}

func TestSceneryFallbackAndRouteConfinement(t *testing.T) {
	app := localizationTestApp(t)
	w := sceneryGet(app, "scene-assets")
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"available":false`) {
		t.Fatal(w.Code, w.Body.String())
	}
	for _, path := range []string{"scene-atlas.png", "scene-atlas.png?v=wrong", "scene-atlas.png/../../user-rom.sfc"} {
		if w := sceneryGet(app, path); w.Code != 404 {
			t.Fatal(path, w.Code)
		}
	}
	if app.localization.current != nil || app.state != "idle" {
		t.Fatal("scenery changed tool/game state")
	}
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() { defer wg.Done(); sceneryGet(app, "scene-assets") }()
	}
	wg.Wait()
}

func TestRetailSceneryCacheSurvivesWithoutROM(t *testing.T) {
	path := os.Getenv("AR_WORKSHOP_TEST_ROM")
	if path == "" {
		t.Skip("optional local US ROM")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	app := localizationTestApp(t)
	rom := filepath.Join(app.options.ProjectRoot, "user-rom.sfc")
	if err := os.MkdirAll(app.options.ProjectRoot, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(rom, data, 0644); err != nil {
		t.Fatal(err)
	}
	w := sceneryGet(app, "scene-assets")
	var status struct {
		Available bool   `json:"available"`
		URL       string `json:"atlasURL"`
		Message   string `json:"message"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &status); err != nil || !status.Available {
		t.Fatal(w.Body.String(), err)
	}
	image := sceneryGet(app, status.URL)
	if image.Code != 200 || image.Header().Get("Content-Type") != "image/png" {
		t.Fatal(image.Code, image.Body.String())
	}
	// v3 adds airborne Master poses, two ground enemies, and both centaur
	// facings. Keep the one-time response bounded as the collection grows.
	if image.Body.Len() > 160*1024 {
		t.Fatal("unexpected atlas size", image.Body.Len())
	}
	if err := os.Rename(rom, rom+".held"); err != nil {
		t.Fatal(err)
	}
	app.scenery = scenerySession{}
	second := sceneryGet(app, "scene-assets")
	if !strings.Contains(second.Body.String(), `"available":true`) || !strings.Contains(second.Body.String(), "Original game art") {
		t.Fatal(second.Body.String())
	}
	if got := sceneryGet(app, "scene-assets"); got.Body.String() != second.Body.String() {
		t.Fatal("cached status changed")
	}
	if got := sceneryGet(app, status.URL); got.Body.String() != image.Body.String() {
		t.Fatal("cached pixels changed")
	}
	// A broken generated cache cannot take down the workshop when no ROM exists.
	cache := filepath.Join(app.options.ProjectRoot, "game-assets", "workshop", "us-scene-v3.json")
	if err := os.WriteFile(cache, []byte("truncated"), 0644); err != nil {
		t.Fatal(err)
	}
	app.scenery = scenerySession{}
	if got := sceneryGet(app, "scene-assets"); got.Code != 200 || !strings.Contains(got.Body.String(), `"available":false`) {
		t.Fatal(got.Body.String())
	}
	if err := os.Rename(rom+".held", rom); err != nil {
		t.Fatal(err)
	}
	app.scenery = scenerySession{}
	if got := sceneryGet(app, "scene-assets"); !strings.Contains(got.Body.String(), `"available":true`) {
		t.Fatal("cache not repaired", got.Body.String())
	}
}
