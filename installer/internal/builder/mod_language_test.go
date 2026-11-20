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
	"strings"
	"testing"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func testLanguageRelease(t *testing.T, id, name, version string) []byte {
	t.Helper()
	project, err := lk.OpenAuthorProjectDirectory("../../../examples/language-pack")
	if err != nil {
		t.Fatal(err)
	}
	metadata := project.Pack().Manifest().Metadata()
	metadata.ID, metadata.Name = id, name
	project, err = project.WithMetadata(metadata)
	if err != nil {
		t.Fatal(err)
	}
	project, err = project.ReviewedAll()
	if err != nil {
		t.Fatal(err)
	}
	project, _, err = project.Publication(lk.PublicationOptions{ConfirmRights: true})
	if err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	if version == "" {
		err = project.WriteArchive(&output, "publication")
	} else {
		err = project.WriteLanguageMod(&output, version)
	}
	if err != nil {
		t.Fatal(err)
	}
	return output.Bytes()
}

func runtimeLanguageIndex(t *testing.T, root string) string {
	t.Helper()
	if err := PrepareRuntimeLanguages(context.Background(), root, "", io.Discard); err != nil {
		t.Fatal(err)
	}
	index, err := os.ReadFile(filepath.Join(languagePacksRoot(filepath.Join(root, "game-assets")), ".arlang-cache", "catalog-v1.tsv"))
	if err != nil {
		t.Fatal(err)
	}
	return string(index)
}

func TestPlayingWithPreparedModsKeepsThePreparedLanguageCatalog(t *testing.T) {
	root := t.TempDir()
	const id = "cached.fr-ca"
	catalog, err := updateModCatalog(root, nil, bytes.NewReader(testLanguageRelease(t, id, "Cached translation", "1.0.0")), nil)
	if err != nil || len(catalog.Packs) != 1 {
		t.Fatalf("install test translation: %+v %v", catalog, err)
	}
	before := runtimeLanguageIndex(t, root)
	if !strings.Contains(before, "replace\t"+id+"\t") {
		t.Fatalf("missing prepared translation: %s", before)
	}
	// The source archive has disappeared but the last installed cache remains.
	if err := os.Remove(filepath.Join(root, "game-assets", "mods", catalog.Packs[0].Filename)); err != nil {
		t.Fatal(err)
	}
	t.Setenv(ModRecoveryEnv, "prepared")
	if after := runtimeLanguageIndex(t, root); after != before {
		t.Fatalf("launch replaced the prepared catalog: %s", after)
	}
	t.Setenv(ModRecoveryEnv, "legacy")
	if after := runtimeLanguageIndex(t, root); after != lk.ArchiveCatalogHeader {
		t.Fatalf("legacy recovery reused prepared mods: %s", after)
	}
}

func TestLanguageModReleaseLifecycle(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	const id = "Example.fr-CA"
	modID := lk.LanguageModID(id)
	update := func(data []byte, change *modChange) modCatalog {
		t.Helper()
		var source io.Reader
		if data != nil {
			source = bytes.NewReader(data)
		}
		catalog, err := updateModCatalog(root, []string{external}, source, change)
		if err != nil {
			t.Fatal(err)
		}
		return catalog
	}
	first := testLanguageRelease(t, id, "Original", "1.9.0")
	initial := update(first, nil)
	if len(initial.Packs) != 1 || !strings.HasPrefix(initial.Packs[0].ID, "language-content-") || !initial.Packs[0].Enabled {
		t.Fatalf("unexpected initial install: %+v", initial.Packs)
	}
	update(nil, &modChange{Action: "disable", Pack: initial.Packs[0].ID})
	app := newApplication(context.Background(), Options{ProjectRoot: root, ModDirectories: []string{external}}, "tok")
	checkPreview := func(release, name, version string) {
		t.Helper()
		preview := httptest.NewRecorder()
		app.ServeHTTP(preview, httptest.NewRequest(http.MethodGet, "/tok/mods/language-preview?mod="+modID+"&release="+release, nil))
		var sample struct {
			Metadata lk.PackMetadata
			Version  string
			Coverage lk.RuntimeTextCoverage
			Samples  []languagePreviewSample
		}
		if err := json.Unmarshal(preview.Body.Bytes(), &sample); err != nil || preview.Code != http.StatusOK ||
			sample.Metadata.ID != id || sample.Metadata.Name != name || sample.Metadata.Author == "" ||
			sample.Version != version || sample.Coverage.Total <= sample.Coverage.Provided ||
			sample.Coverage.Provided != 2 || len(sample.Samples) == 0 {
			t.Fatalf("language preview = %d %s, %v", preview.Code, preview.Body, err)
		}
	}
	catalog := update(nil, nil)
	checkPreview(catalog.languageGroups[0].Active, "Original", "1.9.0")
	second := testLanguageRelease(t, id, "Updated", "1.10.0")
	updated := update(second, nil)
	if len(updated.Packs) != 1 || updated.Packs[0].Version != "1.10.0" || updated.Packs[0].Enabled {
		t.Fatalf("update changed identity or availability: %+v", updated.Packs)
	}
	managed := filepath.Join(root, "game-assets", "mods", modID+".o2r")
	if got, err := os.ReadFile(managed); err != nil || !bytes.Equal(got, second) {
		t.Fatal("managed release was not replaced", err)
	}
	update(nil, &modChange{Action: "enable", Pack: modID})
	update(testLanguageRelease(t, "other.fr-ca", "Alternate", "1.0.0"), nil)
	remote := filepath.Join(external, "remote.o2r")
	newer := testLanguageRelease(t, id, "Launcher release", "2.0.0")
	if err := os.WriteFile(remote, newer, 0644); err != nil {
		t.Fatal(err)
	}
	scan := update(nil, nil)
	if len(scan.Packs) != 2 || scan.Packs[0].Name != "Launcher release" || !scan.Packs[0].External {
		t.Fatalf("new external release was not selected: %+v", scan.Packs)
	}
	shadows := scan.languageGroups[0].LocalAlternates
	if len(shadows) != 1 || shadows[0].ArchiveURL == "" {
		t.Fatalf("missing local copy of the older release: %+v", shadows)
	}
	// Force the preview to use fresh discovery of the older installed copy.
	if err := os.Remove(filepath.Join(root, "game-assets", "mods-cache", modIndexName)); err != nil {
		t.Fatal(err)
	}
	checkPreview(shadows[0].Key, "Updated", "1.10.0")
	index := runtimeLanguageIndex(t, root)
	if !strings.Contains(index, "replace\t"+id+"\t") || !strings.Contains(index, "replace\tother.fr-ca\t") {
		t.Fatal("runtime lost case-sensitive identity or same-locale alternative", index)
	}
	if got, err := os.ReadFile(remote); err != nil || !bytes.Equal(got, newer) {
		t.Fatal("mod resolution modified the external archive", err)
	}
	if err := os.Remove(remote); err != nil {
		t.Fatal(err)
	}
	if scan := update(nil, nil); scan.Packs[0].Version != "1.10.0" || scan.Packs[0].External {
		t.Fatalf("external removal did not restore the installed release: %+v", scan.Packs)
	}
}

func TestLanguageLibraryResolvesLegacyAndEqualVersionChoices(t *testing.T) {
	root := t.TempDir()
	const id = "sample.fr-ca"
	packs := languagePacksRoot(filepath.Join(root, "game-assets"))
	if err := os.MkdirAll(packs, 0755); err != nil {
		t.Fatal(err)
	}
	legacy := testLanguageRelease(t, id, "Legacy", "")
	path := filepath.Join(packs, "old.arlang")
	if err := os.WriteFile(path, legacy, 0644); err != nil {
		t.Fatal(err)
	}
	var directories []string
	for _, name := range []string{"first", "second"} {
		dir := t.TempDir()
		directories = append(directories, dir)
		data := testLanguageRelease(t, id, name, "1.0.0")
		if err := os.WriteFile(filepath.Join(dir, "translation.o2r"), data, 0644); err != nil {
			t.Fatal(err)
		}
	}
	app := newApplication(context.Background(), Options{ProjectRoot: root, ModDirectories: directories}, "secret")
	rows := func() []localizationCatalogEntry {
		t.Helper()
		rec := httptest.NewRecorder()
		app.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/secret/mods/catalog", nil))
		var result modCatalogView
		if err := json.Unmarshal(rec.Body.Bytes(), &result); err != nil || rec.Code != http.StatusOK {
			t.Fatalf("mod catalogue: %d %s (%v)", rec.Code, rec.Body, err)
		}
		return result.Languages
	}
	list := rows()
	if len(list) != 1 || len(list[0].Choices) != 3 || list[0].Release != "" {
		t.Fatalf("ambiguous copies did not reach the language library: %+v", list)
	}
	for i, dir := range directories {
		if choice := list[0].Choices[i]; choice.SourcePath != filepath.Join(dir, "translation.o2r") {
			t.Fatalf("ambiguous external release missing its source: %+v", choice)
		}
	}
	if index := runtimeLanguageIndex(t, root); !strings.Contains(index, "conflict\t"+id+"\n") {
		t.Fatal("ambiguous pack was exposed to game", index)
	}
	choose := func(release string) {
		locJSON(t, app, "select-language-release", localizationInstallRequest{
			ID: id, ModID: lk.LanguageModID(id), Release: release,
		}, http.StatusOK)
	}
	choose("legacy")
	if index := runtimeLanguageIndex(t, root); !strings.Contains(index, "pack\t"+id+"\t") {
		t.Fatal("legacy choice missing from game", index)
	}
	action, err := json.Marshal(modChange{Action: "select", Pack: lk.LanguageModID(id), Release: list[0].Choices[1].Key})
	if err != nil {
		t.Fatal(err)
	}
	changed := httptest.NewRecorder()
	app.ServeHTTP(changed, httptest.NewRequest(http.MethodPost, "/secret/mods/activation", bytes.NewReader(action)))
	var activated modCatalogView
	if err := json.Unmarshal(changed.Body.Bytes(), &activated); err != nil || changed.Code != http.StatusOK ||
		len(activated.Languages) != 1 || activated.Languages[0].InstalledName != "second" || !activated.Languages[0].External {
		t.Fatalf("selected release missing from activation response: %d %s (%v)", changed.Code, changed.Body, err)
	}
	if index := runtimeLanguageIndex(t, root); !strings.Contains(index, "replace\t"+id+"\t") {
		t.Fatal("selected O2R release missing from game", index)
	}
	if got, err := os.ReadFile(path); err != nil || !bytes.Equal(got, legacy) {
		t.Fatal("migration changed the legacy archive", err)
	}

	rec := httptest.NewRecorder()
	file := testLanguageRelease(t, "new.fr-ca", "New", "1.0.0")
	app.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/secret/mods/install?type=actraiser-language", bytes.NewReader(file)))
	var installed modCatalogView
	if err := json.Unmarshal(rec.Body.Bytes(), &installed); err != nil || rec.Code != http.StatusOK ||
		installed.InstalledType != languageModType || len(installed.Languages) != 2 {
		t.Fatalf("language picker failed to install and refresh: %d %s", rec.Code, rec.Body)
	}
}

func TestReviewedLegacyLanguageImportInstallsModAndFollowsSaves(t *testing.T) {
	app := editableWorkflowFixture(t)
	root := app.options.ProjectRoot
	const id = "converted.fr-ca"
	data := testLanguageRelease(t, id, "Legacy translation", "")
	preview := locUpload(t, app, "import", data, map[string]string{"intent": "preview"}, http.StatusOK)
	var imported struct{ Token string }
	if err := json.Unmarshal(preview.Body.Bytes(), &imported); err != nil || imported.Token == "" {
		t.Fatalf("import preview: %s %v", preview.Body, err)
	}
	locJSON(t, app, "accept-import", map[string]string{"importToken": imported.Token}, http.StatusOK)
	q := locIdentity(app)
	response := locJSON(t, app, "install", localizationInstallRequest{ProjectID: q.ProjectID, Revision: q.Revision, AsMod: true}, http.StatusOK)
	var installed struct {
		InstalledType string         `json:"installedType"`
		Catalog       modCatalogView `json:"catalog"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &installed); err != nil ||
		installed.InstalledType != languageModType || len(installed.Catalog.Languages) != 1 ||
		installed.Catalog.Languages[0].ID != id {
		t.Fatal(response.Body.String())
	}
	catalog, err := updateModCatalog(root, nil, nil, &modChange{Action: "disable", Pack: lk.LanguageModID(id)})
	if err != nil || len(catalog.Packs) != 1 || catalog.Packs[0].Enabled {
		t.Fatalf("disable converted pack: %+v %v", catalog.Packs, err)
	}
	original := catalog.Packs[0].ID
	if err := app.testLocalizationWork(func(work *localizationWork) error {
		p := work.current
		metadata := p.Pack().Manifest().Metadata()
		metadata.Name = "Edited after conversion"
		edited, err := p.WithMetadata(metadata)
		if err != nil {
			return err
		}
		update := work.refreshSavedLocalization(context.Background(), edited)
		if !update.Updated || !update.Installed || update.Enabled || update.Error != "" {
			t.Fatalf("saved translation: %+v", update)
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	catalog, err = updateModCatalog(root, nil, nil, nil)
	if err != nil || len(catalog.Packs) != 1 || catalog.Packs[0].ID == original ||
		catalog.Packs[0].Enabled || catalog.Packs[0].Name != "Edited after conversion" {
		t.Fatalf("updated mod: %+v %v", catalog.Packs, err)
	}
}
