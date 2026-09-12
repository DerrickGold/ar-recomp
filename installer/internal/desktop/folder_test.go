package desktop

import (
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestGameFolderPublishesOnlyRuntimeAndPreservesSettings(t *testing.T) {
	source, output := t.TempDir(), t.TempDir()
	name, helper, library := Name, "actraiser-builder", "libSDL3.so.0"
	if runtime.GOOS == "windows" {
		name += ".exe"
		helper += ".exe"
		library = "SDL3.dll"
	}
	if runtime.GOOS == "darwin" {
		library = "libSDL3.dylib"
	}
	for path, data := range map[string]string{name: "game", helper: "runtime helper", library: "SDL", "input.sfc": "ROM", "main.c": "not an output", "game.o": "not an output"} {
		put(t, filepath.Join(source, path), data)
	}
	put(t, filepath.Join(output, "settings.ini"), "user settings")
	put(t, filepath.Join(output, "saves", "save.srm"), "save")
	installed, err := InstallGameFolder(filepath.Join(source, name), filepath.Join(source, helper), filepath.Join(source, "input.sfc"), output)
	if err != nil || installed != filepath.Join(output, name) {
		t.Fatal(installed, err)
	}
	for path, want := range map[string]string{name: "game", library: "SDL", "user-rom.sfc": "ROM", filepath.Join("tools", helper): "runtime helper", "settings.ini": "user settings", "saves/save.srm": "save"} {
		if got := read(t, filepath.Join(output, filepath.FromSlash(path))); got != want {
			t.Fatalf("%s: %q", path, got)
		}
	}
	for _, path := range []string{"main.c", "game.o", "run-game.bat", "run-game.sh", "run-game.command", "utils"} {
		if _, err := os.Stat(filepath.Join(output, path)); !os.IsNotExist(err) {
			t.Fatalf("unnecessary output %s: %v", path, err)
		}
	}
}

func TestGameFolderStagesBeforeReplacingAnyFile(t *testing.T) {
	source, output := t.TempDir(), t.TempDir()
	name := Name
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	put(t, filepath.Join(source, name), "new game")
	put(t, filepath.Join(output, name), "old game")
	put(t, filepath.Join(output, "user-rom.sfc"), "old ROM")
	if _, err := InstallGameFolder(filepath.Join(source, name), filepath.Join(source, "missing-helper"), filepath.Join(source, "missing-rom"), output); err == nil {
		t.Fatal("accepted incomplete staging")
	}
	if read(t, filepath.Join(output, name)) != "old game" || read(t, filepath.Join(output, "user-rom.sfc")) != "old ROM" {
		t.Fatal("failed staging changed installation")
	}
}

func TestGameFolderRollsBackPartialPublication(t *testing.T) {
	stage, output := t.TempDir(), t.TempDir()
	order := []string{"SDL3.dll", "new-helper.dll", "ActRaiserRecomp.exe"}
	for _, leaf := range order {
		put(t, filepath.Join(stage, "new", leaf), "new "+leaf)
	}
	put(t, filepath.Join(output, "SDL3.dll"), "old SDL")
	put(t, filepath.Join(output, "ActRaiserRecomp.exe"), "old game")
	put(t, filepath.Join(output, "saves", "save.srm"), "save")
	keep, err := publishGameFiles(stage, output, order, func(from, to string) error {
		if filepath.Base(to) == "ActRaiserRecomp.exe" {
			return errors.New("simulated executable in use")
		}
		return os.Rename(from, to)
	})
	if err == nil || keep {
		t.Fatal(keep, err)
	}
	if read(t, filepath.Join(output, "SDL3.dll")) != "old SDL" || read(t, filepath.Join(output, "ActRaiserRecomp.exe")) != "old game" || read(t, filepath.Join(output, "saves", "save.srm")) != "save" {
		t.Fatal("rollback failed")
	}
	if _, err := os.Stat(filepath.Join(output, "new-helper.dll")); !os.IsNotExist(err) {
		t.Fatal("new dependency left after rollback")
	}
}

func TestGameFolderRefusesDestinationSymlink(t *testing.T) {
	output := t.TempDir()
	if err := os.Symlink(t.TempDir(), filepath.Join(output, Name)); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	if _, err := InstallGameFolder(filepath.Join(t.TempDir(), Name), "missing", "missing", output); err == nil {
		t.Fatal("accepted redirect")
	}
}
