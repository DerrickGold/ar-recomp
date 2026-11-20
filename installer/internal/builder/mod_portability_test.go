package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
	"github.com/DerrickGold/ar-recomp/installer/internal/modconfig"
)

func TestPublishedPacksTravelWithPortableGame(t *testing.T) {
	source, external := t.TempDir(), t.TempDir()
	target := "song-09-fillmore"
	var manifest strings.Builder
	gain := 75
	writeModMusicSection(&manifest, target, musicModTargets[target], modTrack{Gain: &gain}, "audio/fixture.ogg")
	writeAssetTestFile(t, liveAssetManifestPath(source), []byte(manifest.String()))
	writeAssetTestFile(t, filepath.Join(source, "game-assets", "audio", "fixture.ogg"), testModAudio(t))
	writeAssetTestFile(t, filepath.Join(source, "defaults", "config.ini"), []byte("# fixture\n"))
	meta := modMetadata{Name: "Fixture pack", Version: "1.0.0", Author: "Fixture"}
	var music, title, manual bytes.Buffer
	if err := exportMusicMod(source, &music, modExportRequest{modMetadata: meta, ID: "test-track", Targets: []string{target}}); err != nil {
		t.Fatal(err)
	}
	if err := exportTitleMod(&title, meta, "test-title", bytes.NewReader(testTitlePNG(t, 44))); err != nil {
		t.Fatal(err)
	}
	book := albumPDF(t, 4)
	if err := exportManualMod(&manual, meta, "test-book", bytes.NewReader(book)); err != nil {
		t.Fatal(err)
	}
	language := testLanguageRelease(t, "portable.fr-ca", "Portable translation", "1.0.0")
	for name, content := range map[string][]byte{
		"music": music.Bytes(), "title": title.Bytes(), "manual": manual.Bytes(), "language": language,
	} {
		file := filepath.Join(t.TempDir(), name+".o2r")
		writeAssetTestFile(t, file, content)
		var stdout, stderr bytes.Buffer
		if code := RunMods([]string{"inspect", "--json", file}, &stdout, &stderr); code != 0 {
			t.Fatalf("published %s cannot be inspected: %d %s %s", name, code, &stdout, &stderr)
		}
		if code := RunMods([]string{"install", "--output-dir", source, "--mods-dir", external, "--json", file}, &stdout, &stderr); code != 0 {
			t.Fatalf("published %s cannot be installed: %d %s %s", name, code, &stdout, &stderr)
		}
	}
	if _, err := updateModCatalog(source, nil, nil, &modChange{Action: "disable", Pack: "test-track"}); err != nil {
		t.Fatal(err)
	}
	output := filepath.Join(t.TempDir(), "portable")
	if err := desktop.PreparePortableData(source, output, RuntimeSeedAssets()); err != nil {
		t.Fatal(err)
	}
	outputAssets := filepath.Join(output, "game-assets")
	settings, err := modconfig.Read(filepath.Join(outputAssets, modOverlayName))
	if err != nil || !settings.Disabled["test-track"] || len(settings.ResolveDirectories(output)) != 0 {
		t.Fatalf("portable choices = %+v (%v)", settings, err)
	}
	if _, err := os.Stat(filepath.Join(outputAssets, "mods-cache")); !os.IsNotExist(err) {
		t.Fatalf("source mod cache was shipped with the portable game: %v", err)
	}
	if err := PrepareRuntimeLanguages(context.Background(), output, "", io.Discard); err != nil {
		t.Fatal(err)
	}
	if _, _, album := activeManualPack(output); album.Pages != 4 {
		t.Fatalf("the game's manual did not survive portable seeding: %+v", album)
	}
	archiveCatalog := runtimeLanguageIndex(t, output)
	if !strings.Contains(archiveCatalog, "replace\tportable.fr-ca\t") {
		t.Fatalf("portable language missing from runtime catalog: %s", archiveCatalog)
	}
	catalog, err := updateModCatalog(output, nil, nil, &modChange{Action: "enable", Pack: "test-track"})
	if err != nil || len(catalog.Packs) != 4 || !slices.ContainsFunc(catalog.Slots, func(slot modSlot) bool { return slot.Winner == "test-title" }) {
		t.Fatalf("portable pack catalog = %+v (%v)", catalog, err)
	}
	overlay, err := os.ReadFile(filepath.Join(outputAssets, modOverlayName))
	if err != nil || !bytes.Contains(overlay, []byte("[music:"+target+"]")) || !bytes.Contains(overlay, []byte("gain = 75")) {
		t.Fatalf("portable overlay lost the split's playback settings: %v\n%s", err, overlay)
	}
}

func TestLauncherModDirectoryFollowsMovedGame(t *testing.T) {
	parent := t.TempDir()
	oldGame := filepath.Join(parent, "old", "ActRaiserRecomp")
	newGame := filepath.Join(parent, "new", "ActRaiserRecomp")
	oldMods := filepath.Join(filepath.Dir(oldGame), "mods")
	newMods := filepath.Join(filepath.Dir(newGame), "mods")
	track := []modTrack{{Track: "song-00", File: "audio/track.ogg"}}
	initial := testMusicMod(t, "move-test", track, "")
	writeAssetTestFile(t, filepath.Join(oldMods, "v1.o2r"), initial)
	writeAssetTestFile(t, liveAssetManifestPath(oldGame), []byte("# fixture\n"))
	if _, err := updateModCatalog(oldGame, []string{oldMods}, nil, &modChange{Action: "disable", Pack: "move-test"}); err != nil {
		t.Fatal(err)
	}
	// Leave a different archive with the same ID under the former launcher
	// directory. It should no longer participate in discovery after the move.
	var replacement bytes.Buffer
	writeAssetTestFile(t, filepath.Join(oldGame, "game-assets", "audio", "fixture.ogg"), testModAudio(t))
	writeAssetTestFile(t, liveAssetManifestPath(oldGame), []byte("[music:song-00]\nsrc = "+musicModTargets["song-00"].Song.Src+"\nfile = audio/fixture.ogg\n"))
	request := modExportRequest{modMetadata: modMetadata{Name: "Updated", Version: "2.0.0"}, ID: "move-test", Targets: []string{"song-00"}}
	if err := exportMusicMod(oldGame, &replacement, request); err != nil {
		t.Fatal(err)
	}
	writeAssetTestFile(t, filepath.Join(newMods, "v2.o2r"), replacement.Bytes())
	if err := os.Rename(oldGame, newGame); err != nil {
		t.Fatal(err)
	}
	catalog, err := updateModCatalog(newGame, nil, nil, nil)
	if err != nil || len(catalog.Packs) != 1 || catalog.Packs[0].Filename != "v2.o2r" ||
		catalog.Packs[0].Error != "" || catalog.Packs[0].Enabled || !catalog.Packs[0].External {
		t.Fatalf("portable launcher discovery = %+v (%v)", catalog, err)
	}
	if got, err := os.ReadFile(filepath.Join(oldMods, "v1.o2r")); err != nil || !bytes.Equal(got, initial) {
		t.Fatal("launcher directory was modified", err)
	}
}

func TestPreviousInstallationImportAfterOpeningManageMods(t *testing.T) {
	app, source := importTestApp(t)
	art, _ := bundledTitleArtwork("ja")
	relative := bundledTitleRelativePath(art)
	var legacy strings.Builder
	writeModTitleSections(&legacy, relative)
	writeAssetTestFile(t, liveAssetManifestPath(source), []byte(legacy.String()))
	writeAssetTestFile(t, filepath.Join(source, "game-assets", filepath.FromSlash(relative)), art)

	file := testMusicMod(t, "import-track", []modTrack{{Track: "song-00", File: "audio/track.ogg"}}, "")
	if _, err := updateModCatalog(source, nil, bytes.NewReader(file), nil); err != nil {
		t.Fatal(err)
	}
	if _, err := updateModCatalog(source, nil, nil, &modChange{Action: "disable", Pack: "import-track"}); err != nil {
		t.Fatal(err)
	}
	// Simulate the launcher registering its empty sibling mod directory.
	app.options.ModDirectories = []string{filepath.Join(filepath.Dir(app.options.ProjectRoot), "mods")}
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/tok/mods/catalog", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("open Manage Mods: %s", response.Body)
	}
	body, _ := json.Marshal(map[string]string{"directory": source})
	response = interfacePOST(app, "/tok/installation-import/preview", string(body))
	var plan desktop.ImportPlan
	if err := json.Unmarshal(response.Body.Bytes(), &plan); err != nil || response.Code != http.StatusOK {
		t.Fatalf("import preview: %s (%v)", response.Body, err)
	}
	if plan.Conflicts != 0 || slices.ContainsFunc(plan.Files, func(file desktop.ImportFile) bool { return strings.Contains(file.Path, "/mods-cache/") }) {
		t.Fatalf("unexpected import plan: %+v", plan)
	}
	body, _ = json.Marshal(map[string]any{"revision": plan.Revision, "confirm": true})
	response = interfacePOST(app, "/tok/installation-import/apply", string(body))
	if response.Code != http.StatusOK {
		t.Fatalf("apply previous installation: %s", response.Body)
	}
	catalog, err := updateModCatalog(app.options.ProjectRoot, app.options.ModDirectories, nil, nil)
	if err != nil || !slices.ContainsFunc(catalog.Packs, func(pack installedMod) bool {
		return pack.ID == "import-track" && pack.Error == "" && !pack.Enabled
	}) {
		t.Fatalf("import lost the mod's disabled choice: %+v (%v)", catalog, err)
	}
	if len(catalog.Slots) != 1 || catalog.Slots[0].Winner != bundledTitles[1].id {
		t.Fatalf("import did not restore the Japanese title as an included mod: %+v", catalog.Slots)
	}
	if manifest, err := os.ReadFile(liveAssetManifestPath(source)); err != nil || string(manifest) != legacy.String() {
		t.Fatalf("import modified the source manifest: %v", err)
	}
}
