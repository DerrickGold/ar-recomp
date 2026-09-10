package builder

import (
	"context"
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func TestImportPreviewTargetsIncomingIDAndDoesNotMutate(t *testing.T) {
	app := editableWorkflowFixture(t)
	current := app.localization.current
	m := current.Pack().Manifest().Metadata()
	m.ID = "test.incoming"
	m.Name = "Imported edition"
	incoming, err := current.WithMetadata(m)
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	for name, data := range incoming.Pack().Files() {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0644); err != nil {
			t.Fatal(err)
		}
	}
	preview := func() map[string]any {
		w := locJSON(t, app, "directory", localizationRequest{Directory: root, PreviewImport: true}, 200)
		var data map[string]any
		if err := json.Unmarshal(w.Body.Bytes(), &data); err != nil {
			t.Fatal(err)
		}
		return data
	}
	first := preview()
	if first["existingProject"] != nil || app.localization.current != current {
		t.Fatal("selecting directory changed current project")
	}
	if _, err := app.localization.store.Open(m.ID); !os.IsNotExist(err) {
		t.Fatal("preview wrote project", err)
	}
	if err := app.localization.store.Save(incoming, ""); err != nil {
		t.Fatal(err)
	}
	second := preview()
	if second["existingProject"].(map[string]any)["name"] != m.Name {
		t.Fatal("collision uses wrong project")
	}
	locJSON(t, app, "accept-import", localizationRequest{ImportToken: first["token"].(string)}, 409)
	locJSON(t, app, "accept-import", localizationRequest{ImportToken: second["token"].(string)}, 409)
	if app.localization.current != current {
		t.Fatal("conflicting import switched project")
	}
	locJSON(t, app, "accept-import", localizationRequest{ImportToken: second["token"].(string), Replace: true, Expected: incoming.ProjectRevision()}, 200)
	if app.localization.current.Pack().Manifest().Metadata().ID != m.ID {
		t.Fatal("wrong project imported")
	}
	if saved, err := app.localization.store.Open(current.Pack().Manifest().Metadata().ID); err != nil || saved.ProjectRevision() != current.ProjectRevision() {
		t.Fatal("unrelated open project overwritten", err)
	}
	locJSON(t, app, "accept-import", localizationRequest{ImportToken: second["token"].(string)}, 409)
	q := locIdentity(app)
	// Local installation has no redistribution or WIP prerequisites.
	locJSON(t, app, "installation-check", q, 200)
	locJSON(t, app, "install", q, 200)
	locJSON(t, app, "install", q, 409)
	q.Replace = true
	locJSON(t, app, "install", q, 200)
	locJSON(t, app, "publish", q, 400)
	if _, err := lk.OpenAuthorPack(filepath.Join(app.localizationRoot(), "packs", m.ID)); err != nil {
		t.Fatal(err)
	}
}

func TestFolderPickerCancellationAndNonBlockingStatus(t *testing.T) {
	app := localizationTestApp(t)
	started, release := make(chan struct{}), make(chan struct{})
	app.options.pickDirectory = func(ctx context.Context) (string, error) {
		close(started)
		select {
		case <-release:
			return "", nil
		case <-ctx.Done():
			return "", ctx.Err()
		}
	}
	done := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("POST", "/secret/localization/choose-directory", nil))
		done <- w
	}()
	<-started
	defer func() {
		select {
		case <-release:
		default:
			close(release)
		}
	}()
	statusDone := make(chan struct{})
	go func() {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("GET", "/secret/status", nil))
		close(statusDone)
	}()
	select {
	case <-statusDone:
	case <-time.After(time.Second):
		t.Fatal("folder dialog blocked status polling")
	}
	locJSON(t, app, "choose-directory", nil, 409)
	close(release)
	w := <-done
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"cancelled":true`) {
		t.Fatal(w.Code, w.Body.String())
	}
	if app.localization.store != nil || app.localization.current != nil {
		t.Fatal("cancelled picker changed editor state")
	}
}

func TestDirectoryPickerCommandsArePlatformSpecific(t *testing.T) {
	for _, platform := range []string{"darwin", "windows", "linux"} {
		name, args := directoryPickerCommand(platform, "Choose folder", func(name string) bool { return name == "zenity" })
		if name == "" || len(args) == 0 {
			t.Fatal("missing native chooser", platform)
		}
	}
	if name, _ := directoryPickerCommand("linux", "Choose folder", func(string) bool { return false }); name != "" {
		t.Fatal("invented Linux chooser")
	}
	if name, _ := directoryPickerCommand("linux", "Choose folder", func(name string) bool { return name == "kdialog" }); name != "kdialog" {
		t.Fatal("missing KDialog fallback")
	}
}

func TestDirectoryPromptLanguageIsDataNotProgram(t *testing.T) {
	for _, language := range []string{"en", "fr", "de", "ja", "en-GB", "invalid"} {
		prompt, err := directoryPickerPrompt(language)
		if err != nil || !strings.Contains(prompt, "pack.ini") {
			t.Fatal(language, prompt, err)
		}
		_, args := directoryPickerCommand("darwin", prompt, nil)
		if args[len(args)-1] != prompt || strings.Contains(args[1], prompt) {
			t.Fatal("caption interpolated into AppleScript", args)
		}
	}
	const injection = "'; exit 1; # \" 日本語\n"
	_, mac := directoryPickerCommand("darwin", injection, nil)
	if mac[len(mac)-1] != injection || strings.Contains(mac[1], injection) {
		t.Fatal(mac)
	}
	_, windows := directoryPickerCommand("windows", injection, nil)
	program := windows[len(windows)-1]
	if strings.Contains(program, injection) || !strings.Contains(program, "$env:AR_BUILDER_FOLDER_PROMPT") {
		t.Fatal(program)
	}
}

func TestInstalledChecklistAPIKeepsDisabledPacksManaged(t *testing.T) {
	app := editableWorkflowFixture(t)
	q := locIdentity(app)
	locJSON(t, app, "install", q, 200)
	root := filepath.Join(app.localizationRoot(), "packs")
	row, err := lk.InspectInstalledPack(root, q.ProjectID)
	if err != nil {
		t.Fatal(err)
	}
	toggle := localizationRequest{ID: q.ProjectID, Directory: row.Key, Expected: row.Revision, Enabled: false}
	locJSON(t, app, "set-enabled", toggle, 200)
	w := locJSON(t, app, "installation", q, 200)
	if !strings.Contains(w.Body.String(), `"installed":true`) || !strings.Contains(w.Body.String(), `"enabled":false`) {
		t.Fatal(w.Body.String())
	}
	w = locGET(t, app, "catalog", nil)
	if !strings.Contains(w.Body.String(), `"enabled":false`) || !strings.Contains(w.Body.String(), `"installed":true`) {
		t.Fatal(w.Body.String())
	}
	toggle.Enabled = true
	locJSON(t, app, "set-enabled", toggle, 409)
	row, err = lk.InspectInstalledPack(root, q.ProjectID)
	if err != nil {
		t.Fatal(err)
	}
	toggle.Expected = row.Revision
	locJSON(t, app, "set-enabled", toggle, 200)
	row, err = lk.InspectInstalledPack(root, q.ProjectID)
	if err != nil || !row.Enabled {
		t.Fatal(row, err)
	}
}
