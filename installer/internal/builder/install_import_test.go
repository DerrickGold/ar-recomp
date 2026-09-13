package builder

import (
	"context"
	"encoding/json"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
)

func TestInstallationImportUI(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("optional JS checks need Node; never a builder dependency")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	if out, err := exec.CommandContext(ctx, node, "--test", "testdata/install_import.test.mjs").CombinedOutput(); err != nil {
		t.Fatalf("installation import JS: %v\n%s", err, out)
	}
}

func importTestApp(t *testing.T) (*application, string) {
	t.Helper()
	base := t.TempDir()
	source := filepath.Join(base, "utils")
	if err := os.MkdirAll(filepath.Join(source, "game-assets"), 0755); err != nil {
		t.Fatal(err)
	}
	for leaf, body := range map[string]string{"game-assets/manifest.ini": "legacy", "game-assets/workshop-settings.json": `{"language":"ja"}`, "settings.ini": "legacy settings"} {
		if err := os.WriteFile(filepath.Join(source, leaf), []byte(body), 0644); err != nil {
			t.Fatal(err)
		}
	}
	return newApplication(context.Background(), Options{ProjectRoot: t.TempDir(), ImportSearchDir: base}, "tok"), source
}

func TestInstallImportHTTPPreviewConfirmationAndPersistence(t *testing.T) {
	app, source := importTestApp(t)
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/installation-import/state", nil))
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"decided":false`) || !strings.Contains(w.Body.String(), "utils") {
		t.Fatal(w.Code, w.Body.String())
	}
	body, _ := json.Marshal(map[string]string{"directory": source})
	w = interfacePOST(app, "/tok/installation-import/preview", string(body))
	var plan desktop.ImportPlan
	if w.Code != 200 || json.Unmarshal(w.Body.Bytes(), &plan) != nil || plan.Revision == "" {
		t.Fatal(w.Code, w.Body.String())
	}
	if _, err := os.Stat(filepath.Join(app.options.ProjectRoot, "settings.ini")); !os.IsNotExist(err) {
		t.Fatal("preview wrote data")
	}
	if w = interfacePOST(app, "/tok/installation-import/apply", `{}`); w.Code != 409 {
		t.Fatal("missing confirmation", w.Code)
	}
	body, _ = json.Marshal(map[string]any{"revision": plan.Revision, "confirm": true})
	app.state = "building"
	if w = interfacePOST(app, "/tok/installation-import/apply", string(body)); w.Code != 409 {
		t.Fatal("import while building", w.Code)
	}
	app.state = "idle"
	app.dataGate.RLock()
	if w = interfacePOST(app, "/tok/installation-import/apply", string(body)); w.Code != 409 {
		t.Fatal("import during editor operation", w.Code)
	}
	app.dataGate.RUnlock()
	if w = interfacePOST(app, "/tok/installation-import/apply", string(body)); w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	prefs, err := app.readInterfacePreferences()
	if err != nil || prefs.Language != "ja" {
		t.Fatal(prefs, err)
	}
	state, err := desktop.ReadImportDecision(app.options.ProjectRoot)
	if err != nil || !state.Decided {
		t.Fatal(state, err)
	}
	if w = interfacePOST(app, "/tok/installation-import/apply", string(body)); w.Code != 409 {
		t.Fatal("replayed commit", w.Code)
	}
}

func TestInstallImportHTTPScopedStrictAndOptional(t *testing.T) {
	app, _ := importTestApp(t)
	for _, body := range []string{`{"directory":"/tmp","replace":true}`, `{} {}`, `null`, strings.Repeat("x", 16385)} {
		w := interfacePOST(app, "/tok/installation-import/preview", body)
		if w.Code == 200 {
			t.Fatal("invalid request accepted", body)
		}
	}
	if w := interfacePOST(app, "/wrong/installation-import/skip", `{}`); w.Code != 404 {
		t.Fatal(w.Code)
	}
	if w := interfacePOST(app, "/tok/installation-import/skip", `{}`); w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	app.options.ImportSearchDir = ""
	if w := interfacePOST(app, "/tok/installation-import/skip", `{}`); w.Code != 404 {
		t.Fatal(w.Code)
	}
}

func TestInstallImportStateDoesNotContendWithOtherRequests(t *testing.T) {
	app, _ := importTestApp(t)
	app.installImportMu.Lock()
	defer app.installImportMu.Unlock()
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/installation-import/state", nil))
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"enabled":true`) {
		t.Fatal("read-only startup state contended with an import request", w.Code, w.Body.String())
	}
	if w := interfacePOST(app, "/tok/installation-import/skip", `{}`); w.Code != 409 {
		t.Fatal("concurrent mutation accepted", w.Code)
	}
}
