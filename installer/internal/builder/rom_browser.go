package builder

import (
	"fmt"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"runtime"

	"github.com/DerrickGold/ar-recomp/installer/internal/romset"
)

type romLocation struct {
	ID    string `json:"id"`
	Label string `json:"label,omitempty"`
	path  string
}

func (app *application) romLocations() []romLocation {
	home, _ := os.UserHomeDir()
	locations := []romLocation{
		{ID: "game", path: app.options.ProjectRoot},
		{ID: "builder", path: app.options.ImportSearchDir},
	}
	if home != "" {
		locations = append(locations, romLocation{ID: "downloads", path: filepath.Join(home, "Downloads")}, romLocation{ID: "home", path: home})
	}
	switch runtime.GOOS {
	case "linux":
		if home != "" {
			locations = append(locations, romLocation{ID: "removable", path: filepath.Join("/run/media", filepath.Base(home))})
		}
		locations = append(locations, romLocation{ID: "media", path: "/media"})
	case "darwin":
		locations = append(locations, romLocation{ID: "removable", path: "/Volumes"})
	case "windows":
		locations = append(locations, romDriveLocations()...)
	}
	return locations
}

func (app *application) openROMRoot(id string) (*os.Root, error) {
	for _, location := range app.romLocations() {
		if id == location.ID && location.path != "" {
			return os.OpenRoot(location.path)
		}
	}
	return nil, fmt.Errorf("choose a ROM location")
}

func (app *application) browseROMs(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("location")
	if id == "" {
		locations := []romLocation{}
		for _, location := range app.romLocations() {
			if location.Label != "" {
				locations = append(locations, location)
				continue
			}
			if info, err := os.Stat(location.path); err == nil && info.IsDir() {
				locations = append(locations, location)
			}
		}
		writeJSON(w, http.StatusOK, map[string]any{"locations": locations})
		return
	}
	folder := r.URL.Query().Get("path")
	if folder == "" {
		folder = "."
	}
	if !fs.ValidPath(folder) {
		writeJSONError(w, http.StatusBadRequest, "invalid ROM folder")
		return
	}
	root, err := app.openROMRoot(id)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	defer root.Close()
	entries, err := fs.ReadDir(root.FS(), folder)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	type item struct {
		Name      string `json:"name"`
		Directory bool   `json:"directory"`
	}
	folders := []item{}
	files := []item{}
	for _, entry := range entries {
		name := entry.Name()
		if name[0] == '.' {
			continue
		}
		if entry.IsDir() {
			folders = append(folders, item{Name: name, Directory: true})
		} else if entry.Type().IsRegular() && validROMName(name) {
			files = append(files, item{Name: name})
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"entries": append(folders, files...)})
}

func (app *application) readBrowsedROM(id, selected string) (romset.Input, error) {
	path, err := filepath.Localize(selected)
	if err != nil || !validROMName(path) {
		return romset.Input{}, fmt.Errorf("select a .sfc or .smc ROM")
	}
	root, err := app.openROMRoot(id)
	if err != nil {
		return romset.Input{}, err
	}
	defer root.Close()
	return readROMInRoot(root, path)
}

func readROMInRoot(root *os.Root, path string) (romset.Input, error) {
	if info, err := root.Lstat(path); err != nil || !info.Mode().IsRegular() {
		return romset.Input{}, fmt.Errorf("the selected ROM is missing or unreadable")
	}
	file, err := root.Open(path)
	if err != nil {
		return romset.Input{}, err
	}
	defer file.Close()
	return readRegularROM(filepath.Base(path), file)
}
