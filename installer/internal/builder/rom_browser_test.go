package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestROMBrowserListsOnlyFoldersAndROMsWithinAChosenLocation(t *testing.T) {
	root := t.TempDir()
	outside := t.TempDir()
	for _, file := range []string{"My ROM.smc", "private.txt", ".hidden.sfc"} {
		if err := os.WriteFile(filepath.Join(root, file), []byte("fixture"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(root, "ROM folder"), 0700); err != nil {
		t.Fatal(err)
	}
	// Windows may require Developer Mode or elevated privileges to create links.
	link := os.Symlink(outside, filepath.Join(root, "outside")) == nil
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	for _, test := range []struct {
		query string
		code  int
	}{
		{"location=game", http.StatusOK},
		{"location=game&path=..", http.StatusBadRequest},
		{"location=unknown", http.StatusBadRequest},
	} {
		got := httptest.NewRecorder()
		app.ServeHTTP(got, httptest.NewRequest("GET", "/tok/rom-browser?"+test.query, nil))
		if got.Code != test.code {
			t.Fatalf("%s: %d %s", test.query, got.Code, got.Body.String())
		}
		if test.code == http.StatusOK {
			body := got.Body.String()
			if !strings.Contains(body, "My ROM.smc") || !strings.Contains(body, "ROM folder") ||
				strings.Contains(body, "private.txt") || strings.Contains(body, ".hidden.sfc") || strings.Contains(body, "outside") {
				t.Fatalf("unexpected entries: %s", body)
			}
		}
	}
	if link {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/rom-browser?location=game&path=outside", nil))
		if w.Code != http.StatusBadRequest {
			t.Fatalf("external symbolic link accepted: %d %s", w.Code, w.Body.String())
		}
	}
	selection := httptest.NewRequest("POST", "/tok/rom-selection", strings.NewReader(`{"id":"us","location":"game","path":"My ROM.smc"}`))
	selection.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	app.ServeHTTP(w, selection)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("invalid ROM accepted: %d %s", w.Code, w.Body.String())
	}
}

func buildROMRequest(t *testing.T, fields map[string]string) *http.Request {
	t.Helper()
	body := new(bytes.Buffer)
	form := multipart.NewWriter(body)
	for name, value := range fields {
		if err := form.WriteField(name, value); err != nil {
			t.Fatal(err)
		}
	}
	if err := form.Close(); err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest("POST", "/tok/build", body)
	r.Header.Set("Content-Type", form.FormDataContentType())
	return r
}

func TestFirstBuildRejectsBadOrUnnecessaryLauncherROMs(t *testing.T) {
	for _, test := range []struct {
		name           string
		existing, used bool
		want           string
	}{
		{name: "invalid ROM", want: "builder.build.launch_rom_invalid"},
		{name: "existing build", existing: true, want: "existing game or saved rom"},
		{name: "saved ROM", used: true, want: "existing game or saved rom"},
	} {
		t.Run(test.name, func(t *testing.T) {
			root := t.TempDir()
			rom := filepath.Join(t.TempDir(), "Provided ROM.sfc")
			if err := os.WriteFile(rom, []byte("not a game ROM"), 0600); err != nil {
				t.Fatal(err)
			}
			if test.used {
				if err := os.WriteFile(filepath.Join(root, "user-rom.sfc"), []byte("saved"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			app := newApplication(context.Background(), Options{
				ProjectRoot: root, LaunchROMs: []string{rom}, BuildOnFirstRun: true,
				Detect: func() InstallState { return InstallState{CanLaunch: test.existing, CanRebuild: true} },
			}, "tok")
			w := httptest.NewRecorder()
			app.ServeHTTP(w, buildROMRequest(t, map[string]string{"useSelectedROMs": "true", "firstRun": "true"}))
			if w.Code != http.StatusBadRequest || !strings.Contains(strings.ToLower(w.Body.String()), test.want) {
				t.Fatalf("status = %d, response = %s", w.Code, w.Body.String())
			}
			info, err := os.ReadFile(filepath.Join(root, "user-rom.sfc"))
			if test.used && (err != nil || string(info) != "saved") || !test.used && !os.IsNotExist(err) {
				t.Fatal("the local ROM changed")
			}
		})
	}
}

func TestControllerAndLauncherROMSelectionsReachBuild(t *testing.T) {
	fixture := os.Getenv("AR_BUILDER_TEST_ROM")
	if fixture == "" {
		t.Skip("set AR_BUILDER_TEST_ROM to a local ActRaiser (USA) ROM")
	}
	data, err := os.ReadFile(fixture)
	if err != nil {
		t.Fatal(err)
	}
	for _, source := range []string{"launcher", "browser"} {
		t.Run(source, func(t *testing.T) {
			root, directory := t.TempDir(), t.TempDir()
			name := "ActRaiser USA test.smc"
			rom := filepath.Join(directory, name)
			if err := os.WriteFile(rom, data, 0600); err != nil {
				t.Fatal(err)
			}
			built := make(chan string, 1)
			options := Options{
				ProjectRoot: root, ImportSearchDir: directory, BuildOnFirstRun: true,
				Detect: func() InstallState { return InstallState{CanRebuild: true} },
				Build: func(_ context.Context, path string, _ io.Writer) (Result, error) {
					built <- path
					return Result{}, nil
				},
			}
			fields := map[string]string{"useSelectedROMs": "true"}
			if source == "launcher" {
				options.LaunchROMs = []string{rom}
				fields["firstRun"] = "true"
			}
			app := newApplication(context.Background(), options, "tok")
			if source == "browser" {
				picked := httptest.NewRecorder()
				selection := httptest.NewRequest("POST", "/tok/rom-selection", strings.NewReader(`{"id":"us","location":"builder","path":"`+name+`"}`))
				selection.Header.Set("Content-Type", "application/json")
				app.ServeHTTP(picked, selection)
				if picked.Code != http.StatusOK {
					t.Fatalf("selection: %d %s", picked.Code, picked.Body.String())
				}
			}
			w := httptest.NewRecorder()
			app.ServeHTTP(w, buildROMRequest(t, fields))
			if w.Code != http.StatusAccepted {
				t.Fatalf("status = %d, response = %s", w.Code, w.Body.String())
			}
			select {
			case path := <-built:
				if path != filepath.Join(root, "user-rom.sfc") {
					t.Fatalf("build ROM = %q", path)
				}
			case <-time.After(time.Second):
				t.Fatal("build callback not reached")
			}
			if original, err := os.ReadFile(rom); err != nil || !bytes.Equal(data, original) {
				t.Fatal("the original ROM changed")
			}
		})
	}
}

func TestROMChecklistImportsLauncherAndFoundExtrasIntoExistingGame(t *testing.T) {
	directory := os.Getenv("AR_ROM_SET_DIR")
	if directory == "" {
		t.Skip("set AR_ROM_SET_DIR to a local ROM fixture directory")
	}
	root := t.TempDir()
	core, err := os.ReadFile(filepath.Join(directory, "ar.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	saved := filepath.Join(root, "user-rom.sfc")
	if err := os.WriteFile(saved, core, 0600); err != nil {
		t.Fatal(err)
	}
	german, err := os.ReadFile(filepath.Join(directory, "ar-ger.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "extra.smc"), german, 0600); err != nil {
		t.Fatal(err)
	}
	options := Options{
		ProjectRoot: root, ImportSearchDir: root, BuildOnFirstRun: true,
		Detect: func() InstallState { return InstallState{CanLaunch: true, CanRebuild: true} },
	}
	for _, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-fra.sfc", "ar2.sfc"} {
		path, err := filepath.Abs(filepath.Join(directory, name))
		if err != nil {
			t.Fatal(err)
		}
		options.LaunchROMs = append(options.LaunchROMs, path)
	}
	app := newApplication(context.Background(), options, "tok")
	type row struct {
		ID, Selected, Origin, Found, Media, Master, World string
		Saved, TextReference                              bool
	}
	checklist := func() map[string]row {
		t.Helper()
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/rom-checklist", nil))
		var inventory struct{ ROMs []row }
		if err := json.Unmarshal(w.Body.Bytes(), &inventory); w.Code != http.StatusOK || err != nil || len(inventory.ROMs) != 6 {
			t.Fatalf("checklist: %d %s", w.Code, w.Body.String())
		}
		rows := make(map[string]row)
		for _, item := range inventory.ROMs {
			rows[item.ID] = item
		}
		return rows
	}
	initial := checklist()
	if !initial["us"].Saved || initial["us"].Origin != "launcher" || initial["fr"].Origin != "launcher" || initial["de"].Found != "extra.smc" || initial["de"].Selected != "" {
		t.Fatalf("initial inventory: %+v", initial)
	}
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/status", nil))
	var current status
	if err := json.Unmarshal(w.Body.Bytes(), &current); w.Code != http.StatusOK || err != nil ||
		current.RebuildROM != saved || current.SelectedROM != "" || !current.HasSelectedROMs || current.BuildOnFirstRun {
		t.Fatalf("status should prefer the saved core and keep the checklist selection: %d %s", w.Code, w.Body.String())
	}
	w = httptest.NewRecorder()
	wrongSlot := httptest.NewRequest("POST", "/tok/rom-selection", strings.NewReader(`{"id":"us","location":"game","path":"extra.smc"}`))
	wrongSlot.Header.Set("Content-Type", "application/json")
	app.ServeHTTP(w, wrongSlot)
	if w.Code != http.StatusBadRequest || !strings.Contains(w.Body.String(), "different entry") || checklist()["us"].Origin != "launcher" {
		t.Fatalf("the core picker should preserve its selection after a regional ROM is chosen: %d %s", w.Code, w.Body.String())
	}
	w = httptest.NewRecorder()
	selection := httptest.NewRequest("POST", "/tok/rom-selection", strings.NewReader(`{"id":"de","found":true}`))
	selection.Header.Set("Content-Type", "application/json")
	app.ServeHTTP(w, selection)
	if w.Code != http.StatusOK || checklist()["de"].Selected != "extra.smc" {
		t.Fatalf("found ROM selection: %d %s", w.Code, w.Body.String())
	}
	w = httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("POST", "/tok/rom-import-selected", nil))
	if w.Code != http.StatusOK {
		t.Fatalf("import: %d %s", w.Code, w.Body.String())
	}
	installed := checklist()
	for _, id := range []string{"jp", "eu-en", "fr", "de"} {
		if item := installed[id]; item.Media != "installed" || !item.TextReference {
			t.Errorf("missing regional content: %+v", item)
		}
	}
	if item := installed["ar2-us"]; item.Master != "installed" || item.World != "installed" {
		t.Errorf("missing sequel content: %+v", item)
	}
	if data, err := os.ReadFile(saved); err != nil || !bytes.Equal(data, core) {
		t.Fatal("optional import changed the saved build ROM")
	}
}
