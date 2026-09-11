package desktop

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
)

// Optional end-to-end check with a freshly built real launcher and a tiny
// ROM-free C executable. This exercises codesign, relocation and actual startup
// storage selection without waiting for a complete game recompilation.
func TestStandaloneMacAppLaunchStorage(t *testing.T) {
	helper := os.Getenv("AR_TEST_DESKTOP_BUILDER")
	if runtime.GOOS != "darwin" || helper == "" {
		t.Skip("requires macOS and AR_TEST_DESKTOP_BUILDER")
	}
	source, bins, home := t.TempDir(), t.TempDir(), t.TempDir()
	output := filepath.Join(t.TempDir(), "ActRaiserRecomp")
	rom := nativePackageFixture(t, source)
	if err := PreparePortableData(source, output); err != nil {
		t.Fatal(err)
	}
	put(t, filepath.Join(bins, "probe.c"), "#include <stdio.h>\nint main(void) { FILE *f=fopen(\"storage-probe.txt\",\"w\"); if(!f) return 1; fputs(\"ran\",f); return fclose(f); }\n")
	binary := filepath.Join(bins, Name)
	if data, err := exec.Command("cc", filepath.Join(bins, "probe.c"), "-o", binary).CombinedOutput(); err != nil {
		t.Fatalf("compile: %v: %s", err, data)
	}
	artifact, err := Package(context.Background(), PackageOptions{Binary: binary, Builder: helper, ROM: rom, Root: source, DataRoot: output, Destination: output, Format: "app"})
	if err != nil {
		t.Fatal(err)
	}
	if err := WritePortableMarker(artifact.Path, output); err != nil {
		t.Fatal(err)
	}
	moved := filepath.Join(t.TempDir(), "Moved Portable Game")
	if err := os.Rename(output, moved); err != nil {
		t.Fatal(err)
	}
	if err := os.RemoveAll(source); err != nil {
		t.Fatal(err)
	}
	if err := os.RemoveAll(bins); err != nil {
		t.Fatal(err)
	}
	t.Setenv("HOME", home)
	t.Setenv("AR_USER_DATA_DIR", "")
	launch := func(app string) {
		t.Helper()
		command := exec.Command(filepath.Join(app, "Contents", "MacOS", "actraiser-builder"), "app-launch")
		command.Dir = t.TempDir()
		if data, err := command.CombinedOutput(); err != nil {
			t.Fatalf("launch: %v: %s", err, data)
		}
	}
	app := filepath.Join(moved, Name+".app")
	launch(app)
	if read(t, filepath.Join(moved, "storage-probe.txt")) != "ran" {
		t.Fatal("portable launch used wrong data root")
	}
	globalApp := filepath.Join(t.TempDir(), "Renamed Global.app")
	if err := os.Rename(app, globalApp); err != nil {
		t.Fatal(err)
	}
	launch(globalApp)
	global := filepath.Join(home, "Library", "Application Support", Name, "game")
	if read(t, filepath.Join(global, "storage-probe.txt")) != "ran" {
		t.Fatal("global launch used wrong data root")
	}
	if _, err := os.Stat(filepath.Join(global, "game-assets", "manual.pdf")); err != nil {
		t.Fatal(err)
	}
}
