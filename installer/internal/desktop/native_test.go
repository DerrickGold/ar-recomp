package desktop

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// Compile a tiny executable and a transitive shared-library dependency, move
// the finished app, remove the original libraries, then execute it. This tests
// relocation using the actual loader and signing tools, without a game ROM.
func TestNativeApplicationSurvivesRelocationAndMissingBuildLibraries(t *testing.T) {
	if runtime.GOOS != "darwin" && runtime.GOOS != "linux" {
		t.Skip("native desktop packaging")
	}
	cc, err := exec.LookPath("cc")
	if err != nil {
		t.Skip("C compiler unavailable")
	}
	root, source, destination := t.TempDir(), t.TempDir(), t.TempDir()
	put(t, filepath.Join(source, "base.c"), "int base(void) { return 40; }\n")
	put(t, filepath.Join(source, "middle.c"), "extern int base(void); int middle(void) { return base()+2; }\n")
	put(t, filepath.Join(source, "main.c"), "#include <stdio.h>\nextern int middle(void); int main(void) { printf(\"%d\\n\", middle()); return 0; }\n")
	build := func(args ...string) {
		t.Helper()
		command := exec.Command(cc, args...)
		command.Dir = source
		if output, err := command.CombinedOutput(); err != nil {
			t.Fatalf("cc: %v: %s", err, output)
		}
	}
	if runtime.GOOS == "darwin" {
		build("-dynamiclib", "base.c", "-o", "libbase.dylib", "-Wl,-install_name,@rpath/libbase.dylib")
		build("-dynamiclib", "middle.c", "-L.", "-lbase", "-o", "libmiddle.dylib", "-Wl,-install_name,@rpath/libmiddle.dylib")
	} else {
		build("-shared", "-fPIC", "base.c", "-o", "libbase.so", "-Wl,-soname,libbase.so")
		build("-shared", "-fPIC", "middle.c", "-L.", "-lbase", "-o", "libmiddle.so", "-Wl,-soname,libmiddle.so", "-Wl,-rpath,"+source)
	}
	build("main.c", "-L.", "-lmiddle", "-Wl,-rpath,"+source, "-o", Name)
	if runtime.GOOS == "darwin" {
		// Package generation itself must not invoke an Xcode/CLT shim.
		t.Setenv("DEVELOPER_DIR", filepath.Join(t.TempDir(), "no-developer-tools"))
	}
	for _, leaf := range []string{"config.ini", "diorama-layers.ini", "game-assets/manifest.ini"} {
		put(t, filepath.Join(root, "defaults", leaf), "[test]\n")
	}
	for _, leaf := range []string{"manual.pdf", "manifest.ini", "languages/native-us/pack.ini", "fonts/noto/NotoSans-SemiCondensedExtraBold.ttf", "fonts/noto/NotoSansJP-Bold.otf", "fonts/noto/NotoSansArabic-Bold.ttf", "fonts/noto/NotoSansHebrew-Bold.ttf", "fonts/noto/OFL.txt", "fonts/noto/NotoSansJP-OFL.txt"} {
		put(t, filepath.Join(root, "game-assets", leaf), "synthetic resource")
	}
	put(t, filepath.Join(root, "game-assets/languages/packs/.arlang-cache/private"), "must not ship")
	put(t, filepath.Join(root, "game-assets/languages/sources/private"), "must not ship")
	put(t, filepath.Join(root, "saves/save.srm"), "must not ship")
	rom := filepath.Join(root, "synthetic.sfc")
	put(t, rom, "synthetic ROM")
	format := "app"
	if runtime.GOOS == "linux" {
		format = "appdir"
	}
	artifact, err := Package(context.Background(), PackageOptions{
		Binary: filepath.Join(source, Name), Builder: filepath.Join(source, Name),
		ROM: rom, Root: root, Destination: destination, Format: format, Version: "test<&>",
	})
	if err != nil {
		t.Fatal(err)
	}
	moved := filepath.Join(t.TempDir(), "Renamed Game"+filepath.Ext(artifact.Path))
	if err := os.Rename(artifact.Path, moved); err != nil {
		t.Fatal(err)
	}
	// Only this test's generated build inputs are removed.
	if err := os.RemoveAll(source); err != nil {
		t.Fatal(err)
	}
	bin, resources, libs := filepath.Join(moved, "usr/bin"), filepath.Join(moved, "usr/share", Name), filepath.Join(moved, "usr/lib")
	if runtime.GOOS == "darwin" {
		bin, resources = filepath.Join(moved, "Contents/MacOS"), filepath.Join(moved, "Contents/Resources")
	}
	for _, excluded := range []string{"seed/saves/save.srm", "seed/game-assets/languages/sources/private", "seed/game-assets/languages/packs/.arlang-cache/private"} {
		if _, err := os.Stat(filepath.Join(resources, excluded)); !os.IsNotExist(err) {
			t.Fatalf("private input leaked: %s", excluded)
		}
	}
	command := exec.Command(filepath.Join(bin, Name))
	command.Dir = t.TempDir()
	if runtime.GOOS == "linux" {
		command.Env = append(os.Environ(), "LD_LIBRARY_PATH="+libs)
	}
	output, err := command.CombinedOutput()
	if err != nil || strings.TrimSpace(string(output)) != "42" {
		t.Fatalf("relocated app: %v: %s", err, output)
	}
}
