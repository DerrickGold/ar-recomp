package buildgui

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"time"

	lk "github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

type localizationImport struct {
	token   string
	project *lk.AuthorProject
}

// One detached preview per session. Selecting a folder/archive never replaces
// the open project or writes an installation. Duplicate decisions refer to the
// incoming ID, not whichever unrelated project happens to be open.
func (app *application) previewLocalizationImport(w http.ResponseWriter, p *lk.AuthorProject) error {
	id := p.Pack().Manifest().Metadata().ID
	existing, err := app.localization.store.Open(id)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	token, err := randomToken()
	if err != nil {
		return err
	}
	result := map[string]any{"token": token, "metadata": p.Pack().Manifest().Metadata(), "messages": p.Pack().Workspace().Stats().MessageCount}
	if existing != nil {
		result["existingProject"] = map[string]string{"name": existing.Pack().Manifest().Metadata().Name, "revision": existing.ProjectRevision()}
	}
	if _, err := lk.InspectInstalledPack(filepath.Join(app.localizationRoot(), "packs"), id); err == nil {
		result["installed"] = true
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	if _, _, err := p.Installation(); err != nil {
		result["installError"] = err.Error()
	}
	app.localization.pendingImport = &localizationImport{token: token, project: p}
	writeJSON(w, 200, result)
	return nil
}

func (app *application) chooseLocalizationDirectory(w http.ResponseWriter, r *http.Request) {
	// A native modal must not hold the editor lock or stall build-status polls.
	if !app.directoryPickerMu.TryLock() {
		writeJSONError(w, http.StatusConflict, "a folder chooser is already open")
		return
	}
	defer app.directoryPickerMu.Unlock()
	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Minute)
	defer cancel()
	stop := context.AfterFunc(app.ctx, cancel)
	defer stop()
	pick := app.options.pickDirectory
	if pick == nil {
		pick = choosePackDirectory
	}
	dir, err := pick(ctx)
	if err != nil {
		writeJSONError(w, 400, err.Error())
		return
	}
	if dir != "" && !filepath.IsAbs(dir) {
		writeJSONError(w, 400, fmt.Sprintf("folder chooser returned a non-absolute path: %q", dir))
		return
	}
	writeJSON(w, 200, map[string]any{"directory": dir, "cancelled": dir == ""})
}
