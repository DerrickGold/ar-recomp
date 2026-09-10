package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"sync"
	"testing"
	"testing/fstest"
	"time"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type heldLocalizationWriter struct {
	*httptest.ResponseRecorder
	started, release chan struct{}
	once             sync.Once
}

func (w *heldLocalizationWriter) Write(data []byte) (int, error) {
	w.once.Do(func() { close(w.started) })
	<-w.release
	return w.ResponseRecorder.Write(data)
}

func localizationRequestFor(endpoint string, q any) *http.Request {
	data, _ := json.Marshal(q)
	r := httptest.NewRequest("POST", "/secret/localization/"+endpoint, bytes.NewReader(data))
	r.Header.Set("Content-Type", "application/json")
	return r
}

func awaitLocalization(t *testing.T, done <-chan struct{}, label string) {
	t.Helper()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal(label + " did not complete")
	}
}

func TestLocalizationSlowResponsesReleaseAllLocks(t *testing.T) {
	for _, endpoint := range []string{"state", "edit", "backup", "publish"} {
		t.Run(endpoint, func(t *testing.T) {
			app := editableWorkflowFixture(t)
			q := locIdentity(app)
			q.ID, q.Body, q.Status = "action.hud.act_1", "A new adventure!\n@end\n", lk.TranslationDone
			locJSON(t, app, "edit", q, 200)
			q = locIdentity(app)
			q.ID, q.Body, q.Status = "action.hud.act_1", "Another adventure!\n@end\n", lk.TranslationDone
			q.ConfirmRights = true
			request := localizationRequestFor(endpoint, q)
			if endpoint == "state" {
				request = httptest.NewRequest("GET", "/secret/localization/state", nil)
			}
			writer := &heldLocalizationWriter{ResponseRecorder: httptest.NewRecorder(), started: make(chan struct{}), release: make(chan struct{})}
			done := make(chan struct{})
			go func() { defer close(done); app.ServeHTTP(writer, request) }()
			released := false
			defer func() {
				if !released {
					close(writer.release)
				}
				awaitLocalization(t, done, "slow response")
			}()
			awaitLocalization(t, writer.started, "response start")
			expected := app.localization.current
			answered := make(chan struct{})
			go func() {
				defer close(answered)
				app.localizationAvailability()
				locGET(t, app, "state", nil)
				// A slow response cannot hold the writer queue either. If the
				// first request saved, use its already-published new revision.
				next := locIdentity(app)
				next.ID, next.Body, next.Status = "action.hud.act_1", "Saved during download.\n@end\n", lk.TranslationDone
				locJSON(t, app, "edit", next, 200)
			}()
			awaitLocalization(t, answered, "status/read/edit during stalled "+endpoint)
			close(writer.release)
			released = true
			awaitLocalization(t, done, "completed response")
			if endpoint == "backup" || endpoint == "publish" {
				archive, err := lk.ReadAuthorArchive(bytes.NewReader(writer.Body.Bytes()), int64(writer.Body.Len()))
				if err != nil {
					t.Fatal(err)
				}
				actual, _ := archive.Pack().Workspace().Message(q.ID)
				before, _ := expected.Pack().Workspace().Message(q.ID)
				if actual.Body != before.Body {
					t.Fatal("archive changed during download")
				}
			} else {
				var state struct {
					Project struct{ Revision string } `json:"project"`
				}
				if err := json.Unmarshal(writer.Body.Bytes(), &state); err != nil {
					t.Fatal(err)
				}
				if state.Project.Revision != expected.ProjectRevision() {
					t.Fatal("JSON changed during response")
				}
			}
		})
	}
}

type heldLocalizationStore struct {
	localizationProjectStore
	started, release chan struct{}
	once             sync.Once
}

func (s *heldLocalizationStore) Save(p *lk.AuthorProject, expected string) error {
	s.once.Do(func() { close(s.started); <-s.release })
	return s.localizationProjectStore.Save(p, expected)
}

func TestLocalizationSavePublishesOneSnapshotWithoutBlockingReaders(t *testing.T) {
	app := editableWorkflowFixture(t)
	initial := locIdentity(app)
	store := &heldLocalizationStore{localizationProjectStore: app.localization.store, started: make(chan struct{}), release: make(chan struct{})}
	app.localization.store = store
	q := initial
	q.ID, q.Body, q.Status = "action.hud.act_1", "Complete after the disk transaction.\n@end\n", lk.TranslationDone
	saved, conflicting := httptest.NewRecorder(), httptest.NewRecorder()
	firstDone, secondDone := make(chan struct{}), make(chan struct{})
	go func() { defer close(firstDone); app.ServeHTTP(saved, localizationRequestFor("edit", q)) }()
	released := false
	defer func() {
		if !released {
			close(store.release)
		}
		awaitLocalization(t, firstDone, "first save")
	}()
	awaitLocalization(t, store.started, "save transaction")
	readDone := make(chan struct{})
	go func() {
		defer close(readDone)
		app.localizationAvailability()
		locGET(t, app, "state", nil)
		query := locQuery(app)
		query.Set("id", q.ID)
		locGET(t, app, "message", query)
	}()
	awaitLocalization(t, readDone, "readers during save")
	if current := locIdentity(app); current.ProjectID != initial.ProjectID || current.Revision != initial.Revision {
		t.Fatal("published an unfinished save")
	}
	go func() { defer close(secondDone); app.ServeHTTP(conflicting, localizationRequestFor("edit", q)) }()
	close(store.release)
	released = true
	awaitLocalization(t, firstDone, "first save")
	awaitLocalization(t, secondDone, "stale save")
	if saved.Code != 200 || conflicting.Code != http.StatusConflict {
		t.Fatalf("save=%d stale=%d", saved.Code, conflicting.Code)
	}
	onDisk, err := store.Open(initial.ProjectID)
	if err != nil || onDisk.ProjectRevision() != app.localization.current.ProjectRevision() || onDisk.ProjectRevision() == initial.Revision {
		t.Fatal("session/disk publication diverged", err)
	}
}

func TestLocalizationConcurrentColdReaders(t *testing.T) {
	app := localizationTestApp(t)
	start := make(chan struct{})
	var wait sync.WaitGroup
	for i := 0; i < 16; i++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			<-start
			w := httptest.NewRecorder()
			app.ServeHTTP(w, httptest.NewRequest("GET", "/secret/localization/state", nil))
			if w.Code != 200 {
				t.Errorf("cold reader: %d %s", w.Code, w.Body.String())
			}
		}()
	}
	close(start)
	wait.Wait()
}

func localizationFontHeavyFixture(tb testing.TB) *application {
	tb.Helper()
	app := newApplication(context.Background(), Options{ProjectRoot: filepath.Join(tb.TempDir(), "utils")}, "secret")
	meta := lk.PackMetadata{ID: "native-us", Name: "Synthetic source", Locale: "en-US", Autonym: "English", Author: "Fixture", License: "MIT", Direction: "auto", Target: "us-runtime", SourceProfile: "us", Coverage: "partial", Fallback: "native-us"}
	manifest, err := lk.NewPackManifest(meta, lk.PackFonts{Primary: "fonts/Fixture.ttf"}, []string{"text/source.artext"})
	if err != nil {
		tb.Fatal(err)
	}
	// Storage-only fixture, not a rasterizer font. Incompressible bytes model
	// a large font dependency and prevent unrealistic all-zero ZIP timings.
	font := make([]byte, 8<<20)
	random := uint32(0x12345678)
	for i := range font {
		random ^= random << 13
		random ^= random >> 17
		random ^= random << 5
		font[i] = byte(random)
	}
	pack, err := lk.LoadAuthorPack(fstest.MapFS{
		"pack.ini": {Data: []byte(manifest.Text())}, "fonts/Fixture.ttf": {Data: font},
		"text/source.artext": {Data: []byte(":: action.hud.act_1\nInvented source.\n@end\n")},
	})
	if err != nil {
		tb.Fatal(err)
	}
	meta.ID, meta.Name = "test.font-heavy", "Font-heavy storage fixture"
	p, err := lk.NewTranslationProject(pack, meta)
	if err != nil {
		tb.Fatal(err)
	}
	if err := app.saveLocalization(p, ""); err != nil {
		tb.Fatal(err)
	}
	return app
}

func BenchmarkLocalizationFontHeavySave(b *testing.B) {
	app := localizationFontHeavyFixture(b)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		q := locIdentity(app)
		q.ID, q.Body, q.Status = "action.hud.act_1", fmt.Sprintf("Adventure %d\n@end\n", i), lk.TranslationDone
		w := httptest.NewRecorder()
		app.ServeHTTP(w, localizationRequestFor("edit", q))
		if w.Code != 200 {
			b.Fatal(w.Code, w.Body.String())
		}
	}
}
