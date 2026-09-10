package buildgui

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"testing/fstest"
	"time"

	lk "github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

func localizationTestSource(t *testing.T) *lk.AuthorProject {
	t.Helper()
	m := lk.PackMetadata{ID: "native-us", Name: "Native US fixture", Locale: "en-US", Autonym: "English", Author: "Fixture author", License: "Local use only", Direction: "auto", Target: "us-runtime", SourceProfile: "us", Coverage: "partial", Fallback: "native-us"}
	manifest, err := lk.NewPackManifest(m, lk.PackFonts{Primary: "builtin:actraiser-sans"}, []string{"text/source.artext"})
	if err != nil {
		t.Fatal(err)
	}
	pack, err := lk.LoadAuthorPack(fstest.MapFS{"pack.ini": {Data: []byte(manifest.Text())}, "text/source.artext": {Data: []byte(":: sky.action_mode.confirm\n@anchor reset_text_cursor.00\nInvented US template.\n@anchor yield.01\n@end\n:: action.hud.act_1\nInvented label.\n@end\n")}})
	if err != nil {
		t.Fatal(err)
	}
	p, err := lk.NewSourceProject(pack)
	if err != nil {
		t.Fatal(err)
	}
	return p
}

func localizationTestApp(t *testing.T) *application {
	t.Helper()
	app := newApplication(context.Background(), Options{ProjectRoot: filepath.Join(t.TempDir(), "space bundle", "utils"), fontCoverageProbe: unitFontCoverageProbe}, "secret")
	return app
}

// Unit workflow fixtures model an available font backend, not actual coverage.
// The real headless game command is exercised separately by font integration tests.
func unitFontCoverageProbe(_ context.Context, fonts []lk.FontCoverageSource, scalars []rune) (lk.FontCoverageProbeResult, error) {
	result := lk.FontCoverageProbeResult{Provided: make([]bool, len(scalars))}
	for i := range result.Provided {
		result.Provided[i] = true
	}
	for _, font := range fonts {
		result.Fonts = append(result.Fonts, lk.FontCoverageIdentity{Reference: font.Reference, SHA256: strings.Repeat("0", 64)})
	}
	return result, nil
}

// Seed fixtures through the same detached-work/publication boundary used by
// requests. These helpers are test-only, not alternate production APIs.
func (app *application) testLocalizationWork(action func(*localizationWork) error) error {
	s := &app.localization
	s.editMu.Lock()
	defer s.editMu.Unlock()
	work, err := app.localizationSnapshot()
	if err != nil {
		return err
	}
	err = action(work)
	s.mu.Lock()
	s.localizationStateData = work.localizationStateData
	s.mu.Unlock()
	return err
}

func (app *application) saveLocalization(p *lk.AuthorProject, expected string) error {
	return app.testLocalizationWork(func(work *localizationWork) error { return work.saveLocalization(p, expected) })
}

func (app *application) acceptLocalizationReference(p *lk.AuthorProject) error {
	return app.testLocalizationWork(func(work *localizationWork) error { return work.acceptLocalizationReference(p) })
}

func locJSON(t *testing.T, app *application, endpoint string, q any, code int) *httptest.ResponseRecorder {
	t.Helper()
	data, _ := json.Marshal(q)
	r := httptest.NewRequest("POST", "/secret/localization/"+endpoint, bytes.NewReader(data))
	r.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	app.ServeHTTP(w, r)
	if w.Code != code {
		t.Fatalf("%s: %d want %d: %s", endpoint, w.Code, code, w.Body.String())
	}
	return w
}
func locGET(t *testing.T, app *application, endpoint string, q url.Values) *httptest.ResponseRecorder {
	t.Helper()
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/secret/localization/"+endpoint+"?"+q.Encode(), nil))
	if w.Code != 200 {
		t.Fatalf("GET %s: %d %s", endpoint, w.Code, w.Body.String())
	}
	return w
}
func locIdentity(app *application) localizationRequest {
	p := app.localization.current
	return localizationRequest{ProjectID: p.Pack().Manifest().Metadata().ID, Revision: p.ProjectRevision()}
}
func locQuery(app *application) url.Values {
	q := locIdentity(app)
	return url.Values{"projectID": {q.ProjectID}, "revision": {q.Revision}}
}
func locUpload(t *testing.T, app *application, endpoint string, payload []byte, fields map[string]string, code int) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	form := multipart.NewWriter(&body)
	file, _ := form.CreateFormFile("file", "test.arlang")
	file.Write(payload)
	for k, v := range fields {
		form.WriteField(k, v)
	}
	form.Close()
	r := httptest.NewRequest("POST", "/secret/localization/"+endpoint, &body)
	r.Header.Set("Content-Type", form.FormDataContentType())
	w := httptest.NewRecorder()
	app.ServeHTTP(w, r)
	if w.Code != code {
		t.Fatalf("upload %s: %d want %d: %s", endpoint, w.Code, code, w.Body.String())
	}
	return w
}

func TestLocalizationLocationTreeAndSourceSearch(t *testing.T) {
	app := localizationTestApp(t)
	locGET(t, app, "state", nil)
	source := completeLocalizationTestSource(t)
	if _, err := lk.InstallNativeUSSource(filepath.Join(app.localizationRoot(), "native-us"), source.Pack()); err != nil {
		t.Fatal(err)
	}
	locGET(t, app, "state", nil)
	m := source.Pack().Manifest().Metadata()
	m.ID, m.Name = "test.locations", "Location navigation"
	p, err := lk.NewTranslationProject(source.Pack(), m)
	if err != nil {
		t.Fatal(err)
	}
	if err = app.saveLocalization(p, ""); err != nil {
		t.Fatal(err)
	}
	q := locIdentity(app)
	q.ID, q.Body, q.Status = "sim.menu.listen", "Listen, dude!\n@end\n", lk.TranslationDone
	locJSON(t, app, "edit", q, 200)
	var leaves = map[string]bool{}
	var walk func(string) int
	walk = func(parent string) int {
		query := locQuery(app)
		query.Set("view", "locations")
		query.Set("parent", parent)
		var rows []lk.AuthorTreeEntry
		if err := json.Unmarshal(locGET(t, app, "tree", query).Body.Bytes(), &rows); err != nil {
			t.Fatal(err)
		}
		if parent == "" {
			roots := lk.AuthorLocationRoots()
			if len(rows) != len(roots) {
				t.Fatal("wrong root count", len(rows))
			}
			for i, row := range rows {
				if row.Label != roots[i].Label {
					t.Fatal("wrong playthrough order", rows)
				}
			}
		}
		total := 0
		for _, row := range rows {
			total += row.Total
			if row.IsMessage {
				leaves[row.ID] = true
				if row.ID == "sim.menu.listen" && (row.Done != 1 || !strings.Contains(row.Label, "[shared]")) {
					t.Fatal("shared progress lost", row)
				}
			} else if got := walk(row.ID); got != row.Total {
				t.Fatalf("%s: total %d want %d", row.ID, got, row.Total)
			}
		}
		return total
	}
	walk("")
	refs, _ := lk.AuthorReferences("us")
	if len(leaves) != len(refs) {
		t.Fatal("location tree dropped routes", len(leaves), len(refs))
	}
	for _, ref := range refs {
		if !leaves[ref.ID] {
			t.Fatal("unreachable", ref.ID)
		}
	}
	// Published/imported packs omit unchanged native text. Searching and viewing
	// that missing route must still find the automatic US reference.
	published, _, err := app.localization.current.Publication(lk.PublicationOptions{ConfirmRights: true})
	if err != nil {
		t.Fatal(err)
	}
	app.localization.current = published
	if view, _ := published.Pack().Workspace().Message("sky.action_mode.confirm"); view.Present {
		t.Fatal("fixture did not omit the native fallback")
	}
	query := locQuery(app)
	query.Set("q", "Invented US template")
	if body := locGET(t, app, "search", query).Body.String(); !strings.Contains(body, "sky.action_mode.confirm") {
		t.Fatal("missing source search", body)
	}
	query.Set("q", "welcome back")
	if body := locGET(t, app, "search", query).Body.String(); !strings.Contains(body, "dialogue.event.wrapper_05.call_02.source_00") {
		t.Fatal("missing Continue shortcut", body)
	}
	query.Set("id", "sky.action_mode.confirm")
	if body := locGET(t, app, "message", query).Body.String(); !strings.Contains(body, `"reference":`) || !strings.Contains(body, "Invented US template") {
		t.Fatal("missing automatic native reference", body)
	}
}

func TestLocalizationCloneDoesNotInstall(t *testing.T) {
	app := localizationTestApp(t)
	locGET(t, app, "state", nil)
	source := localizationTestSource(t)
	m := source.Pack().Manifest().Metadata()
	m.ID = "test.original"
	p, err := lk.NewTranslationProject(source.Pack(), m)
	if err != nil {
		t.Fatal(err)
	}
	if err = app.saveLocalization(p, ""); err != nil {
		t.Fatal(err)
	}
	q := locIdentity(app)
	q.NewID, q.Metadata.Name = "test.clone", "Independent copy"
	locJSON(t, app, "clone", q, 200)
	if app.localization.current.Pack().Manifest().Metadata().Author != m.Author {
		t.Fatal("clone lost author")
	}
	if original, err := app.localization.store.Open(m.ID); err != nil || original.ProjectRevision() != p.ProjectRevision() {
		t.Fatal("clone changed original", err)
	}
	if body := locJSON(t, app, "installation", locIdentity(app), 200).Body.String(); !strings.Contains(body, `"installed":false`) {
		t.Fatal(body)
	}
	if _, err := os.Stat(filepath.Join(app.localizationRoot(), "packs")); !os.IsNotExist(err) {
		t.Fatal("opening/cloning wrote installed packs", err)
	}
	q = locIdentity(app)
	q.NewID = "test.original"
	locJSON(t, app, "clone", q, 409)
	if app.localization.current.Pack().Manifest().Metadata().ID != "test.clone" {
		t.Fatal("failed clone changed current project")
	}
}

func TestLocalizationGUIAuthorSharingLifecycle(t *testing.T) {
	app := localizationTestApp(t)
	installLocalizationCoverageSource(t, app)
	locGET(t, app, "state", nil)
	source := localizationTestSource(t)
	if err := app.localization.store.Save(source, ""); err != nil {
		t.Fatal(err)
	}
	locJSON(t, app, "open", localizationRequest{ID: "native-us"}, 200)
	q := locIdentity(app)
	q.ID = "action.hud.act_1"
	q.Body = "Cannot edit a source\n"
	q.Status = lk.TranslationDone
	locJSON(t, app, "edit", q, 400)
	m := source.Pack().Manifest().Metadata()
	m.ID = "community.excellent-adventure"
	m.Name = "Excellent Adventure English"
	m.Author = "Dude Team; original contributor"
	m.License = "CC-BY-4.0"
	m.Locale = "en-CA"
	locJSON(t, app, "create", localizationRequest{Metadata: m}, 200)
	old := locIdentity(app)
	q = old
	q.ID = "sky.action_mode.confirm"
	q.Status = lk.TranslationDone
	q.Body = "# private note\n@anchor reset_text_cursor.00\nMost excellent, {master_name}!\n@page\nThis town is ready for an epic adventure.\n@anchor yield.01\n@end\n"
	locJSON(t, app, "preview", q, 200)
	if app.localization.current.ProjectRevision() != old.Revision {
		t.Fatal("preview saved draft")
	}
	locJSON(t, app, "edit", q, 200)
	locJSON(t, app, "edit", q, 409) // Another tab's stale edit.
	q = locIdentity(app)
	q.ID = "sky.action_mode.confirm"
	q.Body = "Forgot the required controls."
	q.Status = lk.TranslationDone
	locJSON(t, app, "edit", q, 400)
	if app.localization.current.ProjectRevision() != q.Revision {
		t.Fatal("failed validation damaged saved text")
	}
	q = locIdentity(app)
	q.Metadata = m
	q.Notes = "Private reminder"
	locJSON(t, app, "metadata", q, 200)
	q = locIdentity(app)
	q.NoticeName = "CREDITS.txt"
	q.NoticeText = "Dude Team; original contributor. CC-BY-4.0."
	locJSON(t, app, "notice", q, 200)
	query := locQuery(app)
	query.Set("q", "epic adventure")
	rows := locGET(t, app, "search", query).Body.String()
	if !strings.Contains(rows, "sky.action_mode.confirm") || strings.Contains(rows, "Most excellent") {
		t.Fatal("search must send matching IDs, not full bodies", rows)
	}
	query = locQuery(app)
	query.Set("parent", "sky.action_mode")
	locGET(t, app, "tree", query)
	query.Set("id", "sky.action_mode.confirm")
	locGET(t, app, "message", query)
	q = locIdentity(app)
	backup := locJSON(t, app, "backup", q, 200).Body.Bytes()
	locJSON(t, app, "publish", q, 400)
	q.ConfirmRights = true
	shared := locJSON(t, app, "publish", q, 200).Body.Bytes()
	q.PrepareDownload = true
	prepared := locJSON(t, app, "publish", q, 200)
	var link struct {
		URL  string
		Name string
	}
	if err := json.Unmarshal(prepared.Body.Bytes(), &link); err != nil {
		t.Fatal(err)
	}
	attachment := locGET(t, app, strings.TrimPrefix(link.URL, "localization/"), nil)
	if !bytes.Equal(attachment.Body.Bytes(), shared) || !strings.Contains(attachment.Header().Get("Content-Disposition"), ".arlang") {
		t.Fatal("download changed publication bytes")
	}
	q.PrepareDownload = false
	z, err := zip.NewReader(bytes.NewReader(shared), int64(len(shared)))
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range z.File {
		r, _ := f.Open()
		data, _ := io.ReadAll(r)
		r.Close()
		if strings.Contains(string(data), "Private reminder") || strings.Contains(string(data), "private note") || strings.Contains(string(data), "Invented US") {
			t.Fatal("publication leaked private/reference text", f.Name)
		}
	}
	locJSON(t, app, "install", q, 200)
	installed, err := lk.OpenAuthorPack(filepath.Join(app.localizationRoot(), "packs", m.ID))
	if err != nil {
		t.Fatal(err)
	}
	if installed.Manifest().Metadata().Author != m.Author {
		t.Fatal("installation lost credits")
	}
	locJSON(t, app, "install", q, 409)
	q.Replace = true
	locJSON(t, app, "install", q, 200)
	fresh := localizationTestApp(t)
	installLocalizationCoverageSource(t, fresh)
	locUpload(t, fresh, "import", shared, nil, 200)
	if fresh.localization.current.Pack().Manifest().Metadata().Locale != "en-CA" {
		t.Fatal("locale lost on import")
	}
	if fresh.localization.current.Pack().Manifest().Metadata().Author != m.Author || len(fresh.localization.current.Notices()) != 1 {
		t.Fatal("authorship lost on import")
	}
	prior := fresh.localization.current.ProjectRevision()
	locUpload(t, fresh, "import", shared, nil, 409)
	if fresh.localization.current.ProjectRevision() != prior {
		t.Fatal("conflict changed active project")
	}
	locUpload(t, fresh, "import", shared, map[string]string{"newID": "community.another-edition"}, 200)
	locUpload(t, fresh, "import", shared, map[string]string{"newID": "community.another-edition", "replace": "true", "expected": "wrong"}, 409)
	locUpload(t, fresh, "import", shared, map[string]string{"newID": "community.another-edition", "replace": "true", "expected": fresh.localization.current.ProjectRevision()}, 200)
	q = locIdentity(fresh)
	q.ID = "sky.action_mode.confirm"
	q.Body = "@anchor reset_text_cursor.00\nA shorter, stellar mission!\n@anchor yield.01\n@end\n"
	q.Status = lk.TranslationDone
	locJSON(t, fresh, "edit", q, 200)
	q = locIdentity(fresh)
	q.ID = "action.hud.act_2"
	q.Body = "@empty\n@end\n"
	q.Status = lk.TranslationWIP
	locJSON(t, fresh, "edit", q, 200)
	q = locIdentity(fresh)
	q.ConfirmRights = true
	q.IncludeWIP = true
	locJSON(t, fresh, "publish", q, 200)
	locUpload(t, fresh, "import", backup, map[string]string{"newID": "community.restored"}, 200)
	if fresh.localization.current.Notes() != "Private reminder" {
		t.Fatal("backup notes not restored")
	}
	// A new process resumes disk state without any ROM, Python, original files,
	// or dependency on the executable's directory.
	restarted := newApplication(context.Background(), fresh.options, "secret")
	locJSON(t, restarted, "open", localizationRequest{ID: "community.restored"}, 200)
	if restarted.localization.current.ProjectRevision() != fresh.localization.current.ProjectRevision() {
		t.Fatal("reopen lost saved progress")
	}
	list := locGET(t, restarted, "projects", nil).Body.String()
	if !strings.Contains(list, "community.excellent-adventure") || !strings.Contains(list, "community.another-edition") {
		t.Fatal("same-locale packs collapsed")
	}
	if _, err := os.Stat(filepath.Join(app.options.ProjectRoot, "game-assets", "languages", "projects", m.ID+".arproject")); err != nil {
		t.Fatal(err)
	}
}

func TestLocalizationGUIRejectsMalformedAndUnscopedRequests(t *testing.T) {
	app := localizationTestApp(t)
	locUpload(t, app, "import", []byte("not a zip"), nil, 400)
	locUpload(t, app, "extract", []byte("not a clean ROM"), nil, 400)
	if app.localization.current != nil {
		t.Fatal("failed import selected a project")
	}
	locJSON(t, app, "open", localizationRequest{ID: "../escape"}, 400)
	locJSON(t, app, "directory", localizationRequest{Directory: "relative"}, 400)
	locJSON(t, app, "create", map[string]any{"unexpected": true}, 400)
	r := httptest.NewRequest("POST", "/secret/localization/open", strings.NewReader(`{"id":"x"} {}`))
	r.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	app.ServeHTTP(w, r)
	if w.Code != 400 {
		t.Fatal("trailing JSON accepted")
	}
	w = httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/localization/projects", nil))
	if w.Code != 404 {
		t.Fatal("token prefix bypassed")
	}
	for endpoint, ctype := range map[string]string{"editor.js": "text/javascript", "editor.css": "text/css"} {
		w := locGET(t, app, endpoint, nil)
		if !strings.HasPrefix(w.Header().Get("Content-Type"), ctype) {
			t.Fatal("wrong embedded asset type")
		}
	}
	if strings.Contains(localizationJS, "innerHTML") || strings.Contains(localizationJS, "eval(") {
		t.Fatal("untrusted pack content can become markup/code")
	}
	page := renderPage(t)
	for _, id := range []string{"loc-create", "loc-import", "loc-search", "loc-body", "loc-backup", "loc-publish", "loc-install", "loc-notice"} {
		if !strings.Contains(page, `id="`+id+`"`) {
			t.Fatal("missing GUI control", id)
		}
	}
	if strings.Contains(page, "{{LOCALIZATION}}") {
		t.Fatal("missing embedded workspace")
	}
}

// Optional local-ROM acceptance gate. Retail bytes/prose are never embedded in
// the test or required in a distribution. Ordinary CI uses the invented fixture.
func TestLocalizationGUIRetailExtraction(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("optional five-ROM GUI acceptance")
	}
	app := localizationTestApp(t)
	// Expected counts and HUD lettering are reviewed values held independently
	// of the extractor: the HUD labels are transcriptions of packed graphics, so
	// a decoder change must not be able to redefine its own gate. The counts
	// differ per release because the sim/Sky labels are listed only where they
	// have been read: every Western release carries all three, and Japanese
	// carries only the angel label -- its context label is a ten-cell shape
	// this table has no room for.
	for _, tc := range []struct {
		file, profile string
		messages      int
		hud           [5]string
	}{
		{"ar.sfc", "us", 503, [5]string{"ACT", "ENEMY", "PLAYER", "SCORE", "TIME"}},
		{"ar-eu.sfc", "eu-en", 504, [5]string{"ACT", "ENEMY", "PLAYER", "SCORE", "TIME"}},
		{"ar-ger.sfc", "de", 504, [5]string{"ACT", "FEIND", "SPIELER", "PUNKTE", "ZEIT"}},
		{"ar-fra.sfc", "fr", 502, [5]string{"ACT", "ENNEMI", "JOUEUR", "SCORE", "TEMPS"}},
		{"ar-jp.sfc", "jp", 499, [5]string{"ACT", "ENEMY", "PLAYER", "SCORE", "TIME"}},
	} {
		t.Run(tc.profile, func(t *testing.T) {
			data, err := os.ReadFile(filepath.Join(root, tc.file))
			if err != nil {
				t.Fatal(err)
			}
			locUpload(t, app, "extract", data, nil, 200)
			p := app.localization.current
			if p.Pack().Manifest().Metadata().SourceProfile != tc.profile || p.Pack().Workspace().Stats().MessageCount != tc.messages || p.Origin() != "native-source" {
				t.Fatal("wrong regional source/coverage", p.Pack().Workspace().Stats().MessageCount)
			}
			for i, name := range [5]string{"act", "enemy", "player", "score", "time"} {
				id := "action.hud." + name + "_label"
				operations, err := p.Pack().MessageOperations(id)
				if err != nil {
					t.Fatal("missing action HUD route", id, err)
				}
				if len(operations) != 2 || operations[0].Op != "text" || operations[0].Value != tc.hud[i] || operations[1].Op != "end" {
					t.Fatal("wrong HUD lettering", id, operations)
				}
			}
			if tc.profile != "us" && p.Pack().Manifest().Metadata().Target != "reference-only" {
				t.Fatal("regional source became an active US translation")
			}
			var archive bytes.Buffer
			if err := p.WriteArchive(&archive, "backup"); err != nil {
				t.Fatal(err)
			}
			copy, err := lk.ReadAuthorArchive(bytes.NewReader(archive.Bytes()), int64(archive.Len()))
			if err != nil {
				t.Fatal(err)
			}
			if copy.ProjectRevision() != p.ProjectRevision() {
				t.Fatal("retail source roundtrip drift")
			}
			if probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE"); probe != "" {
				directory := t.TempDir()
				for path, data := range p.Pack().Files() {
					target := filepath.Join(directory, filepath.FromSlash(path))
					if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
						t.Fatal(err)
					}
					if err := os.WriteFile(target, data, 0644); err != nil {
						t.Fatal(err)
					}
				}
				output, err := exec.Command(probe, "--inspect", filepath.Join(directory, "pack.ini")).CombinedOutput()
				if err != nil {
					t.Fatal(err, string(output))
				}
				var report struct {
					Metadata lk.PackMetadata
					Revision string
				}
				if err := json.Unmarshal(output, &report); err != nil {
					t.Fatal(err)
				}
				if report.Metadata != p.Pack().Manifest().Metadata() || report.Revision != fmt.Sprintf("%016x", p.Pack().RuntimeRevision()) {
					t.Fatal("GUI extraction and C game disagree")
				}
			}
		})
	}
	baseline, err := lk.EnsureNativeUSSource(filepath.Join(app.localizationRoot(), "native-us"), nil)
	if err != nil {
		t.Fatal("existing source must reopen without a ROM", err)
	}
	m := baseline.Manifest().Metadata()
	m.ID = "community.retail-template-test"
	m.Name = "Independent template test"
	m.Author = "Test author"
	m.License = "CC0-1.0"
	locJSON(t, app, "create", localizationRequest{Metadata: m}, 200)
	q := locIdentity(app)
	q.ID = "sky.action_mode.confirm"
	q.Status = lk.TranslationDone
	q.Body = "@anchor reset_text_cursor.00\nAn entirely invented mission!\n@anchor yield.01\n@end\n"
	locJSON(t, app, "edit", q, 200)
	q = locIdentity(app)
	q.ConfirmRights = true
	locJSON(t, app, "publish", q, 200)
	locJSON(t, app, "install", q, 200)
}

func TestLocalizationRetailReferencesPreserveEditingProject(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("optional five-ROM in-editor reference acceptance")
	}
	app := editableWorkflowFixture(t)
	before := app.localization.current.ProjectRevision()
	for _, file := range []string{"ar.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc", "ar-jp.sfc", "ar.sfc"} {
		data, err := os.ReadFile(filepath.Join(root, file))
		if err != nil {
			t.Fatal(err)
		}
		q := locIdentity(app)
		locUpload(t, app, "extract", data, map[string]string{"intent": "reference", "projectID": q.ProjectID, "revision": q.Revision}, 200)
		if app.localization.current.ProjectRevision() != before || app.localization.reference == nil {
			t.Fatal("extraction switched project", file)
		}
	}
}

// stallingReader delivers a prefix, then blocks until released. It stands in
// for a client whose upload or download stops halfway.
type stallingReader struct {
	prefix   []byte
	released chan struct{}
	reading  chan struct{}
	once     sync.Once
}

func (s *stallingReader) Read(p []byte) (int, error) {
	if len(s.prefix) > 0 {
		n := copy(p, s.prefix)
		s.prefix = s.prefix[n:]
		return n, nil
	}
	s.once.Do(func() { close(s.reading) })
	<-s.released
	return 0, io.EOF
}

// The build page polls localization availability every 500 ms. A transfer that
// stalls mid-body must not hold the session lock and freeze that poll, or the
// rest of the workspace.
func TestLocalizationStalledUploadDoesNotBlockStatus(t *testing.T) {
	app := editableWorkflowFixture(t)
	var body bytes.Buffer
	form := multipart.NewWriter(&body)
	file, _ := form.CreateFormFile("file", "test.arlang")
	file.Write(make([]byte, 4096))
	form.Close()
	stall := &stallingReader{
		prefix:   body.Bytes()[:1024],
		released: make(chan struct{}),
		reading:  make(chan struct{}),
	}
	r := httptest.NewRequest("POST", "/secret/localization/import", stall)
	r.Header.Set("Content-Type", form.FormDataContentType())
	done := make(chan struct{})
	go func() {
		defer close(done)
		app.ServeHTTP(httptest.NewRecorder(), r)
	}()
	select {
	case <-stall.reading:
	case <-time.After(5 * time.Second):
		t.Fatal("upload never started reading")
	}

	answered := make(chan struct{})
	go func() {
		defer close(answered)
		app.localizationAvailability()
		locGET(t, app, "state", nil)
	}()
	select {
	case <-answered:
	case <-time.After(5 * time.Second):
		close(stall.released)
		<-done
		t.Fatal("a stalled upload blocked localization status")
	}
	close(stall.released)
	<-done
}
