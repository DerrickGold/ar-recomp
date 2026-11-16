package builder

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2game"
	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
	"github.com/DerrickGold/ar-recomp/installer/internal/gamerom"
	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
	"github.com/DerrickGold/ar-recomp/installer/internal/romset"
)

type romChoice struct {
	name, launchPath, location, path string
}

func (choice romChoice) preferSavedCore(saved string) bool {
	return saved != "" && choice.launchPath != ""
}

type inspectedROM struct {
	ID   string `json:"id"`
	Name string `json:"name"`
}

func inspectUploadedROMs(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxROMBytes+(1<<20))
	if err := r.ParseMultipartForm(maxROMBytes + (1 << 20)); err != nil {
		writeJSONError(w, http.StatusBadRequest, "could not read the selected ROMs")
		return
	}
	defer r.MultipartForm.RemoveAll()
	inputs, err := readUploadedROMs(r.MultipartForm.File["rom"])
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	selected := []inspectedROM{}
	unavailable := []string{}
	for _, input := range inputs {
		id, _, err := romset.Identify(input.Data)
		if err != nil {
			unavailable = append(unavailable, input.Name)
			continue
		}
		selected = append(selected, inspectedROM{ID: id, Name: input.Name})
	}
	writeJSON(w, http.StatusOK, map[string]any{"selected": selected, "unavailable": unavailable})
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

func supportedROMIDs() []string {
	ids := []string{}
	for _, release := range gamerom.Releases() {
		ids = append(ids, release.ID)
	}
	return append(ids, romset.SequelID)
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
		Selected      string `json:"selected,omitempty"`
		Found         string `json:"found,omitempty"`
		Origin        string `json:"origin,omitempty"`
		Media         string `json:"media,omitempty"`
		Master        string `json:"master,omitempty"`
		World         string `json:"world,omitempty"`
		Saved         bool   `json:"saved,omitempty"`
		TextReference bool   `json:"textReference,omitempty"`
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
	for _, id := range supportedROMIDs() {
		selection := choices[id]
		item := row{ID: id, Selected: selection.name, Found: found[id].name, Media: available[id]}
		if selection.launchPath != "" {
			item.Origin = "launcher"
		} else if selection.name != "" {
			item.Origin = "browser"
		}
		switch id {
		case "us":
			item.Saved = savedErr == nil
		case romset.SequelID:
			item.Master = ar2master.Status(app.options.ProjectRoot)
			item.World = ar2game.Status(app.options.ProjectRoot)
		default:
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
	var request struct {
		ID, Location, Path string
		Found, Clear       bool
	}
	if r.Header.Get("Content-Type") != "application/json" ||
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
			writeJSONError(w, http.StatusBadRequest, "select a supported clean ActRaiser or ActRaiser 2 ROM")
			return
		}
		if id != "" && id != release {
			writeJSONError(w, http.StatusBadRequest, "the selected ROM belongs to a different entry in the checklist")
			return
		}
		id = release
		choice = romChoice{name: input.Name, location: request.Location, path: request.Path}
	}
	app.mu.Lock()
	if app.state == "building" || app.playing {
		app.mu.Unlock()
		writeJSONError(w, http.StatusConflict, "wait for the current session to finish before changing ROMs")
		return
	}
	if request.Clear {
		delete(app.romChoices, id)
	} else {
		app.romChoices[id] = choice
	}
	app.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]string{"id": id, "name": choice.name})
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
	for _, id := range supportedROMIDs() {
		choice, ok := choices[id]
		if !ok {
			continue
		}
		var input romset.Input
		var err error
		if choice.launchPath != "" {
			input, err = readLaunchROM(choice.launchPath)
		} else {
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
			err = app.installROMContent(plan)
		}
	}
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "installed"})
}
