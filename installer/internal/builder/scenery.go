package builder

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/workshopart"
)

type scenerySession struct {
	mu      sync.Mutex
	assets  *workshopart.Assets
	checked time.Time
	message string
}

// Scenery is independent of build/localization locks, selection, saves, and
// runtime assets. Failure here never fails a build or hides a workspace.
func (app *application) loadScenery() (*workshopart.Assets, string) {
	s := &app.scenery
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.assets != nil || time.Since(s.checked) < time.Second {
		return s.assets, s.message
	}
	s.checked = time.Now()
	s.message = "Illustrated scenery · build with your US ROM to add original game art."
	cache, cacheErr := workshopart.OpenCacheRoot(app.options.ProjectRoot)
	if cacheErr == nil {
		defer cache.Close()
		if saved, err := workshopart.ReadCache(cache); err == nil {
			s.assets = saved
			s.message = "Original game art · extracted locally from your US ROM."
			return saved, s.message
		}
	}
	// The same installed-ROM discovery used by audio previews understands both
	// source roots and packaged <bundle>/utils roots. Never guess a regional ROM.
	path := findWorkshopROM(app.options.ProjectRoot)
	if path == "" {
		return nil, s.message
	}
	f, err := os.Open(path)
	if err != nil {
		s.message = "Illustrated scenery · the local ROM could not be read."
		return nil, s.message
	}
	data, err := io.ReadAll(io.LimitReader(f, (1<<20)+1))
	f.Close()
	if err != nil {
		s.message = "Illustrated scenery · the local ROM could not be read."
		return nil, s.message
	}
	assets, err := workshopart.Extract(data)
	if err != nil {
		s.message = "Illustrated scenery · original art needs a clean, headerless US ROM."
		return nil, s.message
	}
	s.assets = assets
	s.message = "Original game art · extracted locally from your US ROM."
	if cacheErr != nil || workshopart.WriteCache(cache, assets) != nil {
		// Still useful in read-only installations; keep the complete small atlas
		// in memory for this session instead of making cache writes mandatory.
		s.message += " Cache unavailable; artwork is kept for this session only."
	}
	return s.assets, s.message
}

func (app *application) serveScenery(w http.ResponseWriter, r *http.Request, atlas bool) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	a, message := app.loadScenery()
	if !atlas {
		w.Header().Set("Cache-Control", "no-store")
		result := map[string]any{"available": a != nil, "message": message}
		if a != nil {
			result["catalog"] = a.Catalog
			result["atlasURL"] = fmt.Sprintf("scene-atlas.png?v=%d-%s", workshopart.Version, a.Catalog.ImageSHA256)
		}
		writeJSON(w, 200, result)
		return
	}
	if a == nil {
		http.NotFound(w, r)
		return
	}
	if r.URL.Query().Get("v") != fmt.Sprintf("%d-%s", workshopart.Version, a.Catalog.ImageSHA256) {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "private, max-age=31536000, immutable")
	http.ServeContent(w, r, "scene-atlas.png", time.Time{}, bytes.NewReader(a.PNG))
}
