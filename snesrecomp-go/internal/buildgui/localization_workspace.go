package buildgui

import (
	"bytes"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	lk "github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

// Session state contains immutable snapshots, not in-progress archive work.
// Writers serialize through editMu; readers take a short snapshot under mu and
// never wait for a save, directory import, publication or client response.
type localizationStateData struct {
	store              localizationProjectStore
	current, reference *lk.AuthorProject
	export             *localizationExport
	pendingImport      *localizationImport
	source             *localizationSourceCache
}

// The GUI needs project transactions, not the store's filesystem internals.
// Keeping this narrow also lets concurrency tests hold a real save at its I/O
// boundary without sleeping or modifying the parser/store implementation.
type localizationProjectStore interface {
	Open(string) (*lk.AuthorProject, error)
	Save(*lk.AuthorProject, string) error
	List() ([]lk.AuthorProjectSummary, error)
}

// One request owns this copy. Expensive work is performed here, then a writer
// publishes its completed state with a short lock. AuthorStore still enforces
// cross-process revisions and atomic replacement; editMu only orders this UI.
type localizationWork struct {
	localizationStateData
	root      string
	fontProbe lk.FontCoverageProbe
}

func (app *application) localizationSource() *localizationSourceCache {
	s := &app.localization
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.source == nil {
		s.source = &localizationSourceCache{root: filepath.Join(app.localizationRoot(), "native-us")}
	}
	return s.source
}

func (app *application) localizationSnapshot() (*localizationWork, error) {
	s := &app.localization
	app.localizationSource()
	s.mu.Lock()
	store := s.store
	s.mu.Unlock()
	if store == nil {
		// Only cold initialization is serialized here; do not let concurrent
		// first requests race directory creation, or cache a retryable failure.
		if err := func() error {
			s.initMu.Lock()
			defer s.initMu.Unlock()
			s.mu.Lock()
			initialized := s.store != nil
			s.mu.Unlock()
			if initialized {
				return nil
			}
			created, err := lk.NewAuthorStore(filepath.Join(app.localizationRoot(), "projects"))
			if err != nil {
				return err
			}
			s.mu.Lock()
			s.store = created
			s.mu.Unlock()
			return nil
		}(); err != nil {
			return nil, err
		}
	}
	s.mu.Lock()
	work := &localizationWork{localizationStateData: s.localizationStateData, root: app.localizationRoot(), fontProbe: app.probeLocalizationFonts}
	s.mu.Unlock()
	return work, nil
}

// Serialize responses only after releasing both session and writer locks.
// Archives retain their existing immutable bytes; no second archive-sized
// response buffer is introduced to get slow clients off the lock.
type localizationReply struct {
	status  int
	value   any
	archive *localizationExport
}

func (reply *localizationReply) json(status int, value any) {
	reply.status, reply.value = status, value
}

func (reply *localizationReply) write(w http.ResponseWriter, r *http.Request) {
	if reply.archive != nil {
		w.Header().Set("Content-Type", "application/zip")
		w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="%s"`, reply.archive.name))
		http.ServeContent(w, r, reply.archive.name, time.Time{}, bytes.NewReader(reply.archive.data))
		return
	}
	writeJSON(w, reply.status, reply.value)
}

// Native-source readiness is independent of author transactions. Only a new
// publication marker (or a delayed retry of an invalid source) reparses it.
// Neither a large imported font nor an archive download holds this lock.
type localizationSourceCache struct {
	mu       sync.Mutex
	root     string
	native   *lk.AuthorProject
	checked  bool
	manifest os.FileInfo
	err      error
	retry    time.Time
}

func (s *localizationSourceCache) snapshot(refresh bool) (*lk.AuthorProject, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if refresh {
		info, err := os.Lstat(filepath.Join(s.root, "pack.ini"))
		unchanged := err == nil && s.manifest != nil && os.SameFile(info, s.manifest) && info.Size() == s.manifest.Size() && info.ModTime().Equal(s.manifest.ModTime()) && info.Mode() == s.manifest.Mode()
		if !(unchanged && s.checked && (s.native != nil || time.Now().Before(s.retry))) {
			s.native, s.checked, s.manifest = nil, false, info
			s.retry = time.Now().Add(2 * time.Second)
		}
	}
	if !s.checked {
		s.checked = true
		pack, err := lk.OpenNativeUSSource(s.root)
		s.err = err
		if pack != nil && err == nil {
			s.native, s.err = lk.NewSourceProject(pack)
		}
	}
	return s.native, s.err
}

func (work *localizationWork) nativeLocalizationSource() *lk.AuthorProject {
	p, _ := work.source.snapshot(false)
	return p
}

func (work *localizationWork) refreshNativeLocalizationSource() { _, _ = work.source.snapshot(true) }

func (app *application) localizationAvailability() (bool, string) {
	native, err := app.localizationSource().snapshot(true)
	if err != nil {
		return false, "The native US language source needs attention: " + err.Error()
	}
	return native != nil, ""
}

func (app *application) checkLocalizationIdentity(id, revision string) error {
	s := &app.localization
	s.mu.Lock()
	p := s.current
	s.mu.Unlock()
	if p == nil || id != p.Pack().Manifest().Metadata().ID || revision != p.ProjectRevision() {
		return fmt.Errorf("%w: project changed; reopen before continuing", lk.ErrProjectConflict)
	}
	return nil
}
