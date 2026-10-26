package desktop

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/appicons"
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
	put(t, filepath.Join(source, "moduledep.c"), "int module_dependency(void) { return 7; }\n")
	put(t, filepath.Join(source, "module.c"), "extern int module_dependency(void); int module_value(void) { return module_dependency(); }\n")
	put(t, filepath.Join(source, "main.c"), `#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
extern int middle(void);
int main(int argc, char **argv) {
  char path[4096];
  snprintf(path, sizeof(path), "%s", argv[0]);
  char *leaf = strrchr(path, '/');
  if (!leaf) return 1;
#ifdef __APPLE__
  strcpy(leaf + 1, "actraiser2-native.dylib");
#else
  strcpy(leaf + 1, "actraiser2-native.so");
#endif
  void *module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!module) { fprintf(stderr, "%s\n", dlerror()); return 2; }
  int (*value)(void) = (int (*)(void))dlsym(module, "module_value");
  if (!value) return 3;
  printf("%d %d\n", middle(), value());
  dlclose(module);
  return 0;
}
`)
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
		build("-dynamiclib", "moduledep.c", "-o", "libmoduledep.dylib", "-Wl,-install_name,@rpath/libmoduledep.dylib")
		build("-dynamiclib", "module.c", "-L.", "-lmoduledep", "-o", "actraiser2-native.dylib", "-Wl,-rpath,"+source)
	} else {
		build("-shared", "-fPIC", "base.c", "-o", "libbase.so", "-Wl,-soname,libbase.so")
		build("-shared", "-fPIC", "middle.c", "-L.", "-lbase", "-o", "libmiddle.so", "-Wl,-soname,libmiddle.so", "-Wl,-rpath,"+source)
		build("-shared", "-fPIC", "moduledep.c", "-o", "libmoduledep.so", "-Wl,-soname,libmoduledep.so")
		build("-shared", "-fPIC", "module.c", "-L.", "-lmoduledep", "-o", "actraiser2-native.so", "-Wl,-rpath,"+source)
	}
	build("main.c", "-L.", "-lmiddle", "-ldl", "-Wl,-rpath,"+source, "-o", Name)
	if runtime.GOOS == "linux" {
		// An inherited path must not replace the SDK the binary was built with.
		wrong := t.TempDir()
		put(t, filepath.Join(wrong, "base.c"), "int base(void) { return -1; }\n")
		build("-shared", "-fPIC", filepath.Join(wrong, "base.c"), "-o", filepath.Join(wrong, "libbase.so"), "-Wl,-soname,libbase.so")
		t.Setenv("LD_LIBRARY_PATH", wrong)
	}
	if runtime.GOOS == "darwin" {
		// Package generation itself must not invoke an Xcode/CLT shim.
		t.Setenv("DEVELOPER_DIR", filepath.Join(t.TempDir(), "no-developer-tools"))
	}
	rom := nativePackageFixture(t, root)
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
		icon, err := os.ReadFile(filepath.Join(resources, Name+".icns"))
		if err != nil || !bytes.Equal(icon, appicons.Game.ICNS()) {
			t.Fatal("missing macOS game icon", err)
		}
		plist, err := os.ReadFile(filepath.Join(moved, "Contents/Info.plist"))
		if err != nil || !bytes.Contains(plist, []byte("<key>CFBundleIconFile</key><string>"+Name+".icns</string>")) {
			t.Fatal("missing icon plist entry", err)
		}
	} else if err := appicons.Game.ValidateAppDir(moved); err != nil {
		t.Fatal(err)
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
	if err != nil || strings.TrimSpace(string(output)) != "42 7" {
		t.Fatalf("relocated app: %v: %s", err, output)
	}
}

// The package API intentionally accepts synthetic inputs; unlike the public
// package command these tests never extract retail data or need a game ROM.
func nativePackageFixture(t *testing.T, root string) string {
	t.Helper()
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
	return rom
}
