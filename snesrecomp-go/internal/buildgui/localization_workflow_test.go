package buildgui

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	lk "github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

func TestDirectArchiveChecklist(t *testing.T) {
	app := editableWorkflowFixture(t)
	p := app.localization.current
	p, err := p.EditMessage("action.hud.act_1", "Excellent!\n@end\n", lk.TranslationDone)
	if err != nil {
		t.Fatal(err)
	}
	p, _, err = p.Publication(lk.PublicationOptions{ConfirmRights: true})
	if err != nil {
		t.Fatal(err)
	}
	var archive bytes.Buffer
	if err = p.WriteArchive(&archive, "publication"); err != nil {
		t.Fatal(err)
	}
	root := filepath.Join(app.localizationRoot(), "packs")
	if err = os.MkdirAll(root, 0755); err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(filepath.Join(root, "Fun translation.arlang"), archive.Bytes(), 0644); err != nil {
		t.Fatal(err)
	}
	for _, enabled := range []bool{false, true} {
		var rows []localizationCatalogEntry
		if err := json.Unmarshal(locGET(t, app, "catalog", nil).Body.Bytes(), &rows); err != nil {
			t.Fatal(err)
		}
		if len(rows) != 1 || !rows[0].Installed || rows[0].Key != "Fun translation.arlang" || rows[0].Error != "" {
			t.Fatal(rows)
		}
		locJSON(t, app, "set-enabled", localizationRequest{ID: rows[0].ID, Directory: rows[0].Key, Expected: rows[0].InstalledRevision, Enabled: enabled}, 200)
	}
	var rows []localizationCatalogEntry
	json.Unmarshal(locGET(t, app, "catalog", nil).Body.Bytes(), &rows)
	locJSON(t, app, "uninstall", localizationRequest{ID: rows[0].ID, Directory: rows[0].Key, Expected: rows[0].InstalledRevision, ConfirmUninstall: true}, 200)
	if _, err := os.Stat(filepath.Join(root, "Fun translation.arlang")); !os.IsNotExist(err) {
		t.Fatal("archive remained discoverable")
	}
}

func editableWorkflowFixture(t *testing.T) *application {
	t.Helper()
	app := localizationTestApp(t)
	installLocalizationCoverageSource(t, app)
	locGET(t, app, "state", nil)
	source := localizationTestSource(t)
	m := source.Pack().Manifest().Metadata()
	m.ID, m.Name = "test.workflow", "Workflow English"
	p, err := lk.NewTranslationProject(source.Pack(), m)
	if err != nil {
		t.Fatal(err)
	}
	if err = app.saveLocalization(p, ""); err != nil {
		t.Fatal(err)
	}
	return app
}

func TestLocalizationSaveAllIsAtomic(t *testing.T) {
	app := editableWorkflowFixture(t)
	q := locIdentity(app)
	q.Metadata = app.localization.current.Pack().Manifest().Metadata()
	q.Metadata.Name, q.Notes = "New project name", "private progress notes"
	q.SaveMessage, q.SaveDetails, q.SaveNotice = true, true, true
	q.NoticeName, q.NoticeText = "CREDITS.txt", "Test author credits."
	q.ID, q.Body, q.Status = "action.hud.act_1", "@invalid-control\n@end\n", lk.TranslationWIP
	before := app.localization.current.ProjectRevision()
	locJSON(t, app, "save", q, 400)
	onDisk, _ := app.localization.store.Open(q.ProjectID)
	if app.localization.current.ProjectRevision() != before || onDisk.ProjectRevision() != before {
		t.Fatal("partial save after validation failed")
	}
	q.Body = "Excellent!\n@end\n"
	locJSON(t, app, "save", q, 200)
	p := app.localization.current
	v, _ := p.Pack().Workspace().Message(q.ID)
	if p.Notes() != q.Notes || p.Pack().Manifest().Metadata().Name != q.Metadata.Name || v.Status != q.Status || v.Body != q.Body || p.Notices()["notices/CREDITS.txt"] != q.NoticeText {
		t.Fatal("save omitted pending fields", p.Notices(), v)
	}
	locJSON(t, app, "save", q, 409)
}

func TestLocalizationReferenceDoesNotSwitchProject(t *testing.T) {
	app := editableWorkflowFixture(t)
	source := localizationTestSource(t)
	before := app.localization.current.ProjectRevision()
	for i := 0; i < 2; i++ {
		if err := app.acceptLocalizationReference(source); err != nil {
			t.Fatal("repeated identical reference failed", err)
		}
	}
	q := locIdentity(app)
	q.ID = source.Pack().Manifest().Metadata().ID
	locJSON(t, app, "reference", q, 200)
	if app.localization.current.ProjectRevision() != before || app.localization.reference == nil {
		t.Fatal("reference changed current project")
	}
	query := locQuery(app)
	query.Set("id", "sky.action_mode.confirm")
	if !strings.Contains(locGET(t, app, "message", query).Body.String(), "referenceMetadata") {
		t.Fatal("reference label unavailable")
	}
	q.ID = ""
	locJSON(t, app, "reference", q, 200)
	if app.localization.reference != nil {
		t.Fatal("automatic US reference did not clear selection")
	}
	q.Revision = "stale"
	locJSON(t, app, "reference", q, 409)
}

func TestLocalizationCatalogAndUninstallPreserveProject(t *testing.T) {
	app := editableWorkflowFixture(t)
	q := locIdentity(app)
	q.ID, q.Body, q.Status = "sim.menu.listen", "Excellent!\n@end\n", lk.TranslationDone
	locJSON(t, app, "edit", q, 200)
	q = locIdentity(app)
	q.ConfirmRights = true
	locJSON(t, app, "install", q, 200)
	var rows []localizationCatalogEntry
	if err := json.Unmarshal(locGET(t, app, "catalog", nil).Body.Bytes(), &rows); err != nil {
		t.Fatal(err)
	}
	if len(rows) != 1 || !rows[0].Installed || !rows[0].Project {
		t.Fatal(rows)
	}
	// Installed-only packages remain visible even without an editable project.
	projectFile := filepath.Join(app.localizationRoot(), "projects", q.ProjectID+".arproject")
	if err := os.Rename(projectFile, projectFile+".backup"); err != nil {
		t.Fatal(err)
	}
	var installedOnly []localizationCatalogEntry
	if err := json.Unmarshal(locGET(t, app, "catalog", nil).Body.Bytes(), &installedOnly); err != nil {
		t.Fatal(err)
	}
	if len(installedOnly) != 1 || !installedOnly[0].Installed || installedOnly[0].Project {
		t.Fatal(installedOnly)
	}
	if err := os.Rename(projectFile+".backup", projectFile); err != nil {
		t.Fatal(err)
	}
	before := app.localization.current.ProjectRevision()
	uninstall := localizationRequest{ID: rows[0].ID, Directory: rows[0].Key, Expected: rows[0].InstalledRevision}
	locJSON(t, app, "uninstall", uninstall, 400)
	uninstall.ConfirmUninstall = true
	locJSON(t, app, "uninstall", uninstall, 200)
	p, err := app.localization.store.Open(q.ProjectID)
	if err != nil || p.ProjectRevision() != before || app.localization.current.ProjectRevision() != before {
		t.Fatal("uninstall changed project", err)
	}
	if _, err := os.Stat(filepath.Join(app.localizationRoot(), "packs", q.ProjectID, "pack.ini")); !os.IsNotExist(err) {
		t.Fatal("still installed", err)
	}
	if err := json.Unmarshal(locGET(t, app, "catalog", nil).Body.Bytes(), &rows); err != nil {
		t.Fatal(err)
	}
	if len(rows) != 1 || rows[0].Installed || !rows[0].Project {
		t.Fatal(rows)
	}
}

func TestLocalizationEditorHasVisibleWorkflowActions(t *testing.T) {
	for _, want := range []string{`id="loc-save-progress"`, `id="loc-back-projects"`, `id="loc-reference-project"`, `id="loc-extract-reference"`, `class="loc-comparison"`, `id="loc-library-filter"`} {
		if !strings.Contains(localizationHTML, want) {
			t.Fatal("missing editor action", want)
		}
	}
	if strings.Contains(localizationHTML, `id="loc-resume-message"`) || strings.Contains(localizationJS, `showMessage("dialogue.event.wrapper_05.call_02.source_00")`) {
		t.Fatal("one-off hard-coded shortcut remains")
	}
}
