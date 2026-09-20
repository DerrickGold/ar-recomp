package builder

import (
	"context"
	"errors"
	"fmt"
	"io"
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

func (work *localizationWork) acceptLocalizationImport(p *lk.AuthorProject, newID string, replace bool, expected string) error {
	if newID != "" {
		m := p.Pack().Manifest().Metadata()
		m.ID = newID
		var err error
		p, err = p.WithMetadata(m)
		if err != nil {
			return err
		}
	}
	if !replace {
		expected = ""
	}
	if replace && expected == "" {
		return fmt.Errorf("replacement requires the existing project's current revision; open it first")
	}
	return work.saveLocalization(p, expected)
}

func (work *localizationWork) acceptLocalizationReference(p *lk.AuthorProject) error {
	if p.Origin() != "native-source" {
		return fmt.Errorf("extracted reference must be a native source")
	}
	if err := work.store.Save(p, ""); err != nil {
		if !errors.Is(err, lk.ErrProjectConflict) {
			return err
		}
		existing, openErr := work.store.Open(p.Pack().Manifest().Metadata().ID)
		if openErr != nil || existing.Origin() != "native-source" || existing.ProjectRevision() != p.ProjectRevision() {
			return err
		}
	}
	work.reference = p
	return nil
}

// localizationUpload is the result of the unlocked half of an upload: the
// client's bytes are in, the ROM (if any) is decoded, and nothing shared has
// been touched yet.
type localizationUpload struct {
	endpoint    string
	project     *lk.AuthorProject
	pack        *lk.AuthorPack // native US source to install, when extracted
	asReference bool
	preview     bool
	newID       string
	replace     bool
	expected    string
	cleanup     func()
}

// prepareLocalizationUpload runs without the session lock. It reads the request
// body and decodes a ROM -- the two slow parts -- and only reads shared state
// through a momentary lock for the fail-fast identity check. The commit half
// rechecks that identity, so a project that changes meanwhile is a conflict,
// not a silent overwrite.
func (app *application) prepareLocalizationUpload(w http.ResponseWriter, r *http.Request, endpoint string) (*localizationUpload, error) {
	limit := int64(lk.MaxAuthorArchiveBytes + (1 << 20))
	if endpoint == "extract" {
		limit = 2 << 20
	}
	r.Body = http.MaxBytesReader(w, r.Body, limit)
	if err := r.ParseMultipartForm(2 << 20); err != nil {
		if r.MultipartForm != nil {
			r.MultipartForm.RemoveAll()
		}
		return nil, err
	}
	upload := &localizationUpload{
		endpoint:    endpoint,
		asReference: endpoint == "extract" && r.FormValue("intent") == "reference",
		preview:     endpoint == "import" && r.FormValue("intent") == "preview",
		newID:       r.FormValue("newID"),
		replace:     r.FormValue("replace") == "true",
		expected:    r.FormValue("expected"),
		cleanup:     func() { _ = r.MultipartForm.RemoveAll() },
	}
	if upload.asReference {
		err := app.checkLocalizationIdentity(r.FormValue("projectID"), r.FormValue("revision"))
		if err != nil {
			return upload, err
		}
	}
	f, _, err := r.FormFile("file")
	if err != nil {
		return upload, err
	}
	defer f.Close()
	if endpoint == "extract" {
		data, err := io.ReadAll(io.LimitReader(f, (1<<20)+1))
		if err != nil {
			return upload, err
		}
		d, err := lk.NewDecoder(data)
		if err != nil {
			return upload, err
		}
		pack, err := d.BuildNativeAuthorPack(d.NativeSourceMetadata())
		if err != nil {
			return upload, err
		}
		upload.project, err = lk.NewSourceProject(pack)
		if err != nil {
			return upload, err
		}
		// Do not touch a build ROM or install a regional reference as US text.
		if d.ReleaseID() == "us" {
			upload.pack = pack
		}
	} else {
		info, err := f.Seek(0, io.SeekEnd)
		if err != nil {
			return upload, err
		}
		upload.project, err = lk.ReadAuthorArchive(f, info)
		if err != nil {
			return upload, err
		}
	}
	return upload, r.Context().Err()
}

// commitLocalizationUpload runs against one writer's detached snapshot. Store
// and installation transactions complete before the session publishes it.
func (work *localizationWork) commitLocalizationUpload(w *localizationReply, r *http.Request, upload *localizationUpload) error {
	if err := r.Context().Err(); err != nil {
		return err
	}
	if upload.asReference {
		if err := work.checkLocalizationIdentity(r.FormValue("projectID"), r.FormValue("revision")); err != nil {
			return err
		}
	}
	if upload.pack != nil {
		if _, err := lk.InstallNativeUSSource(filepath.Join(work.root, "native-us"), upload.pack); err != nil {
			return err
		}
		work.refreshNativeLocalizationSource()
	}
	if upload.preview {
		return work.previewLocalizationImport(w, upload.project)
	}
	var err error
	if upload.asReference {
		err = work.acceptLocalizationReference(upload.project)
	} else {
		err = work.acceptLocalizationImport(upload.project, upload.newID, upload.replace, upload.expected)
	}
	if err != nil {
		return err
	}
	w.json(200, work.localizationState())
	return nil
}
