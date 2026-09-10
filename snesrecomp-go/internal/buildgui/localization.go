package buildgui

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	lk "github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

// The GUI is an adapter over localizationkit, not another script parser or ROM
// decoder. Immutable project snapshots make disk-save failure transactional.
//
//go:embed localization.html
var localizationHTML string

//go:embed localization.css
var localizationCSS string

//go:embed localization.js
var localizationJS string

type localizationSession struct {
	mu             sync.Mutex
	store          *lk.AuthorStore
	current        *lk.AuthorProject
	reference      *lk.AuthorProject
	export         *localizationExport
	pendingImport  *localizationImport
	native         *lk.AuthorProject
	nativeChecked  bool
	nativeManifest os.FileInfo
	nativeError    error
	nativeRetry    time.Time
}

type localizationExport struct {
	token, name string
	data        []byte
}

type localizationRequest struct {
	ProjectID        string               `json:"projectID"`
	Revision         string               `json:"revision"`
	ID               string               `json:"id"`
	Body             string               `json:"body"`
	Status           lk.TranslationStatus `json:"status"`
	Metadata         lk.PackMetadata      `json:"metadata"`
	Notes            string               `json:"notes"`
	NoticeName       string               `json:"noticeName"`
	NoticeText       string               `json:"noticeText"`
	Directory        string               `json:"directory"`
	NewID            string               `json:"newID"`
	Replace          bool                 `json:"replace"`
	Expected         string               `json:"expected"`
	ConfirmRights    bool                 `json:"confirmRights"`
	IncludeWIP       bool                 `json:"includeWIP"`
	PrepareDownload  bool                 `json:"prepareDownload"`
	SaveMessage      bool                 `json:"saveMessage"`
	SaveDetails      bool                 `json:"saveDetails"`
	SaveNotice       bool                 `json:"saveNotice"`
	ConfirmUninstall bool                 `json:"confirmUninstall"`
	PreviewImport    bool                 `json:"previewImport"`
	ImportToken      string               `json:"importToken"`
	Enabled          bool                 `json:"enabled"`
}

func (app *application) localizationRoot() string {
	return filepath.Join(app.options.ProjectRoot, "game-assets", "languages")
}

func (app *application) serveLocalization(w http.ResponseWriter, r *http.Request, endpoint string) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if endpoint == "choose-directory" && r.Method == http.MethodPost {
		app.chooseLocalizationDirectory(w, r)
		return
	}
	if r.Method == http.MethodGet && (endpoint == "editor.js" || endpoint == "editor.css") {
		if endpoint == "editor.js" {
			w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
			io.WriteString(w, localizationJS)
		} else {
			w.Header().Set("Content-Type", "text/css; charset=utf-8")
			io.WriteString(w, localizationCSS)
		}
		return
	}
	if r.Method != http.MethodGet && r.Method != http.MethodPost {
		w.Header().Set("Allow", "GET, POST")
		writeJSONError(w, 405, "method not allowed")
		return
	}
	s := &app.localization
	// Everything that waits on the client -- reading a request body, writing an
	// archive back -- and the ROM decode that follows an upload happen off the
	// session lock. The same lock answers the build page's 500 ms status poll,
	// so a stalled transfer must not be able to hold it.
	if r.Method == http.MethodGet && strings.HasPrefix(endpoint, "download/") {
		app.serveLocalizationDownload(w, r, endpoint)
		return
	}
	var upload *localizationUpload
	var q localizationRequest
	var err error
	if r.Method == http.MethodPost {
		if endpoint == "extract" || endpoint == "import" {
			upload, err = app.prepareLocalizationUpload(w, r, endpoint)
			if upload != nil && upload.cleanup != nil {
				defer upload.cleanup()
			}
		} else {
			q, err = decodeLocalizationRequest(w, r)
		}
		if err != nil {
			writeLocalizationError(w, err)
			return
		}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.store == nil {
		s.store, err = lk.NewAuthorStore(filepath.Join(app.localizationRoot(), "projects"))
		if err != nil {
			writeJSONError(w, 400, err.Error())
			return
		}
	}
	if r.Method == http.MethodGet {
		err = app.readLocalization(w, r, endpoint)
	} else if upload != nil {
		err = app.commitLocalizationUpload(w, r, upload)
	} else {
		err = app.mutateLocalization(w, r, endpoint, q)
	}
	if err != nil {
		writeLocalizationError(w, err)
	}
}

func writeLocalizationError(w http.ResponseWriter, err error) {
	code := http.StatusBadRequest
	if errors.Is(err, lk.ErrProjectConflict) {
		code = http.StatusConflict
	}
	writeJSONError(w, code, err.Error())
}

func decodeLocalizationRequest(w http.ResponseWriter, r *http.Request) (localizationRequest, error) {
	var q localizationRequest
	r.Body = http.MaxBytesReader(w, r.Body, 20<<20)
	if !strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
		return q, fmt.Errorf("expected application/json")
	}
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&q); err != nil {
		return q, err
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF {
		return q, fmt.Errorf("expected one JSON request")
	}
	return q, nil
}

// The prepared archive is immutable once published, so it is served from a
// snapshot taken under a momentary lock rather than for the whole transfer.
func (app *application) serveLocalizationDownload(w http.ResponseWriter, r *http.Request, endpoint string) {
	app.localization.mu.Lock()
	export := app.localization.export
	app.localization.mu.Unlock()
	if export == nil || endpoint != "download/"+export.token {
		writeJSONError(w, http.StatusBadRequest, "download expired; prepare the archive again")
		return
	}
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="%s"`, export.name))
	http.ServeContent(w, r, export.name, time.Time{}, bytes.NewReader(export.data))
}

func (app *application) localizationState() map[string]any {
	s := &app.localization
	state := map[string]any{"project": nil, "root": app.localizationRoot()}
	if s.current != nil {
		p := s.current
		state["project"] = map[string]any{"metadata": p.Pack().Manifest().Metadata(), "revision": p.ProjectRevision(), "origin": p.Origin(), "notes": p.Notes(), "notices": p.Notices(), "fonts": p.Pack().Manifest().Fonts(), "totals": p.Pack().Workspace().Children("")}
	}
	if s.reference != nil {
		state["reference"] = s.reference.Pack().Manifest().Metadata()
	}
	state["sourceAvailable"] = app.nativeLocalizationSource() != nil
	return state
}

// Reused until the publication marker changes. Opening/importing a translation
// must not require a separate, undiscoverable reference selection.
func (app *application) nativeLocalizationSource() *lk.AuthorProject {
	s := &app.localization
	if !s.nativeChecked {
		s.nativeChecked = true
		pack, err := lk.OpenNativeUSSource(filepath.Join(app.localizationRoot(), "native-us"))
		s.nativeError = err
		if pack != nil && err == nil {
			s.native, s.nativeError = lk.NewSourceProject(pack)
		}
	}
	return s.native
}

// Poll only the publication marker while building; never rescan all messages
// on every 500 ms status request. The installer publishes pack.ini last and
// does not modify the version files underneath an active manifest. Failed
// validation retries slowly so a repaired dependency can be picked up too.
// Caller holds localization.mu, independently of the build's log mutex.
func (app *application) refreshNativeLocalizationSource() {
	s := &app.localization
	info, err := os.Lstat(filepath.Join(app.localizationRoot(), "native-us", "pack.ini"))
	unchanged := err == nil && s.nativeManifest != nil && os.SameFile(info, s.nativeManifest) && info.Size() == s.nativeManifest.Size() && info.ModTime().Equal(s.nativeManifest.ModTime()) && info.Mode() == s.nativeManifest.Mode()
	if unchanged && s.nativeChecked && (s.native != nil || time.Now().Before(s.nativeRetry)) {
		return
	}
	s.native, s.nativeChecked, s.nativeManifest = nil, false, info
	s.nativeRetry = time.Now().Add(2 * time.Second)
	app.nativeLocalizationSource()
}

func (app *application) localizationAvailability() (bool, string) {
	app.localization.mu.Lock()
	defer app.localization.mu.Unlock()
	app.refreshNativeLocalizationSource()
	if err := app.localization.nativeError; err != nil {
		return false, "The native US language source needs attention: " + err.Error()
	}
	return app.localization.native != nil, ""
}

func (app *application) localizationReference(id string) *lk.AuthorMessageView {
	view, _ := app.localizationReferenceWithMetadata(id)
	return view
}

func (app *application) localizationReferenceWithMetadata(id string) (*lk.AuthorMessageView, *lk.PackMetadata) {
	for _, p := range []*lk.AuthorProject{app.localization.reference, app.nativeLocalizationSource()} {
		if p != nil {
			if v, ok := p.Pack().Workspace().Message(id); ok && v.Present {
				metadata := p.Pack().Manifest().Metadata()
				return &v, &metadata
			}
		}
	}
	return nil, nil
}

func (app *application) locationTree(parent string) []lk.AuthorTreeEntry {
	w := app.localization.current.Pack().Workspace()
	refs, _ := lk.AuthorReferences(app.localization.current.Pack().Manifest().Metadata().SourceProfile)
	groups := map[string]*lk.AuthorTreeEntry{}
	parents := map[string]string{}
	order := map[string]int{}
	rows := []lk.AuthorTreeEntry{}
	accumulate := func(id, label, parent string, row lk.AuthorTreeEntry) {
		if groups[id] == nil {
			groups[id] = &lk.AuthorTreeEntry{ID: id, Label: label, HasChildren: true}
			parents[id] = parent
		}
		g := groups[id]
		g.Total += row.Total
		g.Present += row.Present
		g.Done += row.Done
		g.WIP += row.WIP
		g.NotStarted += row.NotStarted
	}
	for _, ref := range refs {
		v, _ := w.Message(ref.ID)
		row := lk.AuthorTreeEntry{ID: ref.ID, IsMessage: true, Total: 1}
		if v.Present {
			row.Present = 1
		}
		switch v.Status {
		case lk.TranslationDone:
			row.Done = 1
		case lk.TranslationWIP:
			row.WIP = 1
		default:
			row.NotStarted = 1
		}
		locations := lk.AuthorMessageLocations(ref.ID)
		for _, loc := range locations {
			order[loc.Group] = loc.CategoryOrder
			if parent == loc.Group {
				row.Label = app.localizationMessageTitle(ref.ID, loc)
				if len(locations) > 1 {
					row.Label += " [shared]"
				}
				rows = append(rows, row)
			}
			accumulate(loc.Root, loc.RootLabel, "", row)
			accumulate(loc.Group, loc.GroupLabel, loc.Root, row)
		}
	}
	if parent == "" {
		for _, root := range lk.AuthorLocationRoots() {
			if group := groups["place."+root.ID]; group != nil {
				rows = append(rows, *group)
			}
		}
		return rows
	}
	for id, group := range groups {
		if parents[id] == parent {
			rows = append(rows, *group)
		}
	}
	slices.SortFunc(rows, func(a, b lk.AuthorTreeEntry) int {
		if a.IsMessage && b.IsMessage {
			return strings.Compare(a.ID, b.ID) // Preserve source-slot story order.
		}
		if delta := order[a.ID] - order[b.ID]; delta != 0 {
			return delta
		}
		if cmp := strings.Compare(a.Label, b.Label); cmp != 0 {
			return cmp
		}
		return strings.Compare(a.ID, b.ID)
	})
	return rows
}

// Opaque ROM-wrapper/slot names are useful IDs, not useful translator labels.
// Resolve only requested labels through the shared parser; keep prose out of
// collapsed groups and retain the exact semantic ID in the editor tooltip.
func (app *application) localizationMessageTitle(id string, location lk.AuthorLocation) string {
	if !(strings.Contains(location.Title, "wrapper ") || strings.Contains(location.Title, "slot ")) {
		return location.Title
	}
	for _, project := range []*lk.AuthorProject{app.nativeLocalizationSource(), app.localization.current} {
		if project == nil {
			continue
		}
		ops, err := project.Pack().MessageOperations(id)
		if err != nil {
			continue
		}
		var text strings.Builder
		for _, op := range ops {
			if op.Op == "text" {
				text.WriteString(op.Value)
			} else if op.Op == "line" || op.Op == "paragraph" {
				text.WriteByte(' ')
			} else if op.Op == "placeholder" {
				text.WriteString("{" + op.Name + "}")
			}
			if text.Len() >= 90 || op.Op == "page" {
				break
			}
		}
		snippet := []rune(strings.Join(strings.Fields(text.String()), " "))
		if len(snippet) > 72 {
			snippet = append(snippet[:72], '…')
		}
		if len(snippet) > 0 {
			return string(snippet)
		}
	}
	return location.Title
}

func (app *application) readLocalization(w http.ResponseWriter, r *http.Request, endpoint string) error {
	s := &app.localization
	switch endpoint {
	case "state":
		app.refreshNativeLocalizationSource()
		writeJSON(w, 200, app.localizationState())
		return nil
	case "projects":
		rows, err := s.store.List()
		if err != nil {
			return err
		}
		writeJSON(w, 200, rows)
		return nil
	case "catalog":
		rows, err := app.localizationCatalog()
		if err != nil {
			return err
		}
		writeJSON(w, 200, rows)
		return nil
	}
	if s.current == nil {
		return fmt.Errorf("open or create a project first")
	}
	if r.URL.Query().Get("projectID") != s.current.Pack().Manifest().Metadata().ID || r.URL.Query().Get("revision") != s.current.ProjectRevision() {
		return fmt.Errorf("%w: project changed; reopen before continuing", lk.ErrProjectConflict)
	}
	workspace := s.current.Pack().Workspace()
	switch endpoint {
	case "tree":
		if r.URL.Query().Get("view") == "locations" {
			writeJSON(w, 200, app.locationTree(r.URL.Query().Get("parent")))
			return nil
		}
		writeJSON(w, 200, workspace.Children(r.URL.Query().Get("parent")))
	case "message":
		id := r.URL.Query().Get("id")
		view, ok := workspace.Message(id)
		if !ok {
			return fmt.Errorf("unknown message")
		}
		location := lk.AuthorMessageLocation(id)
		location.Title = app.localizationMessageTitle(id, location)
		result := map[string]any{"message": view, "location": location}
		if ref, metadata := app.localizationReferenceWithMetadata(id); ref != nil {
			result["reference"] = ref
			result["referenceMetadata"] = metadata
		}
		writeJSON(w, 200, result)
	case "search":
		q := r.URL.Query()
		needle, status := strings.ToLower(q.Get("q")), q.Get("status")
		if len(needle) > 256 {
			return fmt.Errorf("search is too long")
		}
		offset, _ := strconv.Atoi(q.Get("offset"))
		if offset < 0 {
			return fmt.Errorf("invalid result offset")
		}
		refs, _ := lk.AuthorReferences(s.current.Pack().Manifest().Metadata().SourceProfile)
		rows := []map[string]any{}
		total := 0
		for _, ref := range refs {
			v, _ := workspace.Message(ref.ID)
			loc := lk.AuthorMessageLocation(ref.ID)
			if status != "" && string(v.Status) != status {
				continue
			}
			if needle != "" {
				haystack := ref.ID + " " + v.Body
				for _, place := range lk.AuthorMessageLocations(ref.ID) {
					haystack += " " + place.Title + " " + place.RootLabel + " " + place.GroupLabel + " " + place.Context
				}
				if source := app.localizationReference(ref.ID); source != nil {
					haystack += " " + source.Body
				}
				if !strings.Contains(strings.ToLower(haystack), needle) {
					continue
				}
			}
			if total >= offset && len(rows) < 60 {
				rows = append(rows, map[string]any{"id": ref.ID, "title": app.localizationMessageTitle(ref.ID, loc), "group": loc.GroupLabel, "status": v.Status, "present": v.Present})
			}
			total++
		}
		writeJSON(w, 200, map[string]any{"rows": rows, "total": total, "offset": offset})
	default:
		return fmt.Errorf("unknown localization endpoint")
	}
	return nil
}

func (app *application) saveLocalization(p *lk.AuthorProject, expected string) error {
	if err := app.localization.store.Save(p, expected); err != nil {
		return err
	}
	app.localization.current = p
	return nil
}

func (app *application) mutateLocalization(w http.ResponseWriter, r *http.Request, endpoint string, q localizationRequest) error {
	s := &app.localization
	if endpoint == "set-enabled" {
		if err := lk.SetLanguagePackEnabled(filepath.Join(app.localizationRoot(), "packs"), q.Directory, q.ID, q.Expected, q.Enabled); err != nil {
			return err
		}
		writeJSON(w, 200, map[string]any{"enabled": q.Enabled, "message": "Package availability saved. Restart the game to refresh its language selector."})
		return nil
	}
	if endpoint == "accept-import" {
		if s.pendingImport == nil || q.ImportToken != s.pendingImport.token {
			return fmt.Errorf("%w: import preview expired; select the pack again", lk.ErrProjectConflict)
		}
		if err := app.acceptLocalizationImport(s.pendingImport.project, q.NewID, q.Replace, q.Expected); err != nil {
			return err
		}
		s.pendingImport = nil
		writeJSON(w, 200, app.localizationState())
		return nil
	}
	if endpoint == "uninstall" {
		if !q.ConfirmUninstall {
			return fmt.Errorf("confirm removal of this installed package first")
		}
		backup, err := lk.UninstallLanguagePack(filepath.Join(app.localizationRoot(), "packs"), q.Directory, q.ID, q.Expected)
		if err != nil {
			return err
		}
		writeJSON(w, 200, map[string]string{"backup": backup, "message": "Uninstalled from game discovery. Restart the game. Your workshop project and installed files are retained; the manifest was saved for recovery."})
		return nil
	}
	if endpoint == "open" || endpoint == "reference" {
		if endpoint == "reference" {
			if err := app.checkLocalizationIdentity(q.ProjectID, q.Revision); err != nil {
				return err
			}
			if q.ID == "" {
				s.reference = nil // Automatic Native US fallback.
				writeJSON(w, 200, app.localizationState())
				return nil
			}
		}
		p, err := s.store.Open(q.ID)
		if err != nil {
			return err
		}
		if endpoint == "open" {
			s.current = p
		} else {
			s.reference = p
		}
		writeJSON(w, 200, app.localizationState())
		return nil
	}
	if endpoint == "directory" {
		if !filepath.IsAbs(q.Directory) {
			return fmt.Errorf("choose an absolute pack directory")
		}
		p, err := lk.OpenAuthorProjectDirectory(q.Directory)
		if err != nil {
			return err
		}
		if q.PreviewImport {
			return app.previewLocalizationImport(w, p)
		}
		if err = app.acceptLocalizationImport(p, q.NewID, q.Replace, q.Expected); err != nil {
			return err
		}
		writeJSON(w, 200, app.localizationState())
		return nil
	}
	if endpoint == "create" {
		var source *lk.AuthorPack
		if s.current != nil && s.current.Origin() == "native-source" && s.current.Pack().Manifest().Metadata().SourceProfile == "us" {
			source = s.current.Pack()
		} else {
			var err error
			source, err = lk.OpenAuthorPack(filepath.Join(app.localizationRoot(), "native-us"))
			if err != nil {
				return fmt.Errorf("extract the US source or build the game first: %w", err)
			}
		}
		m := q.Metadata
		m.SourceProfile, m.Target, m.Coverage, m.Fallback = "us", "us-runtime", "partial", "native-us"
		p, err := lk.NewTranslationProject(source, m)
		if err != nil {
			return err
		}
		if err = app.saveLocalization(p, ""); err != nil {
			return err
		}
		s.reference, _ = lk.NewSourceProject(source)
		writeJSON(w, 200, app.localizationState())
		return nil
	}
	if s.current == nil || q.ProjectID != s.current.Pack().Manifest().Metadata().ID || q.Revision != s.current.ProjectRevision() {
		return fmt.Errorf("%w: project changed; reopen before saving", lk.ErrProjectConflict)
	}
	if err := r.Context().Err(); err != nil {
		return err
	}
	p := s.current
	var next *lk.AuthorProject
	var err error
	switch endpoint {
	case "save":
		if p.Origin() == "native-source" {
			return fmt.Errorf("native sources are read-only; create a translation first")
		}
		// One immutable edit chain and one archive replacement: invalid text or
		// metadata must not leave a partially saved set of progress/details.
		if !q.SaveMessage && !q.SaveDetails && !q.SaveNotice {
			return fmt.Errorf("no changes to save")
		}
		next = p
		if q.SaveDetails {
			m := p.Pack().Manifest().Metadata()
			m.Name, m.Locale, m.Autonym, m.Author, m.License, m.Direction = q.Metadata.Name, q.Metadata.Locale, q.Metadata.Autonym, q.Metadata.Author, q.Metadata.License, q.Metadata.Direction
			next, err = next.WithMetadata(m)
			if err == nil {
				next, err = next.WithNotes(q.Notes)
			}
		}
		if err == nil && q.SaveNotice {
			next, err = next.WithNotice(q.NoticeName, q.NoticeText)
		}
		if err == nil && q.SaveMessage {
			next, err = next.EditMessage(q.ID, q.Body, q.Status)
		}
	case "clone":
		if p.Origin() == "native-source" {
			return fmt.Errorf("create a translation from the US source instead of cloning a read-only source")
		}
		m := p.Pack().Manifest().Metadata()
		m.ID = q.NewID
		if q.Metadata.Name != "" {
			m.Name = q.Metadata.Name
		}
		next, err = p.WithMetadata(m)
		if err != nil {
			return err
		}
		if err = app.saveLocalization(next, ""); err != nil {
			return err
		}
		writeJSON(w, 200, app.localizationState())
		return nil
	case "installation":
		path := filepath.Join(app.localizationRoot(), "packs", p.Pack().Manifest().Metadata().ID, "pack.ini")
		installed, statErr := lk.InspectInstalledPack(filepath.Join(app.localizationRoot(), "packs"), p.Pack().Manifest().Metadata().ID)
		if statErr != nil && !os.IsNotExist(statErr) {
			return statErr
		}
		writeJSON(w, 200, map[string]any{"installed": statErr == nil, "enabled": installed.Enabled, "path": path})
		return nil
	case "install", "installation-check":
		prepared, report, err := p.Installation()
		if err != nil {
			return err
		}
		if endpoint == "installation-check" {
			writeJSON(w, 200, report)
			return nil
		}
		path, err := lk.InstallAuthorProject(filepath.Join(app.localizationRoot(), "packs", p.Pack().Manifest().Metadata().ID), prepared, q.Replace)
		if err != nil {
			return err
		}
		writeJSON(w, 200, map[string]any{"path": path, "report": report, "enabled": filepath.Base(path) == "pack.ini"})
		return nil
	case "edit":
		next, err = p.EditMessage(q.ID, q.Body, q.Status)
	case "preview":
		if p.Origin() != "native-source" {
			p, err = p.EditMessage(q.ID, q.Body, q.Status)
			if err != nil {
				return err
			}
		}
		ops, err := p.Pack().MessageOperations(q.ID)
		if err != nil {
			return err
		}
		writeJSON(w, 200, ops)
		return nil
	case "materialize":
		ops, err := p.Pack().MessageOperations(q.ID)
		if err != nil {
			return err
		}
		script, err := lk.EmitAuthorScript([]lk.AuthorMessage{{ID: q.ID, Operations: ops}}, "preview.artext")
		if err != nil {
			return err
		}
		body, _ := script.Body(q.ID)
		writeJSON(w, 200, map[string]string{"body": body})
		return nil
	case "metadata":
		m := p.Pack().Manifest().Metadata()
		m.Name, m.Locale, m.Autonym, m.Author, m.License, m.Direction = q.Metadata.Name, q.Metadata.Locale, q.Metadata.Autonym, q.Metadata.Author, q.Metadata.License, q.Metadata.Direction
		next, err = p.WithMetadata(m)
		if err == nil {
			next, err = next.WithNotes(q.Notes)
		}
	case "notice":
		next, err = p.WithNotice(q.NoticeName, q.NoticeText)
	case "backup", "publish", "publication-check":
		kind, extension := "backup", ".arproject"
		var report lk.PublicationReport
		if endpoint != "backup" {
			p, report, err = p.Publication(lk.PublicationOptions{ConfirmRights: q.ConfirmRights, IncludeWIP: q.IncludeWIP})
			if err != nil {
				return err
			}
			kind, extension = "publication", ".arlang"
		}
		if endpoint == "publication-check" {
			writeJSON(w, 200, report)
			return nil
		}
		var archive bytes.Buffer
		if err := p.WriteArchive(&archive, kind); err != nil {
			return err
		}
		if q.PrepareDownload {
			token, err := randomToken()
			if err != nil {
				return err
			}
			s.export = &localizationExport{token: token, name: p.Pack().Manifest().Metadata().ID + extension, data: archive.Bytes()}
			writeJSON(w, 200, map[string]string{"url": "localization/download/" + token, "name": s.export.name})
			return nil
		}
		w.Header().Set("Content-Type", "application/zip")
		w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="%s%s"`, p.Pack().Manifest().Metadata().ID, extension))
		w.Header().Set("Content-Length", strconv.Itoa(archive.Len()))
		_, err = io.Copy(w, &archive)
		return err
	default:
		return fmt.Errorf("unknown localization action")
	}
	if err != nil {
		return err
	}
	if err = app.saveLocalization(next, p.ProjectRevision()); err != nil {
		return err
	}
	writeJSON(w, 200, app.localizationState())
	return nil
}

func (app *application) acceptLocalizationImport(p *lk.AuthorProject, newID string, replace bool, expected string) error {
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
	return app.saveLocalization(p, expected)
}

func (app *application) checkLocalizationIdentity(id, revision string) error {
	p := app.localization.current
	if p == nil || id != p.Pack().Manifest().Metadata().ID || revision != p.ProjectRevision() {
		return fmt.Errorf("%w: project changed; reopen before continuing", lk.ErrProjectConflict)
	}
	return nil
}

// Extracting a comparison source is not opening a different author project.
// Repeated extraction can reuse the identical read-only source, but cannot
// silently overwrite a modified project with a colliding ID.
func (app *application) acceptLocalizationReference(p *lk.AuthorProject) error {
	if p.Origin() != "native-source" {
		return fmt.Errorf("extracted reference must be a native source")
	}
	if err := app.localization.store.Save(p, ""); err != nil {
		if !errors.Is(err, lk.ErrProjectConflict) {
			return err
		}
		existing, openErr := app.localization.store.Open(p.Pack().Manifest().Metadata().ID)
		if openErr != nil || existing.Origin() != "native-source" || existing.ProjectRevision() != p.ProjectRevision() {
			return err
		}
	}
	app.localization.reference = p
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
		app.localization.mu.Lock()
		err := app.checkLocalizationIdentity(r.FormValue("projectID"), r.FormValue("revision"))
		app.localization.mu.Unlock()
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

// commitLocalizationUpload holds the session lock. It only installs, accepts
// and answers from state that is already in memory.
func (app *application) commitLocalizationUpload(w http.ResponseWriter, r *http.Request, upload *localizationUpload) error {
	if err := r.Context().Err(); err != nil {
		return err
	}
	if upload.asReference {
		if err := app.checkLocalizationIdentity(r.FormValue("projectID"), r.FormValue("revision")); err != nil {
			return err
		}
	}
	if upload.pack != nil {
		if _, err := lk.InstallNativeUSSource(filepath.Join(app.localizationRoot(), "native-us"), upload.pack); err != nil {
			return err
		}
		app.refreshNativeLocalizationSource()
	}
	if upload.preview {
		return app.previewLocalizationImport(w, upload.project)
	}
	var err error
	if upload.asReference {
		err = app.acceptLocalizationReference(upload.project)
	} else {
		err = app.acceptLocalizationImport(upload.project, upload.newID, upload.replace, upload.expected)
	}
	if err != nil {
		return err
	}
	writeJSON(w, 200, app.localizationState())
	return nil
}
