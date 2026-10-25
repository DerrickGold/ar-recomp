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
