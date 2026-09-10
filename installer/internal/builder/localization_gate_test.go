package builder

import (
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

// Complete semantic coverage with invented text; no ROM or retail wording in CI.
func completeLocalizationTestSource(t *testing.T) *lk.AuthorProject {
	t.Helper()
	m := localizationTestSource(t).Pack().Manifest().Metadata()
	m.Coverage = "complete"
	manifest, err := lk.NewPackManifest(m, lk.PackFonts{Primary: "builtin:actraiser-sans"}, []string{"text/source.artext"})
	if err != nil {
		t.Fatal(err)
	}
	refs, err := lk.AuthorReferences("us")
	if err != nil {
		t.Fatal(err)
	}
	var script strings.Builder
	for _, ref := range refs {
		fmt.Fprintf(&script, ":: %s\n", ref.ID)
		if ref.ID == "sky.action_mode.confirm" {
			script.WriteString("@anchor reset_text_cursor.00\nInvented US template.\n@anchor yield.01\n@end\n")
			continue
		}
		for _, anchor := range ref.Anchors {
			fmt.Fprintf(&script, "@anchor %s\n", anchor)
		}
		script.WriteString("@empty\n")
		script.WriteString("@end\n")
	}
	pack, err := lk.LoadAuthorPack(fstest.MapFS{"pack.ini": {Data: []byte(manifest.Text())}, "text/source.artext": {Data: []byte(script.String())}})
	if err != nil {
		t.Fatal(err)
	}
	p, err := lk.NewSourceProject(pack)
	if err != nil {
		t.Fatal(err)
	}
	return p
}

func installLocalizationCoverageSource(t *testing.T, app *application) {
	t.Helper()
	if _, err := lk.InstallNativeUSSource(filepath.Join(app.localizationRoot(), "native-us"), completeLocalizationTestSource(t).Pack()); err != nil {
		t.Fatal(err)
	}
	if _, err := app.localizationSource().snapshot(true); err != nil {
		t.Fatal(err)
	}
}

func localizationGateStatus(t *testing.T, app *application) status {
	t.Helper()
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/secret/status", nil))
	if w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	var s status
	if err := json.Unmarshal(w.Body.Bytes(), &s); err != nil {
		t.Fatal(err)
	}
	return s
}

func TestLocalizationGateUnlocksDuringBuildAndSurvivesLaterFailure(t *testing.T) {
	app := localizationTestApp(t)
	if s := localizationGateStatus(t, app); s.LocalizationReady || s.LocalizationError != "" {
		t.Fatal(s)
	}
	app.state = "building"
	app.log.WriteString("=== Preparing native US language source ===\n=== Regenerating banks ===\n")
	if s := localizationGateStatus(t, app); s.LocalizationReady {
		t.Fatal("log markers must not unlock the editor")
	}
	dir := filepath.Join(app.localizationRoot(), "native-us")
	if _, err := lk.InstallNativeUSSource(dir, completeLocalizationTestSource(t).Pack()); err != nil {
		t.Fatal(err)
	}
	if s := localizationGateStatus(t, app); s.State != "building" || !s.LocalizationReady {
		t.Fatal("waited for build completion", s)
	}
	first, _ := app.localizationSource().snapshot(false)
	s := localizationGateStatus(t, app)
	currentSource, _ := app.localizationSource().snapshot(false)
	if !s.LocalizationReady || first != currentSource {
		t.Fatal("reloaded unchanged source on a poll")
	}
	app.state = "failed"
	if s := localizationGateStatus(t, app); !s.LocalizationReady {
		t.Fatal("later compilation failure relocked editor", s)
	}
	if body := locGET(t, app, "state", nil).Body.String(); !strings.Contains(body, `"sourceAvailable":true`) {
		t.Fatal("editor and shell disagree", body)
	}
	// A new workshop recognizes it immediately, even without a ROM or game.
	other := localizationTestApp(t)
	other.options.ProjectRoot = app.options.ProjectRoot
	if s := localizationGateStatus(t, other); !s.LocalizationReady || s.State != "idle" {
		t.Fatal("existing source not recognized", s)
	}
	if _, err := os.Stat(filepath.Join(app.options.ProjectRoot, "user-rom.sfc")); !os.IsNotExist(err) {
		t.Fatal("readiness required a ROM")
	}
	// Removing only the publication marker locks navigation, without deleting
	// or changing a currently open project/reference.
	app.localization.current = localizationTestSource(t)
	current := app.localization.current
	if err := os.Rename(filepath.Join(dir, "pack.ini"), filepath.Join(dir, "saved.ini")); err != nil {
		t.Fatal(err)
	}
	if s := localizationGateStatus(t, app); s.LocalizationReady || app.localization.current != current {
		t.Fatal("missing source left gate open or lost the project")
	}
	if err := os.Rename(filepath.Join(dir, "saved.ini"), filepath.Join(dir, "pack.ini")); err != nil {
		t.Fatal(err)
	}
	if s := localizationGateStatus(t, app); !s.LocalizationReady || app.localization.current != current {
		t.Fatal("restored source not recognized")
	}
}

func TestLocalizationGateRejectsIncompleteAndInvalidSources(t *testing.T) {
	for _, kind := range []string{"partial", "wrong-id", "malformed", "missing-script", "symlink"} {
		t.Run(kind, func(t *testing.T) {
			app := localizationTestApp(t)
			dir := filepath.Join(app.localizationRoot(), "native-us")
			if err := os.MkdirAll(dir, 0755); err != nil {
				t.Fatal(err)
			}
			p := completeLocalizationTestSource(t)
			if kind == "partial" {
				p = localizationTestSource(t)
			}
			files := p.Pack().Files()
			if kind == "wrong-id" {
				files["pack.ini"] = []byte(strings.Replace(string(files["pack.ini"]), "id = native-us", "id = not-native", 1))
			}
			if kind == "malformed" {
				files["pack.ini"] = []byte("not a pack")
			}
			for name, data := range files {
				if kind == "missing-script" && name != "pack.ini" {
					continue
				}
				path := filepath.Join(dir, filepath.FromSlash(name))
				if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(path, data, 0644); err != nil {
					t.Fatal(err)
				}
			}
			if kind == "symlink" {
				if err := os.Rename(filepath.Join(dir, "pack.ini"), filepath.Join(dir, "source.ini")); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink("source.ini", filepath.Join(dir, "pack.ini")); err != nil {
					t.Skip(err)
				}
			}
			if s := localizationGateStatus(t, app); s.LocalizationReady || s.LocalizationError == "" {
				t.Fatal("invalid source unlocked editor", s)
			}
			if body := locGET(t, app, "state", nil).Body.String(); !strings.Contains(body, `"sourceAvailable":false`) {
				t.Fatal(body)
			}
		})
	}
}
