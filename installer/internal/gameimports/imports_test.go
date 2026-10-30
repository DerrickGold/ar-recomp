package gameimports

import (
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/interfacecatalog"
)

func TestCatalogAndUnknownContent(t *testing.T) {
	root := t.TempDir()
	items := List(root)
	if len(items) == 0 {
		t.Fatal("missing import catalog")
	}
	entries, err := interfacecatalog.Entries()
	if err != nil {
		t.Fatal(err)
	}
	keys := map[string]bool{}
	for _, entry := range entries {
		keys[entry.Key] = true
	}
	for _, item := range items {
		if item.ID == "" || !keys[item.LabelKey] || !keys[item.HelpKey] || !keys[item.NextKey] || item.Status != "missing" {
			t.Fatalf("invalid catalog item: %+v", item)
		}
	}
	if _, err := Import(root, "../../other", []byte("ROM")); err == nil {
		t.Fatal("unknown importer accepted")
	}
}

func TestAdditionalContentUsesItsOwnImporter(t *testing.T) {
	previous := importers
	t.Cleanup(func() { importers = previous })
	installed := false
	importers = append(append([]importer{}, previous...), importer{
		item: Item{ID: "another-game-content"}, maximumROMBytes: 4,
		extract: func(data []byte) ([]byte, error) { return append([]byte("extracted:"), data...), nil },
		install: func(root string, data []byte) (bool, error) {
			if root != "selected-root" || string(data) != "extracted:ROM" {
				t.Fatal("wrong content dispatched")
			}
			installed = true
			return true, nil
		},
		status: func(string) string { return "missing" },
	})
	if changed, err := Import("selected-root", "another-game-content", []byte("ROM")); err != nil || !changed || !installed {
		t.Fatal(changed, installed, err)
	}
	installed = false
	if _, err := Import("selected-root", "another-game-content", []byte("oversized")); err == nil || installed {
		t.Fatal("ignored per-import size limit")
	}
}

func TestWorkshopOffersMasterAndHidesDeveloperImports(t *testing.T) {
	if items := WorkshopList(t.TempDir()); len(items) != 1 || items[0].ID != "ar2-master" || items[0].Status != "missing" {
		t.Fatalf("fresh installation must offer the character import: %+v", items)
	}
	previous := importers
	t.Cleanup(func() { importers = previous })
	importers = []importer{
		{item: Item{ID: "released"}, status: func(string) string { return "missing" }},
		{item: Item{ID: "preview"}, developerOnly: true, status: func(string) string { return "missing" }},
		{item: Item{ID: "installed-preview"}, developerOnly: true, status: func(string) string { return "installed" }},
		{item: Item{ID: "damaged-preview"}, developerOnly: true, status: func(string) string { return "invalid" }},
	}
	items := WorkshopList(t.TempDir())
	if len(items) != 1 || items[0].ID != "released" {
		t.Fatalf("developer imports must stay out of the public catalog: %+v", items)
	}
	if len(List(t.TempDir())) != 4 {
		t.Fatal("developer catalog lost explicit preview imports")
	}
}
