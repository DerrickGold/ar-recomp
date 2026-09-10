package buildgui

import (
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func sceneryGet(app *application, path string) *httptest.ResponseRecorder {
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/secret/"+path, nil))
	return w
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
