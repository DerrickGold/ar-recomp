package builder

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/json"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

func testModAudio(t *testing.T) []byte {
	t.Helper()
	data, err := os.ReadFile("../../../tests/fixtures/audio/stream-probe.ogg")
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func testMusicMod(t *testing.T, id string, tracks []modTrack, extra string) []byte {
	t.Helper()
	var output bytes.Buffer
	archive := zip.NewWriter(&output)
	payload := modPayload{Format: 1, Type: musicModType, ID: id, Game: modGameID, Tracks: tracks}
	for name, value := range map[string]any{
		"manifest.json": modMetadata{Name: id, Version: "1.0.0", Author: "Fixture"},
		"pack.json":     payload,
	} {
		file, err := archive.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Store})
		if err != nil {
			t.Fatal(err)
		}
		if err := json.NewEncoder(file).Encode(value); err != nil {
			t.Fatal(err)
		}
	}
	seen := map[string]bool{}
	for _, track := range tracks {
		if seen[track.File] {
			continue
		}
		seen[track.File] = true
		file, err := archive.Create(track.File)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := file.Write(testModAudio(t)); err != nil {
			t.Fatal(err)
		}
	}
	if extra != "" {
		if _, err := archive.Create(extra); err != nil {
			t.Fatal(err)
		}
	}
	if err := archive.Close(); err != nil {
		t.Fatal(err)
	}
	return output.Bytes()
}

func TestMusicModConflictsAndExternalRemoval(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	legacy := []byte("[music:title-theme]\nsrc = 1A:94B8\nfile = audio/title.ogg\n")
	if err := os.Mkdir(filepath.Join(root, "game-assets"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(liveAssetManifestPath(root), legacy, 0644); err != nil {
		t.Fatal(err)
	}
	track := modTrack{Track: "song-09", Region: "fillmore", File: "audio/shared.ogg"}
	files := []string{"first.o2r", "second.o2r"}
	for i, file := range files {
		id := strings.TrimSuffix(file, ".o2r")
		tracks := []modTrack{track}
		if i == 0 {
			tracks = append(tracks, modTrack{Track: "title-theme", File: track.File})
		}
		if err := os.WriteFile(filepath.Join(external, file), testMusicMod(t, id, tracks, ""), 0644); err != nil {
			t.Fatal(err)
		}
	}
	update := func(dirs []string, change *modChange, want string, count int) {
		t.Helper()
		catalog, err := updateModCatalog(root, dirs, nil, change)
		if err != nil {
			t.Fatal(err)
		}
		index := slices.IndexFunc(catalog.Slots, func(slot modSlot) bool { return slot.ID == track.target() })
		if index < 0 || catalog.Slots[index].Winner != want || len(catalog.Slots[index].Candidates) != count {
			t.Fatalf("unexpected slots: %+v", catalog.Slots)
		}
	}
	update([]string{external}, nil, "", 2)
	overlayPath := filepath.Join(root, "game-assets", modOverlayName)
	data, err := os.ReadFile(overlayPath)
	if err != nil || !bytes.Contains(data, []byte("[music:title-theme]")) || bytes.Contains(data, []byte("[music:song-09-fillmore]")) {
		t.Fatalf("unexpected unresolved overlay: %v\n%s", err, data)
	}
	update(nil, &modChange{Action: "select", Pack: "second", Slot: track.target()}, "second", 2)
	update(nil, &modChange{Action: "disable", Pack: "second"}, "first", 1)
	if _, err := os.Stat(filepath.Join(external, files[1])); err != nil {
		t.Fatalf("disable touched external archive: %v", err)
	}
	update(nil, &modChange{Action: "enable", Pack: "second"}, "second", 2)
	if err := os.Remove(filepath.Join(external, files[0])); err != nil {
		t.Fatal(err)
	}
	update(nil, nil, "second", 1)
	if data, err := os.ReadFile(liveAssetManifestPath(root)); err != nil || !bytes.Equal(data, legacy) {
		t.Fatal("legacy manifest changed", err)
	}
}

func TestPerAssetChoicesSurviveRescan(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	title := modTrack{Track: "title-theme", File: "audio/theme.ogg"}
	regional := modTrack{Track: "song-09", Region: "fillmore", File: "audio/fillmore.ogg"}
	solo := testMusicMod(t, "solo", []modTrack{title}, "")
	suite := testMusicMod(t, "suite", []modTrack{title, regional}, "")
	soloPath, suitePath := filepath.Join(external, "solo.o2r"), filepath.Join(external, "suite.o2r")
	if err := os.WriteFile(soloPath, solo, 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := updateModCatalog(root, []string{external}, nil, nil); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(suitePath, suite, 0644); err != nil {
		t.Fatal(err)
	}

	check := func(change *modChange, titleWinner, regionWinner string) {
		t.Helper()
		catalog, err := updateModCatalog(root, nil, nil, change)
		if err != nil {
			t.Fatal(err)
		}
		overlay, err := os.ReadFile(filepath.Join(root, "game-assets", modOverlayName))
		if err != nil {
			t.Fatal(err)
		}
		for target, winner := range map[string]string{title.target(): titleWinner, regional.target(): regionWinner} {
			i := slices.IndexFunc(catalog.Slots, func(slot modSlot) bool { return slot.ID == target })
			if winner == "" {
				if i >= 0 || bytes.Contains(overlay, []byte("[music:"+target+"]")) {
					t.Fatalf("expected manifest fallback for %s, slots %+v", target, catalog.Slots)
				}
				continue
			}
			if i < 0 || catalog.Slots[i].Winner != winner {
				t.Fatalf("wanted %s for %s, slots %+v", winner, target, catalog.Slots)
			}
			for _, pack := range catalog.Packs {
				if pack.ID != winner {
					continue
				}
				file := pack.cacheFilename(pack.digest, slices.Index(pack.Targets, target))
				_, rest, found := strings.Cut(string(overlay), "[music:"+target+"]")
				section, _, _ := strings.Cut(rest, "\n[")
				if !found || !strings.Contains(section, "file = mods-cache/"+file+"\n") {
					t.Fatalf("expected %s in %s", file, section)
				}
			}
		}
	}
	check(nil, "solo", "suite")
	check(&modChange{Action: "select", Pack: "suite", Slot: title.target()}, "suite", "suite")
	check(&modChange{Action: "exclude-target", Pack: "suite", Slot: title.target()}, "solo", "suite")
	check(&modChange{Action: "exclude-target", Pack: "suite", Slot: regional.target()}, "solo", "")
	check(nil, "solo", "")
	if _, err := updateModCatalog(root, nil, nil, &modChange{Action: "select", Pack: "suite", Slot: title.target()}); err == nil {
		t.Fatal("selected an excluded asset")
	}
	if _, err := updateModCatalog(root, nil, nil, &modChange{Action: "exclude-target", Pack: "solo", Slot: regional.target()}); err == nil {
		t.Fatal("excluded an asset the pack does not supply")
	}
	check(&modChange{Action: "disable", Pack: "suite"}, "solo", "")
	check(&modChange{Action: "enable", Pack: "suite"}, "solo", "")
	check(&modChange{Action: "include-target", Pack: "suite", Slot: title.target()}, "suite", "")
	check(&modChange{Action: "include-target", Pack: "suite", Slot: regional.target()}, "suite", "suite")
	check(&modChange{Action: "select", Pack: "solo", Slot: title.target()}, "solo", "suite")
	for path, original := range map[string][]byte{soloPath: solo, suitePath: suite} {
		if data, err := os.ReadFile(path); err != nil || !bytes.Equal(data, original) {
			t.Fatalf("activation changed %s: %v", path, err)
		}
	}
}

func TestInstalledSharedTargetNeedsExplicitChoice(t *testing.T) {
	root := t.TempDir()
	theme := modTrack{Track: "title-theme", File: "audio/title.ogg"}
	install := func(id string, tracks ...modTrack) modCatalog {
		t.Helper()
		catalog, err := updateModCatalog(root, nil, bytes.NewReader(testMusicMod(t, id, tracks, "")), nil)
		if err != nil {
			t.Fatal(err)
		}
		return catalog
	}
	update := func(change *modChange) modCatalog {
		t.Helper()
		catalog, err := updateModCatalog(root, nil, nil, change)
		if err != nil {
			t.Fatal(err)
		}
		return catalog
	}
	check := func(catalog modCatalog, winner string, needsChoice bool) {
		t.Helper()
		for _, slot := range catalog.Slots {
			if slot.ID == theme.target() {
				if slot.Winner != winner || slot.NeedsChoice != needsChoice {
					t.Fatalf("expected %s, needsChoice=%t, got %+v", winner, needsChoice, slot)
				}
			} else if slot.NeedsChoice {
				t.Fatalf("unrelated track flagged: %+v", slot)
			}
		}
		if !slices.ContainsFunc(catalog.Slots, func(slot modSlot) bool { return slot.ID == theme.target() }) {
			t.Fatal("missing title theme")
		}
	}
	selectTheme := func(id string) modCatalog {
		return update(&modChange{Action: "select", Pack: id, Slot: theme.target()})
	}
	check(install("solo", theme), "solo", false)
	check(install("suite", theme, modTrack{Track: "song-09", Region: "fillmore", File: "audio/fillmore.ogg"}), "solo", true)
	check(selectTheme("solo"), "solo", false)
	check(update(nil), "solo", false)
	check(install("third", theme), "solo", true)
	check(update(&modChange{Action: "disable", Pack: "third"}), "solo", false)
	check(update(&modChange{Action: "enable", Pack: "third"}), "solo", true)
	check(selectTheme("suite"), "suite", false)
	check(update(nil), "suite", false)
}

func TestModIndexReusesAndRepairsCachedAssets(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	path := filepath.Join(external, "music.o2r")
	track := modTrack{Track: "title-theme", File: "audio/title.ogg"}
	if err := os.WriteFile(path, testMusicMod(t, "sample", []modTrack{track}, ""), 0644); err != nil {
		t.Fatal(err)
	}
	list := func() installedMod {
		t.Helper()
		catalog, err := updateModCatalog(root, []string{external}, nil, nil)
		if err != nil || len(catalog.Packs) != 1 || !catalog.Packs[0].Enabled {
			t.Fatalf("unexpected catalog: %+v (%v)", catalog, err)
		}
		return catalog.Packs[0]
	}
	pack := list()

	store, unlock, err := openModStore(root, nil)
	if err != nil {
		t.Fatal(err)
	}
	entry := store.index[path]
	prepared, err := store.prepareArchive(path, entry)
	unlock()
	if err != nil {
		t.Fatal(err)
	}
	if prepared.Source.usable() && (entry == nil || prepared != entry) {
		t.Fatal("unchanged archive did not reuse its persisted validation")
	}
	asset := filepath.Join(store.cache, pack.cacheFilename(pack.digest, 0))
	if err := os.Remove(asset); err != nil {
		t.Fatal(err)
	}
	if repaired := list(); repaired.digest != pack.digest {
		t.Fatal("repair changed the pack digest")
	}
	if got, err := os.ReadFile(asset); err != nil || !bytes.Equal(got, testModAudio(t)) {
		t.Fatalf("missing cached asset was not restored: %v", err)
	}
	if err := os.WriteFile(filepath.Join(store.cache, modIndexName), []byte("{"), 0644); err != nil {
		t.Fatal(err)
	}
	if recovered := list(); recovered.digest != pack.digest {
		t.Fatal("corrupt index changed the pack digest")
	}
	if prepared.Source.usable() && readModIndex(store.cache)[path] == nil {
		t.Fatal("corrupt index was not rebuilt")
	}

	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	catalog, err := updateModCatalog(root, nil, nil, nil)
	if err != nil || len(catalog.Packs) != 0 || len(catalog.Slots) != 0 || len(readModIndex(store.cache)) != 0 {
		t.Fatalf("removed archive is still indexed or active: %+v (%v)", catalog, err)
	}
}

func TestModIndexDetectsSameSizeUpdatesWithPreservedMtime(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	path := filepath.Join(external, "music.o2r")
	staged := filepath.Join(external, "incoming")
	track := modTrack{Track: "title-theme", File: "audio/title.ogg"}
	mtime := time.Unix(1700000000, 0)
	size := len(testMusicMod(t, "first", []modTrack{track}, ""))
	for i, id := range []string{"first", "edits", "third"} {
		data := testMusicMod(t, id, []modTrack{track}, "")
		if len(data) != size {
			t.Fatal("fixture must preserve the archive size")
		}
		target := path
		if i == 2 {
			target = staged
		}
		if err := os.WriteFile(target, data, 0644); err != nil {
			t.Fatal(err)
		}
		if err := os.Chtimes(target, mtime, mtime); err != nil {
			t.Fatal(err)
		}
		if target == staged {
			if err := os.Rename(staged, path); err != nil {
				t.Fatal(err)
			}
		}
		catalog, err := updateModCatalog(root, []string{external}, nil, nil)
		if err != nil || len(catalog.Packs) != 1 || catalog.Packs[0].ID != id ||
			len(catalog.Slots) != 1 || catalog.Slots[0].Winner != id {
			t.Fatalf("archive update to %q was not detected: %+v (%v)", id, catalog, err)
		}
	}
}

func TestMusicModExportInstallRoundTrip(t *testing.T) {
	root := t.TempDir()
	target := musicModTargets["song-09-fillmore"]
	audioPath := filepath.Join(root, "game-assets", "audio", "custom.ogg")
	if err := os.MkdirAll(filepath.Dir(audioPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(audioPath, testModAudio(t), 0644); err != nil {
		t.Fatal(err)
	}
	manifest := "[music:song-09-fillmore]\nsrc = " + target.Song.Src + "\nwhen = " + target.Gate +
		"\nfile = audio/custom.ogg\nloop = 0\ngain = 75\n"
	if err := os.WriteFile(liveAssetManifestPath(root), []byte(manifest), 0644); err != nil {
		t.Fatal(err)
	}
	request := modExportRequest{
		modMetadata: modMetadata{Name: "Test pack", Author: "Fixture", Version: "1.0.0"},
		ID:          "test-pack", Targets: []string{"song-09-fillmore"},
	}
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "token")
	defer app.replaceModDownload(nil)
	body, err := json.Marshal(request)
	if err != nil {
		t.Fatal(err)
	}
	export := httptest.NewRecorder()
	app.ServeHTTP(export, httptest.NewRequest(http.MethodPost, "/token/mods/export", bytes.NewReader(body)))
	var prepared map[string]string
	if err := json.Unmarshal(export.Body.Bytes(), &prepared); err != nil || export.Code != http.StatusOK || !strings.HasPrefix(prepared["name"], "music-") {
		t.Fatalf("export failed: %d %s", export.Code, export.Body)
	}
	prefix := httptest.NewRecorder()
	requestRange := httptest.NewRequest(http.MethodGet, "/token/"+prepared["url"], nil)
	requestRange.Header.Set("Range", "bytes=0-3")
	app.ServeHTTP(prefix, requestRange)
	if prefix.Code != http.StatusPartialContent || prefix.Body.Len() != 4 {
		t.Fatalf("download range: %d %s", prefix.Code, prefix.Body)
	}
	download := httptest.NewRecorder()
	app.ServeHTTP(download, httptest.NewRequest(http.MethodGet, "/token/"+prepared["url"], nil))
	if download.Code != http.StatusOK || !strings.Contains(download.Header().Get("Content-Disposition"), prepared["name"]) {
		t.Fatalf("download failed: %d %s", download.Code, download.Body)
	}
	targetRoot := t.TempDir()
	for i := 0; i < 2; i++ {
		catalog, err := updateModCatalog(targetRoot, nil, bytes.NewReader(download.Body.Bytes()), nil)
		if err != nil || len(catalog.Packs) != 1 || len(catalog.Slots) != 1 || catalog.Slots[0].Winner != strings.TrimSuffix(prepared["name"], ".o2r") {
			t.Fatalf("install %d: %+v %v", i, catalog, err)
		}
	}
	overlay, err := os.ReadFile(filepath.Join(targetRoot, "game-assets", modOverlayName))
	if err != nil || !bytes.Contains(overlay, []byte("loop = 0\ngain = 75")) {
		t.Fatalf("export lost playback settings: %v\n%s", err, overlay)
	}
	recipient := newApplication(context.Background(), Options{ProjectRoot: targetRoot}, "recipient")
	list := httptest.NewRecorder()
	recipient.ServeHTTP(list, httptest.NewRequest(http.MethodGet, "/recipient/mods/catalog", nil))
	var catalog modCatalog
	if err := json.Unmarshal(list.Body.Bytes(), &catalog); err != nil || list.Code != http.StatusOK ||
		len(catalog.Packs) != 1 || catalog.Packs[0].ArchiveURL == "" {
		t.Fatalf("recipient catalog: %d %s %v", list.Code, list.Body, err)
	}
	copy := httptest.NewRecorder()
	recipient.ServeHTTP(copy, httptest.NewRequest(http.MethodGet, "/recipient/"+catalog.Packs[0].ArchiveURL, nil))
	if copy.Code != http.StatusOK || !bytes.Equal(copy.Body.Bytes(), download.Body.Bytes()) {
		t.Fatalf("Save .o2r copy did not return the installed archive: %d", copy.Code)
	}
}

func TestMusicModRejectsNonAssetMembers(t *testing.T) {
	track := modTrack{Track: "title-theme", File: "audio/title.ogg"}
	for _, member := range []string{"../outside.ogg", "binaries/plugin.so", "build.gen", "Audio/TITLE.OGG"} {
		t.Run(member, func(t *testing.T) {
			archive := testMusicMod(t, "test", []modTrack{track}, member)
			if _, err := readModArchive(bytes.NewReader(archive), int64(len(archive))); err == nil {
				t.Fatal("accepted disallowed member")
			}
		})
	}
}

func TestModCLIInstallsIntoFreshGameOutput(t *testing.T) {
	root := t.TempDir()
	archive := filepath.Join(root, "test.o2r")
	track := modTrack{Track: "title-theme", File: "audio/title.ogg"}
	if err := os.WriteFile(archive, testMusicMod(t, "sample", []modTrack{track}, ""), 0644); err != nil {
		t.Fatal(err)
	}
	output := filepath.Join(root, "fresh-game")
	var stdout, stderr bytes.Buffer
	code := RunMods([]string{"install", "--json", "--output-dir", output, archive}, &stdout, &stderr)
	var result modCLIState
	err := json.Unmarshal(stdout.Bytes(), &result)
	if code != 0 || err != nil || len(result.Packs) != 1 || result.Packs[0].ID != "sample" || !result.Packs[0].Enabled {
		t.Fatalf("install exited %d: %s %s (%v)", code, stdout.String(), stderr.String(), err)
	}
	if strings.Contains(stdout.String(), "\"tracks\"") {
		t.Fatalf("generic CLI returned a music-specific payload: %s", stdout.String())
	}
}

func testTitlePNG(t *testing.T, red uint8) []byte {
	t.Helper()
	art := image.NewNRGBA(image.Rect(0, 0, 237, 95))
	art.SetNRGBA(0, 0, color.NRGBA{R: red, A: 255})
	var output bytes.Buffer
	if err := png.Encode(&output, art); err != nil {
		t.Fatal(err)
	}
	return output.Bytes()
}

func TestTitleModExportConflictAndLegacyFallback(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "token")
	defer app.replaceModDownload(nil)
	assets := filepath.Join(root, "game-assets")
	if err := os.Mkdir(assets, 0755); err != nil {
		t.Fatal(err)
	}
	legacy := []byte("[replace:title-logo]\nimage = hd/legacy.png\n[replace:title-swirl]\nimage = hd/legacy.png\n")
	if err := os.WriteFile(liveAssetManifestPath(root), legacy, 0644); err != nil {
		t.Fatal(err)
	}
	install := func(id string, art []byte) modCatalog {
		t.Helper()
		var body bytes.Buffer
		form := multipart.NewWriter(&body)
		for name, value := range map[string]string{"id": id, "name": id, "version": "1.0", "author": "Fixture"} {
			if err := form.WriteField(name, value); err != nil {
				t.Fatal(err)
			}
		}
		input, err := form.CreateFormFile("artwork", "title.png")
		if err != nil {
			t.Fatal(err)
		}
		if _, err := input.Write(art); err != nil {
			t.Fatal(err)
		}
		if err := form.Close(); err != nil {
			t.Fatal(err)
		}
		export := httptest.NewRecorder()
		request := httptest.NewRequest(http.MethodPost, "/token/mods/export-title", &body)
		request.Header.Set("Content-Type", form.FormDataContentType())
		app.ServeHTTP(export, request)
		var prepared map[string]string
		if err := json.Unmarshal(export.Body.Bytes(), &prepared); err != nil || export.Code != http.StatusOK {
			t.Fatalf("export failed: %d %s", export.Code, export.Body)
		}
		download := httptest.NewRecorder()
		app.ServeHTTP(download, httptest.NewRequest(http.MethodGet, "/token/"+prepared["url"], nil))
		if download.Code != http.StatusOK {
			t.Fatalf("download failed: %d %s", download.Code, download.Body)
		}
		catalog, err := updateModCatalog(root, nil, download.Body, nil)
		if err != nil || catalog.InstalledType != titleModType {
			t.Fatalf("install failed: %+v %v", catalog, err)
		}
		return catalog
	}
	assertTitle := func(catalog modCatalog, winner string, art []byte) {
		t.Helper()
		overlay, _, err := readAssetManifest(root)
		if err != nil || overlay != string(legacy) {
			t.Fatalf("legacy manifest changed: %v\n%s", err, overlay)
		}
		slot := slices.IndexFunc(catalog.Slots, func(slot modSlot) bool { return slot.ID == titleModTarget })
		overlayBytes, err := os.ReadFile(filepath.Join(assets, modOverlayName))
		if err != nil {
			t.Fatal(err)
		}
		generated := string(overlayBytes)
		logo, logoOK := manifestSectionValue(generated, "replace:title-logo", "image")
		swirl, swirlOK := manifestSectionValue(generated, "replace:title-swirl", "image")
		if winner == "" {
			if slot >= 0 || logoOK || swirlOK {
				t.Fatalf("title pack still active: %s", generated)
			}
			return
		}
		if slot < 0 || catalog.Slots[slot].Winner != winner || !logoOK || !swirlOK || logo != swirl {
			t.Fatalf("title hooks out of sync: %+v\n%s", catalog.Slots, generated)
		}
		got, err := os.ReadFile(filepath.Join(assets, logo))
		if err != nil || !bytes.Equal(got, art) {
			t.Fatalf("unexpected active title artwork: %v", err)
		}
	}
	first, second := authoredTitlePNG(t, color.NRGBA{R: 60, A: 255}), authoredTitlePNG(t, color.NRGBA{R: 200, A: 255})
	initial := install("first-title", first)
	firstID := initial.Packs[0].ID
	assertTitle(initial, firstID, first)
	both := install("second-title", second)
	secondID := both.Packs[1].ID
	if firstID == secondID || !strings.HasPrefix(secondID, "title-") {
		t.Fatalf("expected distinct content IDs, got %q and %q", firstID, secondID)
	}
	assertTitle(both, firstID, first)
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/token/mods/catalog", nil))
	var discovered modCatalog
	if err := json.Unmarshal(response.Body.Bytes(), &discovered); err != nil || response.Code != http.StatusOK ||
		len(discovered.Packs) != 2 || len(discovered.Packs[0].Previews) != 1 {
		t.Fatalf("missing title preview: %d %s", response.Code, response.Body)
	}
	thumbnail := httptest.NewRecorder()
	app.ServeHTTP(thumbnail, httptest.NewRequest(http.MethodGet, "/token/"+discovered.Packs[0].Previews[0], nil))
	if thumbnail.Code != http.StatusOK || thumbnail.Header().Get("Content-Type") != "image/png" {
		t.Fatalf("title preview did not return PNG: %d", thumbnail.Code)
	}
	update := func(action, id string) modCatalog {
		t.Helper()
		catalog, err := updateModCatalog(root, nil, nil, &modChange{Action: action, Pack: id, Slot: titleModTarget})
		if err != nil {
			t.Fatal(err)
		}
		return catalog
	}
	music := testMusicMod(t, "title-music", []modTrack{{Track: "title-theme", File: "audio/title.ogg"}}, "")
	catalog, err := updateModCatalog(root, nil, bytes.NewReader(music), nil)
	if err != nil || len(catalog.Slots) != 2 {
		t.Fatalf("artwork and music should have independent slots: %v %+v", err, catalog.Slots)
	}
	assertTitle(update("select", secondID), secondID, second)
	assertTitle(update("disable", secondID), firstID, first)
	assertTitle(update("disable", firstID), "", nil)
}

func TestTitleModRejectsInvalidPNG(t *testing.T) {
	var output bytes.Buffer
	if err := exportTitleMod(&output, modMetadata{Name: "invalid"}, "broken", strings.NewReader("not a PNG")); err == nil {
		t.Fatal("export accepted an invalid title image")
	}
}

func authoredTitlePNG(t *testing.T, fill color.NRGBA) []byte {
	t.Helper()
	art := image.NewNRGBA(image.Rect(0, 0, titleArtworkWidth, titleArtworkHeight))
	draw.Draw(art, art.Bounds(), image.NewUniform(fill), image.Point{}, draw.Src)
	var pngBytes bytes.Buffer
	if err := png.Encode(&pngBytes, art); err != nil {
		t.Fatal(err)
	}
	return pngBytes.Bytes()
}

func TestTitleAuthoringRejectsWrongCanvasAndEmptyArtwork(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "token")
	for _, test := range []struct {
		art     []byte
		problem string
	}{
		{testTitlePNG(t, 175), "2212 × 760"},
		{authoredTitlePNG(t, color.NRGBA{}), "cover at least 1%"},
	} {
		response := httptest.NewRecorder()
		app.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/token/mods/validate-title", bytes.NewReader(test.art)))
		if response.Code != http.StatusBadRequest || !strings.Contains(response.Body.String(), test.problem) {
			t.Fatalf("expected %q, got %d %s", test.problem, response.Code, response.Body)
		}
	}
}

func TestUninstallLocalModKeepsLauncherArchiveAndRecoversLocalCopy(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	track := []modTrack{{Track: "song-09", File: "audio/replace.ogg"}}
	remote := testMusicMod(t, "from-launcher", track, "")
	remotePath := filepath.Join(external, "external.o2r")
	if err := os.WriteFile(remotePath, remote, 0644); err != nil {
		t.Fatal(err)
	}
	local := testMusicMod(t, "local-pack", track, "")
	catalog, err := updateModCatalog(root, []string{external}, bytes.NewReader(local), nil)
	if err != nil {
		t.Fatal(err)
	}
	index := slices.IndexFunc(catalog.Packs, func(pack installedMod) bool { return pack.ID == "local-pack" })
	if index < 0 {
		t.Fatalf("local pack missing: %+v", catalog.Packs)
	}
	pack := catalog.Packs[index]
	change := modChange{Action: "uninstall", Filename: pack.Filename, Revision: "stale"}
	if _, err := updateModCatalog(root, nil, nil, &change); err == nil {
		t.Fatal("stale uninstall succeeded")
	}
	change.Revision = localModUninstallRevision(&pack)
	if change.Revision == "" {
		t.Fatal("local uninstall has no revision")
	}
	catalog, err = updateModCatalog(root, nil, nil, &change)
	if err != nil || len(catalog.Packs) != 1 || !catalog.Packs[0].External ||
		len(catalog.Slots) != 1 || catalog.Slots[0].Winner != "from-launcher" {
		t.Fatalf("uninstall did not refresh the fallback: %+v %v", catalog, err)
	}
	backups, err := filepath.Glob(filepath.Join(root, "game-assets", "mods", ".uninstalled", "*", pack.Filename))
	if err != nil || len(backups) != 1 {
		t.Fatalf("missing recovery copy: %v %v", backups, err)
	}
	for path, expected := range map[string][]byte{backups[0]: local, remotePath: remote} {
		if saved, err := os.ReadFile(path); err != nil || !bytes.Equal(saved, expected) {
			t.Fatalf("archive changed at %s: %v", path, err)
		}
	}
}

func TestMusicDraftSavesAndInstallsWithoutEditingGameManifest(t *testing.T) {
	root := t.TempDir()
	manifest := []byte("# Existing game manifest\n[custom]\nenabled = 1\n")
	if err := writeAtomicFile(liveAssetManifestPath(root), manifest); err != nil {
		t.Fatal(err)
	}
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	song := testModAudio(t)
	saved := postAssetFormTo(t, app, "/tok/music-draft", map[string]string{
		"split-change-song-09": "1", "split-song-09": "fillmore",
	}, map[string][]byte{"track-song-09": song, "variant-song-09-fillmore": song})
	if saved.Code != http.StatusOK || !strings.Contains(saved.Body.String(), "music-draft-audio/song-09-fillmore") {
		t.Fatalf("draft: %d %s", saved.Code, saved.Body)
	}
	if hasModSources(root, nil) {
		t.Fatal("saving a draft activated a game mod")
	}
	preview := httptest.NewRecorder()
	app.ServeHTTP(preview, httptest.NewRequest(http.MethodGet, "/tok/music-draft-audio/song-09-fillmore", nil))
	if preview.Code != http.StatusOK || !bytes.Equal(preview.Body.Bytes(), song) {
		t.Fatalf("draft preview: %d", preview.Code)
	}

	pack := strings.NewReader(`{"name":"Draft","version":"1.0.0","author":"Fixture","targets":["song-09","song-09-fillmore"]}`)
	installed := httptest.NewRecorder()
	app.ServeHTTP(installed, httptest.NewRequest(http.MethodPost, "/tok/mods/export-draft?install=true", pack))
	if installed.Code != http.StatusOK || !strings.Contains(installed.Body.String(), musicModType) {
		t.Fatalf("install: %d %s", installed.Code, installed.Body)
	}
	if got, err := os.ReadFile(liveAssetManifestPath(root)); err != nil || !bytes.Equal(got, manifest) {
		t.Fatalf("the live manifest changed: %v\n%s", err, got)
	}
	catalog, err := updateModCatalog(root, nil, nil, nil)
	if err != nil || len(catalog.Packs) != 1 || len(catalog.Packs[0].Tracks) != 2 {
		t.Fatalf("installed draft lost assignments: %+v %v", catalog.Packs, err)
	}
}

func TestModPickerConvertsLegacyLanguageAndManualInputs(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	install := func(format string, data []byte) modCatalog {
		t.Helper()
		response := httptest.NewRecorder()
		app.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/tok/mods/install?format="+format, bytes.NewReader(data)))
		var catalog modCatalog
		if err := json.Unmarshal(response.Body.Bytes(), &catalog); response.Code != http.StatusOK || err != nil {
			t.Fatalf("install %s: %d %s", format, response.Code, response.Body)
		}
		return catalog
	}
	translation := install(".arlang", testLanguageRelease(t, "legacy.fr", "Legacy French", ""))
	if translation.InstalledType != languageModType || len(translation.Packs) != 1 ||
		!strings.HasPrefix(translation.Packs[0].ID, "language-content-") {
		t.Fatalf("expected converted language archive: %+v", translation)
	}
	compat := install(".pdf", albumPDF(t, 3))
	if compat.InstalledType != manualModType || compat.LegacyManual || len(compat.Packs) != 2 {
		t.Fatalf("expected converted manual archive: %+v", compat)
	}
	loose := []byte("%PDF-1.4\nA text-only fixture.\n%%EOF\n")
	fallback := install(".pdf", loose)
	if !fallback.LegacyManual || !fallback.InstalledLegacyManual || len(fallback.Packs) != 2 {
		t.Fatalf("expected legacy PDF fallback: %+v", fallback)
	}
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/tok/manual.pdf?legacy=true", nil))
	if response.Code != http.StatusOK || !bytes.Equal(response.Body.Bytes(), loose) {
		t.Fatalf("active manual shadowed the legacy preview: %d", response.Code)
	}
	response = httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodDelete, "/tok/manual", nil))
	matches, err := filepath.Glob(filepath.Join(root, "game-assets", legacyModBackupDir, "manual-*", "manual.pdf"))
	if response.Code != http.StatusOK || err != nil || len(matches) != 1 {
		t.Fatalf("fallback recovery missing: %d %v %v", response.Code, matches, err)
	}
}
