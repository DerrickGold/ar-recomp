package buildgui

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	lk "github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
	"github.com/DerrickGold/snesrecomp-go/internal/uicatalog"
)

func interfacePOST(app *application, path, data string) *httptest.ResponseRecorder {
	w := httptest.NewRecorder()
	r := httptest.NewRequest(http.MethodPost, path, strings.NewReader(data))
	r.Header.Set("Content-Type", "application/json")
	app.ServeHTTP(w, r)
	return w
}

func TestInterfacePreferenceIsIndependentAndDurable(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "first")
	prefs, err := app.readInterfacePreferences()
	if err != nil || prefs.Language != "en" {
		t.Fatalf("default: %+v %v", prefs, err)
	}
	if _, err := os.Stat(filepath.Join(root, "game-assets")); !os.IsNotExist(err) {
		t.Fatal("reading preferences must not create assets")
	}
	for _, language := range uicatalog.Locales() {
		w := interfacePOST(app, "/first/interface/preferences", `{"language":"`+language+`"}`)
		if w.Code != 200 {
			t.Fatal(w.Code, w.Body.String())
		}
		// Different process/session token and browser port, same local bundle.
		reopened := newApplication(context.Background(), Options{ProjectRoot: root}, "second")
		prefs, err := reopened.readInterfacePreferences()
		if err != nil || prefs.Language != language {
			t.Fatalf("reopen: %+v %v", prefs, err)
		}
		page := httptest.NewRecorder()
		reopened.ServeHTTP(page, httptest.NewRequest("GET", "/second/", nil))
		if page.Code != 200 || !strings.Contains(page.Body.String(), `"locale":"`+language+`"`) {
			t.Fatal("locale missing from bootstrap")
		}
	}
	files, err := os.ReadDir(filepath.Join(root, "game-assets"))
	if err != nil {
		t.Fatal(err)
	}
	if len(files) != 1 || files[0].Name() != "workshop-settings.json" {
		t.Fatal("UI choice created game packs/assets", files)
	}
	if _, err := os.Stat(filepath.Join(root, "settings.ini")); !os.IsNotExist(err) {
		t.Fatal("UI language modified game settings")
	}
}

func TestInterfacePreferenceRejectsInvalidRequestsWithoutMutation(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	if w := interfacePOST(app, "/tok/interface/preferences", `{"language":"fr"}`); w.Code != 200 {
		t.Fatal(w.Body.String())
	}
	for _, body := range []string{`{"language":"de-DE"}`, `{"language":"ja","path":"../settings.ini"}`, `{"language":"ja"} {}`, `null`, strings.Repeat("x", 1025)} {
		w := interfacePOST(app, "/tok/interface/preferences", body)
		if w.Code != 400 || !strings.Contains(w.Body.String(), `"errorCode":"builder.interface.invalid"`) {
			t.Fatal(w.Code, w.Body.String())
		}
		prefs, err := app.readInterfacePreferences()
		if err != nil || prefs.Language != "fr" {
			t.Fatal("invalid request mutated preference")
		}
	}
	if w := interfacePOST(app, "/interface/preferences", `{"language":"ja"}`); w.Code != 404 {
		t.Fatal("unscoped write allowed")
	}
	if w := interfacePOST(app, "/other/interface/preferences", `{"language":"ja"}`); w.Code != 404 {
		t.Fatal("wrong-token write allowed")
	}
	if w := interfacePOST(app, "/tok/interface/japanese.otf", `{}`); w.Code != 404 {
		t.Fatal("font mutation allowed")
	}
}

func TestInterfaceBootstrapAndBindings(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir(), Title: `{{UI_CATALOG}} <script>bad</script>`}, "tok")
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/", nil))
	if w.Code != 200 {
		t.Fatal(w.Body.String())
	}
	match := regexp.MustCompile(`<script type="application/json" id="interface-catalog">(.*?)</script>`).FindStringSubmatch(w.Body.String())
	if len(match) != 2 {
		t.Fatal("missing inert bootstrap")
	}
	var bootstrap struct {
		Locale   string
		Messages map[string][]string
	}
	if err := json.Unmarshal([]byte(match[1]), &bootstrap); err != nil {
		t.Fatal(err)
	}
	if bootstrap.Locale != "en" || len(bootstrap.Messages["interface.language"]) != 4 {
		t.Fatal("incomplete bootstrap")
	}
	if _, exists := bootstrap.Messages["overlay.title"]; exists {
		t.Fatal("builder fetched unused overlay catalog")
	}
	for _, phase := range buildPhases {
		if len(bootstrap.Messages["builder.phase."+phase.id]) != 4 {
			t.Errorf("missing phase translation: %s", phase.id)
		}
	}
	// Dynamic bindings are derived from authoritative IDs, never English labels.
	for _, track := range assetTracks {
		key := "builder.assets.track." + strings.ReplaceAll(track.ID, "-", "_")
		if texts := bootstrap.Messages[key]; len(texts) != 4 || texts[0] != track.Name {
			t.Errorf("missing/drifted track translation: %s", key)
		}
	}
	for _, region := range assetRegions {
		key := "builder.assets.region." + strings.ReplaceAll(region.Slug, "-", "_")
		if texts := bootstrap.Messages[key]; len(texts) != 4 || texts[0] != region.Label {
			t.Errorf("missing/drifted region translation: %s", key)
		}
	}
	for key, english := range lk.AuthorNavigationCaptions() {
		if texts := bootstrap.Messages["builder.navigation."+key]; len(texts) != 4 || texts[0] != english {
			t.Errorf("missing/drifted navigation caption: %s", key)
		}
	}
	if !strings.Contains(w.Body.String(), "&lt;script&gt;bad&lt;/script&gt;") {
		t.Fatal("title is not escaped")
	}
	if strings.Index(w.Body.String(), `src="builder/i18n.js"`) > strings.Index(w.Body.String(), `src="localization/editor.js"`) {
		t.Fatal("interface bootstrap must precede editor execution")
	}
	for _, name := range []string{"web/index.html", "web/app.js", "web/scene.js", "web/i18n.js", "localization.js"} {
		data, err := frontendFiles.ReadFile(name)
		if name == "web/index.html" {
			data = []byte(pageHTML)
			err = nil
		}
		if name == "localization.js" {
			data, err = []byte(localizationJS), nil
		}
		if err != nil {
			t.Fatal(err)
		}
		for _, match := range regexp.MustCompile(`"((?:builder|interface)\.[a-z0-9_.]+)"`).FindAllSubmatch(data, -1) {
			switch string(match[1]) {
			case "builder.phase.", "builder.assets.track.", "builder.assets.region.", "builder.navigation.":
				continue
			} // Completed from the authoritative IDs above.
			if len(bootstrap.Messages[string(match[1])]) != 4 {
				t.Errorf("%s references missing message %s", name, match[1])
			}
		}
	}
	for _, match := range regexp.MustCompile(`data-i18n(?:-[a-z]+)?="([a-z0-9_.]+)"`).FindAllStringSubmatch(w.Body.String(), -1) {
		if len(bootstrap.Messages[match[1]]) != 4 {
			t.Errorf("rendered HTML references missing message %s", match[1])
		}
	}
}

func TestInterfaceInvalidDiskPreferenceAndWriteFailure(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	path := app.interfacePreferencesPath()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	for _, data := range []string{"invalid", `{"language":"unavailable"}`, strings.Repeat("x", 1025)} {
		if err := os.WriteFile(path, []byte(data), 0644); err != nil {
			t.Fatal(err)
		}
		bootstrap, err := app.interfaceBootstrap()
		if err != nil || !strings.Contains(bootstrap, `"preferenceError":true`) || !strings.Contains(bootstrap, `"locale":"en"`) {
			t.Fatal("corrupt preference did not fall back", err)
		}
		got, _ := os.ReadFile(path)
		if string(got) != data {
			t.Fatal("GET overwrote invalid preference")
		}
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(path, 0755); err != nil {
		t.Fatal(err)
	}
	w := interfacePOST(app, "/tok/interface/preferences", `{"language":"ja"}`)
	if w.Code != 500 || !strings.Contains(w.Body.String(), `"errorCode":"builder.interface.save_failed"`) {
		t.Fatal(w.Code, w.Body.String())
	}
	if info, err := os.Stat(path); err != nil || !info.IsDir() {
		t.Fatal("replaced invalid target directory")
	}
}

func TestBrowserInterfaceBindings(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("optional JS checks need Node; never a builder dependency")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	if out, err := exec.CommandContext(ctx, node, "--test", "testdata/i18n.test.mjs", "testdata/localization_ui.test.mjs").CombinedOutput(); err != nil {
		t.Fatalf("interface JS: %v\n%s", err, out)
	}
}

func TestLocalizationErrorsSeparatePresentationFromDiagnosticText(t *testing.T) {
	for _, test := range []struct {
		err    error
		status int
		key    string
	}{
		{fmt.Errorf("invalid text: <raw> {name}"), 400, "builder.language.request_failed"},
		{fmt.Errorf("%w: edition <raw> changed", lk.ErrProjectConflict), 409, "builder.language.request_conflict"},
	} {
		w := httptest.NewRecorder()
		writeLocalizationError(w, test.err)
		var body map[string]string
		if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
			t.Fatal(err)
		}
		if w.Code != test.status || body["errorCode"] != test.key || body["error"] != test.err.Error() {
			t.Fatal(w.Code, body)
		}
	}
}
