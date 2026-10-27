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

func TestWorkshopDoesNotPromoteUninstalledPreviews(t *testing.T) {
	if items := WorkshopList(t.TempDir()); len(items) != 0 {
		t.Fatalf("fresh installation promotes previews: %+v", items)
	}
	previous := importers
	t.Cleanup(func() { importers = previous })
	importers = []importer{
		{item: Item{ID: "released"}, status: func(string) string { return "missing" }},
		{item: Item{ID: "preview"}, preview: true, status: func(string) string { return "missing" }},
		{item: Item{ID: "installed-preview"}, preview: true, status: func(string) string { return "installed" }},
		{item: Item{ID: "damaged-preview"}, preview: true, status: func(string) string { return "invalid" }},
	}
	items := WorkshopList(t.TempDir())
	if len(items) != 3 || items[0].ID != "released" || items[1].ID != "installed-preview" || items[2].ID != "damaged-preview" {
		t.Fatalf("released content or existing preview became inaccessible: %+v", items)
	}
	if len(List(t.TempDir())) != 4 {
		t.Fatal("developer catalog lost explicit preview imports")
	}
}
