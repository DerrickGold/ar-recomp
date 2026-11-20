package builder

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/romset"
)

type romLocation struct {
	ID    string `json:"id"`
	Label string `json:"label,omitempty"`
	Drive bool   `json:"drive,omitempty"`
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
		locations = append(locations, romLocation{ID: "media", path: "/media"}, romLocation{ID: "system", Drive: true, path: "/"})
	case "darwin":
		locations = append(locations, romLocation{ID: "removable", path: "/Volumes"}, romLocation{ID: "system", Drive: true, path: "/"})
	case "windows":
		locations = append(locations, romDriveLocations()...)
	}
	for _, location := range locations {
		if location.ID != "media" && location.ID != "removable" {
			continue
		}
		entries, _ := os.ReadDir(location.path)
		for _, entry := range entries {
			if !entry.IsDir() || strings.HasPrefix(entry.Name(), ".") {
				continue
			}
			mount := filepath.Join(location.path, entry.Name())
			if runtime.GOOS == "linux" && location.ID == "media" && entry.Name() == filepath.Base(home) {
				children, _ := os.ReadDir(mount)
				for _, child := range children {
					if child.IsDir() && !strings.HasPrefix(child.Name(), ".") {
						locations = append(locations, romVolume(filepath.Join(mount, child.Name())))
					}
				}
				continue
			}
			locations = append(locations, romVolume(mount))
		}
	}
	return locations
}

func romVolume(mount string) romLocation {
	id := sha256.Sum256([]byte(mount))
	return romLocation{ID: fmt.Sprintf("volume-%x", id[:12]), Label: filepath.Base(mount), Drive: true, path: mount}
}

type browserFolder struct {
	Location string `json:"location"`
	Path     string `json:"path"`
}

func (app *application) browserPreferencesPath() string {
	return filepath.Join(filepath.Dir(app.interfacePreferencesPath()), "workshop-browser.json")
}

// Call with interfaceMu held so a ROM selection and a mod installation can
// both update their own entry without losing the other picker's directory.
func (app *application) readBrowserFolders() map[string]browserFolder {
	folders := make(map[string]browserFolder)
	file, err := os.Open(app.browserPreferencesPath())
	if err != nil {
		return folders
	}
	defer file.Close()
	if json.NewDecoder(io.LimitReader(file, 64<<10)).Decode(&folders) != nil || folders == nil {
		return make(map[string]browserFolder)
	}
	return folders
}

func (app *application) rememberBrowserFolder(kind, location, selected string) {
	app.interfaceMu.Lock()
	defer app.interfaceMu.Unlock()
	folders := app.readBrowserFolders()
	folder := browserFolder{Location: location, Path: path.Dir(selected)}
	if folders[kind] == folder {
		return
	}
	folders[kind] = folder
	filename := app.browserPreferencesPath()
	if err := os.MkdirAll(filepath.Dir(filename), 0755); err == nil {
		data, _ := json.Marshal(folders)
		_ = writeAtomicFile(filename, data)
	}
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
	app.browseLocalFiles(w, r, "ROM", validROMName)
}

// Aliases such as Downloads and the game folder are starting points. At the
// edge of an alias continue on the containing drive using a root-relative path.
func browserParent(id, relative, directory string) *browserFolder {
	if relative != "." {
		return &browserFolder{Location: id, Path: path.Dir(relative)}
	}
	directory, err := filepath.Abs(directory)
	if err != nil {
		return nil
	}
	if resolved, err := filepath.EvalSymlinks(directory); err == nil {
		directory = resolved
	}
	parent := filepath.Dir(directory)
	if parent == directory {
		return nil // The parent of a drive is the file picker's Computer view.
	}
	root, drive := string(filepath.Separator), "system"
	if volume := filepath.VolumeName(directory); volume != "" {
		root, drive = volume+string(filepath.Separator), strings.ToUpper(volume)
	}
	relative, err = filepath.Rel(root, parent)
	if err != nil || !filepath.IsLocal(relative) {
		return nil
	}
	return &browserFolder{Location: drive, Path: filepath.ToSlash(relative)}
}

func (app *application) browseLocalFiles(w http.ResponseWriter, r *http.Request, kind string, matchesFile func(string) bool) {
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
		app.interfaceMu.Lock()
		recent := app.readBrowserFolders()[kind]
		app.interfaceMu.Unlock()
		writeJSON(w, http.StatusOK, map[string]any{"locations": locations, "recent": recent})
		return
	}
	folder := r.URL.Query().Get("path")
	if folder == "" {
		folder = "."
	}
	if !fs.ValidPath(folder) {
		writeJSONError(w, http.StatusBadRequest, "invalid "+kind+" folder")
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
		} else if entry.Type().IsRegular() && matchesFile(name) {
			files = append(files, item{Name: name})
		}
	}
	directory := filepath.Join(root.Name(), filepath.FromSlash(folder))
	writeJSON(w, http.StatusOK, map[string]any{
		"entries": append(folders, files...),
		"folder":  directory,
		"name":    filepath.Base(directory),
		"parent":  browserParent(id, folder, root.Name()),
	})
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
