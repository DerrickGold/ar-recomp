package desktop

import (
	"os"
	"path/filepath"
	"testing"
)

func TestStandaloneDataSurvivesWorkspaceRemovalAndPreservesEdits(t *testing.T) {
	source, parent := t.TempDir(), t.TempDir()
	output := filepath.Join(parent, Name)
	nativePackageFixture(t, source)
	put(t, filepath.Join(source, "config.ini"), "old settings")
	put(t, filepath.Join(source, "LICENSE"), "notice")
	if err := PreparePortableData(source, output); err != nil {
		t.Fatal(err)
	}
	put(t, filepath.Join(output, "game-assets", "manifest.ini"), "player edit")
	put(t, filepath.Join(output, "saves", "save.srm"), "new save")
	put(t, filepath.Join(output, "config.ini"), "new settings")
	if err := PreparePortableData(source, output); err != nil {
		t.Fatal(err)
	}
	if read(t, filepath.Join(output, "game-assets", "manifest.ini")) != "player edit" || read(t, filepath.Join(output, "config.ini")) != "new settings" || read(t, filepath.Join(output, "saves", "save.srm")) != "new save" {
		t.Fatal("runtime edit overwritten")
	}
	if err := os.Remove(filepath.Join(output, "saves", "save.srm")); err != nil {
		t.Fatal(err)
	}
	if err := PreparePortableData(source, output); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(output, "saves", "save.srm")); !os.IsNotExist(err) {
		t.Fatal("deleted save imported again")
	}
	artifact := filepath.Join(output, Name+".app")
	if err := WritePortableMarker(artifact, output); err != nil {
		t.Fatal(err)
	}
	if read(t, artifact+".portable") != ".\n" {
		t.Fatal("sidecar points outside output")
	}
	moved := filepath.Join(t.TempDir(), "Moved Game")
	if err := os.Rename(output, moved); err != nil {
		t.Fatal(err)
	}
	if err := os.RemoveAll(source); err != nil {
		t.Fatal(err)
	}
	data, err := ResolveDataDirectory(Layout{Artifact: filepath.Join(moved, Name+".app")}, StorageOptions{}, t.TempDir(), "darwin", func(string) string { return "" })
	if err != nil || data != moved {
		t.Fatalf("relocated data: %s %v", data, err)
	}
	if read(t, filepath.Join(moved, "notices", "LICENSE")) != "notice" {
		t.Fatal("notice missing")
	}
	if read(t, filepath.Join(moved, "game-assets", "manual.pdf")) != "synthetic resource" {
		t.Fatal("runtime data depends on old workspace")
	}
}

func TestPortableOutputRefusesSourceOverlapAndSymlinks(t *testing.T) {
	source := t.TempDir()
	for _, output := range []string{source, filepath.Join(source, "game-assets", "hd", "output")} {
		if err := PreparePortableData(source, output); err == nil {
			t.Fatal("accepted output inside build inputs")
		}
	}
	source = t.TempDir()
	nativePackageFixture(t, source)
	output, elsewhere := t.TempDir(), t.TempDir()
	if err := os.Symlink(elsewhere, filepath.Join(output, "game-assets")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	if err := PreparePortableData(source, output); err == nil {
		t.Fatal("seed followed symlink")
	}
	entries, err := os.ReadDir(elsewhere)
	if err != nil || len(entries) != 0 {
		t.Fatalf("external data modified: %v %v", entries, err)
	}
}

func TestLegacyGlobalDataImportIsNonDestructiveAndGameOnly(t *testing.T) {
	legacy := t.TempDir()
	game := filepath.Join(legacy, "game")
	put(t, filepath.Join(legacy, seedStateName), "{}")
	put(t, filepath.Join(legacy, "saves", "save.srm"), "legacy save")
	put(t, filepath.Join(legacy, "settings.ini"), "old settings")
	put(t, filepath.Join(legacy, "installer", "private"), "tools")
	put(t, filepath.Join(game, "settings.ini"), "new settings")
	if err := importLegacyGlobalData(game); err != nil {
		t.Fatal(err)
	}
	if read(t, filepath.Join(game, "saves", "save.srm")) != "legacy save" || read(t, filepath.Join(legacy, "saves", "save.srm")) != "legacy save" {
		t.Fatal("save missing or original removed")
	}
	if read(t, filepath.Join(game, "settings.ini")) != "new settings" {
		t.Fatal("existing new data overwritten")
	}
	if _, err := os.Stat(filepath.Join(game, "installer")); !os.IsNotExist(err) {
		t.Fatal("imported Builder data")
	}
	if err := os.Remove(filepath.Join(game, "saves", "save.srm")); err != nil {
		t.Fatal(err)
	}
	if err := importLegacyGlobalData(game); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(game, "saves", "save.srm")); !os.IsNotExist(err) {
		t.Fatal("reimported deleted save")
	}
}
