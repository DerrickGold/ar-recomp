package builder

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2game"
	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
	"github.com/DerrickGold/ar-recomp/installer/internal/romset"
)

type romChoice struct {
	name, launchPath, location, path string
	uploaded                         []byte
}

func (choice romChoice) preferSavedCore(saved string) bool {
	return saved != "" && choice.launchPath != ""
}

func (app *application) selectUploadedROMs(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxROMBytes+(1<<20))
	if err := r.ParseMultipartForm(maxROMBytes + (1 << 20)); err != nil {
		writeJSONError(w, http.StatusBadRequest, "could not read the selected ROMs")
		return
	}
	defer r.MultipartForm.RemoveAll()
	files := r.MultipartForm.File["rom"]
	slot := firstFormValue(r.MultipartForm, "id")
	if len(files) == 0 || (slot != "" && len(files) != 1) {
		writeJSONError(w, http.StatusBadRequest, "choose a ROM from the checklist")
		return
	}
	if len(files) > len(romset.SupportedReleases()) {
		writeJSONError(w, http.StatusBadRequest, "select up to six supported ROMs")
		return
	}
	choices := make(map[string]romChoice)
	unavailable := []string{}
	for _, file := range files {
		input, err := readUploadedROM(file)
		if err != nil {
			unavailable = append(unavailable, file.Filename)
			continue
		}
		id, _, err := romset.Identify(input.Data)
		if err != nil {
			unavailable = append(unavailable, input.Name)
			continue
		}
		if slot != "" && slot != id {
			writeJSONError(w, http.StatusBadRequest, "the selected ROM belongs to a different entry in the checklist")
			return
		}
		if _, exists := choices[id]; !exists {
			choices[id] = romChoice{name: input.Name, uploaded: input.Data}
		}
	}
	if app.updateROMChoices(w, choices, "") {
		writeJSON(w, http.StatusOK, map[string]any{"unavailable": unavailable})
	}
}

func (app *application) initializeROMChoices() {
	app.romChoices = make(map[string]romChoice)
	for _, path := range app.options.LaunchROMs {
		name := filepath.Base(path)
		input, err := readLaunchROM(path)
		if err != nil {
			app.romIssues = append(app.romIssues, name)
			continue
		}
		id, _, err := romset.Identify(input.Data)
		if err != nil {
			app.romIssues = append(app.romIssues, name)
			continue
		}
		if _, exists := app.romChoices[id]; !exists {
			app.romChoices[id] = romChoice{name: name, launchPath: path}
		}
	}
}

func (app *application) foundROMs() map[string]romChoice {
	found := make(map[string]romChoice)
	for _, location := range []string{"game", "builder"} {
		root, err := app.openROMRoot(location)
		if err != nil {
			continue
		}
		names, err := fs.ReadDir(root.FS(), ".")
		if err != nil {
			root.Close()
			continue
		}
		for _, entry := range names {
			name := entry.Name()
			if !entry.Type().IsRegular() || strings.HasPrefix(name, ".") || !validROMName(name) {
				continue
			}
			if location == "game" && (name == "user-rom.sfc" || name == "game.sfc") {
				continue
			}
			input, err := readROMInRoot(root, name)
			if err != nil {
				continue
			}
			id, _, err := romset.Identify(input.Data)
			if _, exists := found[id]; err == nil && !exists {
				found[id] = romChoice{name: name, location: location, path: name}
			}
		}
		root.Close()
	}
	return found
}

func (app *application) serveROMChecklist(w http.ResponseWriter) {
	type row struct {
		ID            string `json:"id"`
		SHA256        string `json:"sha256"`
		Selected      string `json:"selected,omitempty"`
		Found         string `json:"found,omitempty"`
		Origin        string `json:"origin,omitempty"`
		Media         string `json:"media,omitempty"`
		Master        string `json:"master,omitempty"`
		World         string `json:"world,omitempty"`
		Saved         bool   `json:"saved,omitempty"`
		TextReference bool   `json:"textReference,omitempty"`
		LanguagePack  bool   `json:"languagePack,omitempty"`
	}
	found := app.foundROMs()
	available := make(map[string]string)
	if donors, err := regionalmedia.ListInstalled(app.options.ProjectRoot); err == nil {
		for _, donor := range donors {
			available[donor.Release] = donor.Status
		}
	}
	_, savedErr := reusableBuildROM(app.options.ProjectRoot)
	choices := app.snapshotROMChoices()
	rows := []row{}
	for _, release := range romset.SupportedReleases() {
		id := release.ID
		selection := choices[id]
		item := row{ID: id, SHA256: release.SHA256, Selected: selection.name, Found: found[id].name, Media: available[id]}
		if selection.launchPath != "" {
			item.Origin = "launcher"
		} else if selection.name != "" {
			item.Origin = "browser"
		}
		switch id {
		case "us":
			item.Saved = savedErr == nil
			item.LanguagePack = regularFileExists(nativeUSModPath(app.options.ProjectRoot))
		case romset.SequelID:
			item.Master = ar2master.Status(app.options.ProjectRoot)
			item.World = ar2game.Status(app.options.ProjectRoot)
		default:
			item.LanguagePack = regularFileExists(regionalROMModPath(app.options.ProjectRoot, id))
			path := filepath.Join(app.options.ProjectRoot, "game-assets", "languages", "projects", "source."+id+".arproject")
			if info, err := os.Lstat(path); err == nil {
				item.TextReference = info.Mode().IsRegular()
			}
		}
		rows = append(rows, item)
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, map[string]any{"roms": rows, "unavailable": app.romIssues})
}

func (app *application) selectChecklistROM(w http.ResponseWriter, r *http.Request) {
	contentType, _, _ := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if contentType == "multipart/form-data" {
		app.selectUploadedROMs(w, r)
		return
	}
	var request struct {
		ID, Location, Path string
		Found, Clear       bool
	}
	if contentType != "application/json" ||
		json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10)).Decode(&request) != nil {
		writeJSONError(w, http.StatusBadRequest, "choose a ROM from the checklist")
		return
	}
	var choice romChoice
	id := request.ID
	if request.Found {
		choice = app.foundROMs()[id]
		if choice.name == "" {
			writeJSONError(w, http.StatusBadRequest, "the ROM was not found again")
			return
		}
	} else if !request.Clear {
		input, err := app.readBrowsedROM(request.Location, request.Path)
		if err != nil {
			writeJSONError(w, http.StatusBadRequest, err.Error())
			return
		}
		release, _, err := romset.Identify(input.Data)
		if err != nil {
			writeJSONError(w, http.StatusBadRequest, "Choose a supported clean ActRaiser or ActRaiser 2 ROM. See ROM checksums in the checklist.")
			return
		}
		if id != "" && id != release {
			writeJSONError(w, http.StatusBadRequest, "the selected ROM belongs to a different entry in the checklist")
			return
		}
		id = release
		choice = romChoice{name: input.Name, location: request.Location, path: request.Path}
	}
	choices := map[string]romChoice{}
	clear := ""
	if request.Clear {
		clear = id
	} else {
		choices[id] = choice
	}
	if app.updateROMChoices(w, choices, clear) {
		if choice.location != "" && !request.Found {
			app.rememberBrowserFolder("ROM", choice.location, choice.path)
		}
		writeJSON(w, http.StatusOK, map[string]string{"id": id, "name": choice.name})
	}
}

func (app *application) updateROMChoices(w http.ResponseWriter, choices map[string]romChoice, clear string) bool {
	app.mu.Lock()
	defer app.mu.Unlock()
	if app.state == "building" || app.playing {
		writeJSONError(w, http.StatusConflict, "wait for the current session to finish before changing ROMs")
		return false
	}
	delete(app.romChoices, clear)
	for id, choice := range choices {
		app.romChoices[id] = choice
	}
	return true
}

func (app *application) snapshotROMChoices() map[string]romChoice {
	app.mu.Lock()
	choices := make(map[string]romChoice, len(app.romChoices))
	for id, choice := range app.romChoices {
		choices[id] = choice
	}
	app.mu.Unlock()
	return choices
}

func (app *application) readROMChoices(choices map[string]romChoice) ([]romset.Input, error) {
	var inputs []romset.Input
	for _, expected := range romset.SupportedReleases() {
		id := expected.ID
		choice, ok := choices[id]
		if !ok {
			continue
		}
		var input romset.Input
		var err error
		switch {
		case choice.uploaded != nil:
			// The server already identified this immutable file on selection.
			inputs = append(inputs, romset.Input{Name: choice.name, Data: choice.uploaded})
			continue
		case choice.launchPath != "":
			input, err = readLaunchROM(choice.launchPath)
		default:
			input, err = app.readBrowsedROM(choice.location, choice.path)
		}
		if err != nil {
			return nil, err
		}
		release, _, err := romset.Identify(input.Data)
		if err != nil || release != id {
			return nil, fmt.Errorf("%s changed since selection; choose it again", choice.name)
		}
		inputs = append(inputs, input)
	}
	return inputs, nil
}

func (app *application) importSelectedROMs(w http.ResponseWriter) {
	if !app.dataGate.TryLock() {
		writeJSONError(w, http.StatusConflict, "the Builder is busy; try again shortly")
		return
	}
	defer app.dataGate.Unlock()
	app.mu.Lock()
	busy := app.state == "building" || app.playing || app.slimming
	app.mu.Unlock()
	if busy {
		writeJSONError(w, http.StatusConflict, "close the game or wait for the build before importing ROM content")
		return
	}
	choices := app.snapshotROMChoices()
	delete(choices, "us")
	inputs, err := app.readROMChoices(choices)
	if err == nil {
		var plan *romset.Plan
		plan, err = romset.Prepare(inputs)
		if err == nil {
			err = app.installROMContent(plan, true)
		}
	}
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "installed"})
}
