package desktop

import (
	"os"
	"path/filepath"
	"testing"
)

func TestGameImportsTravelWithPackagedGame(t *testing.T) {
	root, resources, relocated := t.TempDir(), t.TempDir(), t.TempDir()
	nativePackageFixture(t, root)
	for _, name := range []string{"ar2-master.armaster", "ar2-world.argame", "future-game/content.dat"} {
		put(t, filepath.Join(root, "game-assets", "imports", name), "synthetic imported content")
	}
	put(t, filepath.Join(root, "ar2.sfc"), "private source ROM")
	put(t, filepath.Join(root, "game-assets", "regions", "native-jp", "media.arregion"), "regional content")
	if err := stageSeed(root, resources); err != nil {
		t.Fatal(err)
	}
	if err := InitializeData(resources, relocated); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"ar2-master.armaster", "ar2-world.argame", "future-game/content.dat"} {
		if read(t, filepath.Join(relocated, "game-assets", "imports", name)) != "synthetic imported content" {
			t.Fatal("imported content lost in packaging")
		}
	}
	if read(t, filepath.Join(relocated, "game-assets", "regions", "native-jp", "media.arregion")) != "regional content" {
		t.Fatal("regional content lost in packaging")
	}
	if _, err := os.Stat(filepath.Join(relocated, "ar2.sfc")); !os.IsNotExist(err) {
		t.Fatal("copied donor ROM into game data")
	}
}
