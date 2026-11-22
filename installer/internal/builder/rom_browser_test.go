package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
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
	checkBrowserRecent(t, app, "rom-browser", browserFolder{})
}

func TestBrowserCanGoAboveAnAliasAndReachTheDriveRoot(t *testing.T) {
	game := filepath.Join(t.TempDir(), "Game")
	if err := os.MkdirAll(filepath.Join(game, "Packs"), 0755); err != nil {
		t.Fatal(err)
	}
	app := newApplication(context.Background(), Options{ProjectRoot: game}, "tok")
	browse := func(query string) (folder, name string, parent *browserFolder) {
		t.Helper()
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/rom-browser?"+query, nil))
		var listing struct {
			Folder string
			Name   string
			Parent *browserFolder
		}
		if err := json.Unmarshal(w.Body.Bytes(), &listing); err != nil || w.Code != http.StatusOK {
			t.Fatalf("browse %s: %d %s (%v)", query, w.Code, w.Body, err)
		}
		return listing.Folder, listing.Name, listing.Parent
	}
	if _, name, parent := browse("location=game&path=Packs"); name != "Packs" || parent == nil ||
		*parent != (browserFolder{Location: "game", Path: "."}) {
		t.Fatalf("unexpected child name or parent: %q %+v", name, parent)
	}
	_, _, parent := browse("location=game")
	if parent == nil || parent.Location == "game" {
		t.Fatalf("Up cannot leave the game folder: %+v", parent)
	}
	query := url.Values{"location": {parent.Location}, "path": {parent.Path}}
	directory, _, _ := browse(query.Encode())
	want, err := filepath.EvalSymlinks(filepath.Dir(game))
	if err != nil || !strings.EqualFold(directory, want) {
		t.Fatalf("Up opened %q, wanted %q (%v)", directory, want, err)
	}
	if _, _, parent := browse("location=" + parent.Location); parent != nil {
		t.Fatalf("the drive root should lead to the Computer view: %+v", parent)
	}
}

func checkBrowserRecent(t *testing.T, app *application, endpoint string, want browserFolder) {
	t.Helper()
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/"+endpoint, nil))
	var result struct {
		Recent browserFolder `json:"recent"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &result); err != nil || w.Code != http.StatusOK || result.Recent != want {
		t.Fatalf("recent folder = %+v, want %+v (%v, %d %s)", result.Recent, want, err, w.Code, w.Body.String())
	}
}

func TestModBrowserInstallsValidatedO2R(t *testing.T) {
	root, source := t.TempDir(), t.TempDir()
	pack := testMusicMod(t, "controller-pack", []modTrack{{Track: "song-00", File: "audio/track.ogg"}}, "")
	if err := os.Mkdir(filepath.Join(source, "Music"), 0700); err != nil {
		t.Fatal(err)
	}
	for name, data := range map[string][]byte{
		"Music/Music.O2R": pack, "Broken.o2r": []byte("invalid archive"), ".hidden.o2r": pack, "Game.sfc": pack,
	} {
		if err := os.WriteFile(filepath.Join(source, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	link := os.Symlink(filepath.Join(source, "Music", "Music.O2R"), filepath.Join(root, "Linked.o2r")) == nil
	options := Options{ProjectRoot: root, ImportSearchDir: source}
	app := newApplication(context.Background(), options, "tok")
	listing := httptest.NewRecorder()
	app.ServeHTTP(listing, httptest.NewRequest("GET", "/tok/mods/browse?location=builder&path=Music", nil))
	if body := listing.Body.String(); listing.Code != http.StatusOK || !strings.Contains(body, "Music.O2R") ||
		strings.Contains(body, "Game.sfc") || strings.Contains(body, ".hidden.o2r") {
		t.Fatalf("listing = %d %s", listing.Code, body)
	}

	install := func(location, path string, code int) string {
		t.Helper()
		data, err := json.Marshal(map[string]string{"location": location, "path": path})
		if err != nil {
			t.Fatal(err)
		}
		r := httptest.NewRequest("POST", "/tok/mods/install-browsed", bytes.NewReader(data))
		r.Header.Set("Content-Type", "application/json")
		w := httptest.NewRecorder()
		app.ServeHTTP(w, r)
		if w.Code != code {
			t.Fatalf("%q = %d %s", path, w.Code, w.Body.String())
		}
		return w.Body.String()
	}
	for _, path := range []string{"../Music.O2R", "Game.sfc", ".hidden.o2r", "Broken.o2r"} {
		install("builder", path, http.StatusBadRequest)
	}
	if link {
		install("game", "Linked.o2r", http.StatusBadRequest)
	}
	checkBrowserRecent(t, app, "mods/browse", browserFolder{})
	if body := install("builder", "Music/Music.O2R", http.StatusOK); !strings.Contains(body, "controller-pack") ||
		!strings.Contains(body, musicModType) {
		t.Fatalf("installed catalog = %s", body)
	}
	if original, err := os.ReadFile(filepath.Join(source, "Music", "Music.O2R")); err != nil || !bytes.Equal(original, pack) {
		t.Fatal("install changed the selected archive")
	}
	reopened := newApplication(context.Background(), options, "tok")
	checkBrowserRecent(t, reopened, "mods/browse", browserFolder{Location: "builder", Path: "Music"})
	checkBrowserRecent(t, reopened, "rom-browser", browserFolder{})
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

func TestJapaneseLauncherROMRequiresUSABaseForNewBuild(t *testing.T) {
	directory := os.Getenv("AR_ROM_SET_DIR")
	if directory == "" {
		t.Skip("set AR_ROM_SET_DIR to a local ROM fixture directory")
	}
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(), BuildOnFirstRun: true,
		LaunchROMs: []string{filepath.Join(directory, "ar-jp.sfc")},
		Detect:     func() InstallState { return InstallState{CanRebuild: true} },
	}, "tok")
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/status", nil))
	var state status
	if err := json.Unmarshal(w.Body.Bytes(), &state); err != nil || w.Code != http.StatusOK {
		t.Fatalf("status: %d %s", w.Code, w.Body.String())
	}
	if !state.HasSelectedROMs || state.SelectedROM != "" || state.BuildOnFirstRun {
		t.Fatalf("a regional extra should be selected without enabling a first build: %+v", state)
	}
	for _, firstRun := range []string{"true", "false"} {
		w = httptest.NewRecorder()
		app.ServeHTTP(w, buildROMRequest(t, map[string]string{"useSelectedROMs": "true", "firstRun": firstRun}))
		if w.Code != http.StatusBadRequest || !strings.Contains(w.Body.String(), "ActRaiser (USA)") {
			t.Fatalf("firstRun=%s: expected a request for the USA base, got %d %s", firstRun, w.Code, w.Body.String())
		}
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
				checkBrowserRecent(t, app, "rom-browser", browserFolder{Location: "builder", Path: "."})
				checkBrowserRecent(t, app, "mods/browse", browserFolder{})
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

func writeJapaneseROMFontFixture(t *testing.T, root string) {
	t.Helper()
	for _, file := range []string{"NotoSansJP-Bold.otf", "NotoSansJP-OFL.txt"} {
		path := filepath.Join("game-assets", "fonts", "noto", file)
		data, err := os.ReadFile(filepath.Join("..", "..", "..", path))
		if err != nil {
			t.Fatal(err)
		}
		writeAssetTestFile(t, filepath.Join(root, path), data)
	}
}

func TestROMChecklistImportsLauncherAndFoundExtrasIntoExistingGame(t *testing.T) {
	directory := os.Getenv("AR_ROM_SET_DIR")
	if directory == "" {
		t.Skip("set AR_ROM_SET_DIR to a local ROM fixture directory")
	}
	root := t.TempDir()
	writeJapaneseROMFontFixture(t, root)
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
		Saved, TextReference, LanguagePack                bool
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
		if item := installed[id]; item.Media != "installed" || !item.TextReference || !item.LanguagePack {
			t.Errorf("missing regional content: %+v", item)
		}
	}
	index := runtimeLanguageIndex(t, root)
	for _, id := range []string{"jp", "eu-en", "fr", "de"} {
		if !strings.Contains(index, "replace\tsource."+id+"\t") {
			t.Errorf("missing optional runtime language source.%s", id)
		}
	}
	if probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE"); probe != "" {
		for _, line := range strings.Split(index, "\n") {
			fields := strings.Split(line, "\t")
			if len(fields) != 3 || fields[0] != "replace" {
				continue
			}
			manifest := filepath.Join(languagePacksRoot(filepath.Join(root, "game-assets")), ".arlang-cache",
				filepath.FromSlash(fields[2]), "pack.ini")
			if output, err := exec.Command(probe, manifest).CombinedOutput(); err != nil {
				t.Fatalf("runtime load %s: %v\n%s", fields[1], err, output)
			}
		}
	}
	if item := installed["ar2-us"]; item.Master != "installed" || item.World != "installed" {
		t.Errorf("missing sequel content: %+v", item)
	}
	if data, err := os.ReadFile(saved); err != nil || !bytes.Equal(data, core) {
		t.Fatal("optional import changed the saved build ROM")
	}

	listing := httptest.NewRecorder()
	app.ServeHTTP(listing, httptest.NewRequest("GET", "/tok/mods/catalog", nil))
	var catalog modCatalogView
	if err := json.Unmarshal(listing.Body.Bytes(), &catalog); err != nil || listing.Code != http.StatusOK {
		t.Fatalf("catalogue: %d %s (%v)", listing.Code, listing.Body, err)
	}
	for _, pack := range catalog.Languages {
		if pack.ID != "source.de" {
			continue
		}
		action, err := json.Marshal(modChange{Action: "uninstall", Filename: pack.Filename, Revision: pack.UninstallRevision})
		if err != nil {
			t.Fatal(err)
		}
		removed := httptest.NewRecorder()
		app.ServeHTTP(removed, httptest.NewRequest("POST", "/tok/mods/activation", bytes.NewReader(action)))
		if removed.Code != http.StatusOK {
			t.Fatalf("uninstall optional pack: %d %s", removed.Code, removed.Body)
		}
	}
	if err := prepareExistingROMMods(root, nil); err != nil {
		t.Fatal(err)
	}
	if regularFileExists(regionalROMModPath(root, "de")) {
		t.Fatal("startup restored an explicitly uninstalled regional mod")
	}
}

func TestROMChecklistUploadsMatchContentAndFeedBuild(t *testing.T) {
	directory := os.Getenv("AR_ROM_SET_DIR")
	if directory == "" {
		t.Skip("set AR_ROM_SET_DIR to a local ROM fixture directory")
	}
	core, err := os.ReadFile(filepath.Join(directory, "ar.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	german, err := os.ReadFile(filepath.Join(directory, "ar-ger.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	upload := func(slot string, files map[string][]byte) *httptest.ResponseRecorder {
		t.Helper()
		var body bytes.Buffer
		writer := multipart.NewWriter(&body)
		if err := writer.WriteField("id", slot); err != nil {
			t.Fatal(err)
		}
		for name, data := range files {
			part, err := writer.CreateFormFile("rom", name)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := part.Write(data); err != nil {
				t.Fatal(err)
			}
		}
		if err := writer.Close(); err != nil {
			t.Fatal(err)
		}
		r := httptest.NewRequest(http.MethodPost, "/tok/rom-selection", &body)
		r.Header.Set("Content-Type", writer.FormDataContentType())
		w := httptest.NewRecorder()
		app.ServeHTTP(w, r)
		return w
	}
	w := upload("", map[string][]byte{"misnamed-de.smc": core, "misnamed-us.sfc": german, "broken.sfc": []byte("bad")})
	choices := app.snapshotROMChoices()
	if w.Code != http.StatusOK || !strings.Contains(w.Body.String(), "broken.sfc") ||
		choices["us"].name != "misnamed-de.smc" || choices["de"].name != "misnamed-us.sfc" || len(choices) != 2 {
		t.Fatalf("drag-and-drop selection: status %d, response %s", w.Code, w.Body.String())
	}
	w = upload("us", map[string][]byte{"wrong-region.sfc": german})
	if w.Code != http.StatusBadRequest || app.snapshotROMChoices()["us"].name != "misnamed-de.smc" {
		t.Fatalf("wrong-region selection replaced the core: %d %s", w.Code, w.Body.String())
	}
	w = upload("us", map[string][]byte{"core.sfc": core})
	if w.Code != http.StatusOK || app.snapshotROMChoices()["us"].name != "core.sfc" {
		t.Fatalf("replacement was not selected: %d %s", w.Code, w.Body.String())
	}
	path, _, err := app.resolveBuildROM(&multipart.Form{Value: map[string][]string{"useSelectedROMs": {"true"}}})
	if err != nil {
		t.Fatal(err)
	}
	if saved, err := os.ReadFile(path); err != nil || !bytes.Equal(saved, core) {
		t.Fatal("build did not save the ROM selected through the checklist")
	}
}
