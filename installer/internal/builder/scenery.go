package builder

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"io"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
	"github.com/DerrickGold/ar-recomp/installer/internal/workshopart"
)

type scenerySession struct {
	mu           sync.Mutex
	assets       *workshopart.Assets
	checked      time.Time
	message      string
	master       *workshopart.Assets
	masterDigest [32]byte
}

// Read the bounded installed pack on metadata requests, not animation frames.
// Reuse the rendered atlas until its content changes; removal/corruption also
// removes the theme. The existing AR1 scenery cache is independent.
func (app *application) loadMasterScenery() *workshopart.Assets {
	s := &app.scenery
	s.mu.Lock()
	defer s.mu.Unlock()
	pack, err := ar2master.Read(app.options.ProjectRoot)
	if err != nil {
		s.master = nil
		return nil
	}
	digest := sha256.Sum256(pack)
	if s.master == nil || digest != s.masterDigest {
		s.master, _ = workshopart.ExtractAR2Master(pack)
		s.masterDigest = digest
	}
	return s.master
}

func sceneryVersion(a *workshopart.Assets) string {
	return fmt.Sprintf("%d-%s", a.Catalog.Version, a.Catalog.ImageSHA256)
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
	var a *workshopart.Assets
	var message string
	switch r.URL.Query().Get("content") {
	case "":
		a, message = app.loadScenery()
	case "ar2-master":
		if !atlas {
			http.NotFound(w, r)
			return
		}
		a = app.loadMasterScenery()
	default:
		http.NotFound(w, r)
		return
	}
	if !atlas {
		w.Header().Set("Cache-Control", "no-store")
		result := map[string]any{"available": a != nil, "message": message}
		if a != nil {
			result["catalog"] = a.Catalog
			result["atlasURL"] = "scene-atlas.png?v=" + sceneryVersion(a)
		}
		if master := app.loadMasterScenery(); master != nil {
			result["imports"] = map[string]any{"ar2-master": map[string]any{
				"catalog":  master.Catalog,
				"atlasURL": "scene-atlas.png?content=ar2-master&v=" + sceneryVersion(master),
			}}
		}
		writeJSON(w, 200, result)
		return
	}
	if a == nil {
		http.NotFound(w, r)
		return
	}
	if r.URL.Query().Get("v") != sceneryVersion(a) {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "private, max-age=31536000, immutable")
	http.ServeContent(w, r, "scene-atlas.png", time.Time{}, bytes.NewReader(a.PNG))
}
