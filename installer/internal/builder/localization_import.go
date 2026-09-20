package builder

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"time"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type localizationImport struct {
	token   string
	project *lk.AuthorProject
}

// One detached preview per session. Selecting a folder/archive never replaces
// the open project or writes an installation. Duplicate decisions refer to the
// incoming ID, not whichever unrelated project happens to be open.
func (work *localizationWork) previewLocalizationImport(w *localizationReply, p *lk.AuthorProject) error {
	id := p.Pack().Manifest().Metadata().ID
	existing, err := work.store.Open(id)
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
	if _, err := lk.InspectInstalledPack(filepath.Join(work.root, "packs"), id); err == nil {
		result["installed"] = true
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	if _, report, err := p.Installation(); err != nil {
		result["installError"] = err.Error()
	} else if report.Upgrade != nil {
		result["upgrade"] = report.Upgrade
	}
	work.pendingImport = &localizationImport{token: token, project: p}
	w.json(200, result)
	return nil
}

func (app *application) chooseLocalizationDirectory(w http.ResponseWriter, r *http.Request) {
	// A native modal must not hold the editor lock or stall build-status polls.
	if !app.directoryPickerMu.TryLock() {
		writeJSON(w, http.StatusConflict, map[string]string{"error": "a folder chooser is already open", "errorCode": "builder.language.chooser_busy"})
		return
	}
	defer app.directoryPickerMu.Unlock()
	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Minute)
	defer cancel()
	stop := context.AfterFunc(app.ctx, cancel)
	defer stop()
	pick := app.options.pickDirectory
	if pick == nil {
		preferences, _ := app.readInterfacePreferences()
		pick = func(ctx context.Context) (string, error) { return choosePackDirectory(ctx, preferences.Language) }
	}
	dir, err := pick(ctx)
	if err != nil {
		code := "builder.language.chooser_failed"
		if errors.Is(err, errDirectoryChooserUnavailable) {
			code = "builder.language.chooser_unavailable"
		}
		writeJSON(w, 400, map[string]string{"error": err.Error(), "errorCode": code})
		return
	}
	if dir != "" && !filepath.IsAbs(dir) {
		writeJSON(w, 400, map[string]string{"error": fmt.Sprintf("folder chooser returned a non-absolute path: %q", dir), "errorCode": "builder.language.chooser_failed"})
		return
	}
	writeJSON(w, 200, map[string]any{"directory": dir, "cancelled": dir == ""})
}
