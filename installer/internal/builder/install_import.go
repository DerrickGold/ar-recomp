package builder

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
)

func (app *application) serveInstallImport(w http.ResponseWriter, r *http.Request, endpoint string) {
	w.Header().Set("Cache-Control", "no-store")
	if endpoint == "state" && r.Method == http.MethodGet && app.options.ImportSearchDir == "" {
		writeJSON(w, 200, map[string]bool{"enabled": false})
		return
	}
	if app.options.ImportSearchDir == "" {
		http.NotFound(w, r)
		return
	}
	fail := func(err error) { writeJSONError(w, 400, err.Error()) }
	if endpoint == "state" && r.Method == http.MethodGet {
		// This read-only snapshot never touches the pending import plan. Receipts
		// are published atomically, so startup requests from multiple tabs may
		// read concurrently, including while a chooser or import is active.
		state, err := desktop.ReadImportDecision(app.options.ProjectRoot)
		if err != nil {
			fail(err)
			return
		}
		candidates, err := desktop.DiscoverInstallData(app.options.ImportSearchDir, app.options.ProjectRoot)
		warning := ""
		if err != nil {
			warning = err.Error()
		}
		writeJSON(w, 200, map[string]any{"enabled": true, "decided": state.Decided, "destination": app.options.ProjectRoot, "candidates": candidates, "warning": warning})
		return
	}
	if r.Method != http.MethodPost {
		http.NotFound(w, r)
		return
	}
	if !app.installImportMu.TryLock() {
		writeJSONError(w, 409, "another import request is in progress")
		return
	}
	defer app.installImportMu.Unlock()
	if endpoint == "choose" {
		if !app.directoryPickerMu.TryLock() {
			writeJSONError(w, 409, "a folder chooser is already open")
			return
		}
		defer app.directoryPickerMu.Unlock()
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Minute)
		defer cancel()
		stop := context.AfterFunc(app.ctx, cancel)
		defer stop()
		pick := app.options.pickDirectory
		if pick == nil {
			pick = func(ctx context.Context) (string, error) {
				return chooseDirectoryWithPrompt(ctx, "Choose a previous ActRaiserRecomp installation or data folder")
			}
		}
		directory, err := pick(ctx)
		if err != nil {
			fail(err)
			return
		}
		if directory != "" && !filepath.IsAbs(directory) {
			fail(errors.New("folder chooser returned a relative path"))
			return
		}
		writeJSON(w, 200, map[string]string{"directory": directory})
		return
	}
	var q struct {
		Directory string `json:"directory"`
		Revision  string `json:"revision"`
		Confirm   bool   `json:"confirm"`
		Exact     bool   `json:"exact"`
	}
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16384))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&q); err != nil {
		fail(err)
		return
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		fail(errors.New("unexpected import request data"))
		return
	}
	switch endpoint {
	case "preview":
		app.installImportPlan = nil
		// Accept the outer old install as well as its data directory. If it
		// contains several recognized roots, let the user choose explicitly.
		candidates := []string{q.Directory}
		if !q.Exact {
			var err error
			candidates, err = desktop.DiscoverInstallData(q.Directory, app.options.ProjectRoot)
			if err != nil {
				fail(err)
				return
			}
		}
		if len(candidates) != 1 {
			if len(candidates) == 0 {
				fail(errors.New("no previous ActRaiserRecomp data found; choose its install folder or utils directory"))
				return
			}
			writeJSON(w, 200, map[string]any{"candidates": candidates})
			return
		}
		plan, err := desktop.PreviewInstallImport(r.Context(), candidates[0], app.options.ProjectRoot)
		if err != nil {
			fail(err)
			return
		}
		app.installImportPlan = &plan
		writeJSON(w, 200, plan)
	case "skip", "apply":
		if !app.dataGate.TryLock() {
			writeJSONError(w, 409, "wait for the current Workshop operation to finish")
			return
		}
		defer app.dataGate.Unlock()
		app.mu.Lock()
		busy := app.state == "building" || app.slimming
		app.mu.Unlock()
		app.previewMu.Lock()
		busy = busy || app.preview.State == "generating"
		app.previewMu.Unlock()
		if busy {
			writeJSONError(w, 409, "finish the build, cleanup, or audio generation before importing")
			return
		}
		if endpoint == "skip" {
			if err := desktop.SkipInstallImport(app.options.ProjectRoot); err != nil {
				fail(err)
				return
			}
			app.installImportPlan = nil
			writeJSON(w, 200, map[string]bool{"decided": true})
			return
		}
		if !q.Confirm || app.installImportPlan == nil || q.Revision != app.installImportPlan.Revision {
			writeJSONError(w, 409, "preview the installation and confirm that games are closed and editor changes are saved")
			return
		}
		plan := *app.installImportPlan
		app.installImportPlan = nil
		result, err := desktop.ApplyInstallImport(r.Context(), plan)
		// Even a partial, retryable import invalidates cached editor snapshots.
		app.localization.mu.Lock()
		app.localization.localizationStateData = localizationStateData{}
		app.localization.mu.Unlock()
		app.scenery.mu.Lock()
		app.scenery.assets = nil
		app.scenery.checked = time.Time{}
		app.scenery.mu.Unlock()
		log := &lockedLogWriter{app: app}
		if err != nil {
			fmt.Fprintf(log, "Installation import incomplete (originals unchanged): %v\n", err)
			fail(err)
			return
		}
		fmt.Fprintf(log, "Imported installation %s into %s: %d copied, %d identical, %d conflicts preserved. Originals unchanged.\n", plan.Source, plan.Destination, result.Copy, result.Identical, result.Conflicts)
		for _, file := range result.Files {
			if file.Action == "conflict" {
				fmt.Fprintf(log, "Import kept destination: %s\n", file.Path)
			}
		}
		app.refreshState()
		writeJSON(w, 200, result)
	default:
		http.NotFound(w, r)
	}
}
