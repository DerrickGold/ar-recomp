package project

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestSDL3TtfClosedSDKPreflight(t *testing.T) {
	root := t.TempDir()
	options := HermeticOptions{Target: "x86_64-windows-gnu", SDLIncludeDir: filepath.Join(root, "include"), SDLLibDir: filepath.Join(root, "lib")}
	writeTestFile(t, filepath.Join(options.SDLIncludeDir, "SDL3_ttf", "SDL_ttf.h"), "header")
	writeTestFile(t, filepath.Join(options.SDLLibDir, "SDL3.dll"), "base is not ttf")
	if _, _, err := resolveSDL3Ttf(options, true); err == nil {
		t.Fatal("cross SDK borrowed host SDL3_ttf")
	}
	writeTestFile(t, filepath.Join(options.SDLLibDir, "SDL3_ttf.dll"), "runtime")
	if _, _, err := resolveSDL3Ttf(options, false); err == nil {
		t.Fatal("SDK has no link library")
	}
	writeTestFile(t, filepath.Join(options.SDLLibDir, "libSDL3_ttf.dll.a"), "import library")
	include, lib, err := resolveSDL3Ttf(options, false)
	if err != nil || include != options.SDLIncludeDir || lib != options.SDLLibDir {
		t.Fatal(include, lib, err)
	}
	if err := os.Remove(filepath.Join(options.SDLIncludeDir, "SDL3_ttf", "SDL_ttf.h")); err != nil {
		t.Fatal(err)
	}
	if _, _, err := resolveSDL3Ttf(options, false); err == nil {
		t.Fatal("SDK has no header")
	}
	options.Target = ""
	if _, _, err := resolveSDL3Ttf(options, false); err == nil {
		t.Fatal("closed native SDK borrowed host fonts")
	}
}

func TestSDL3TtfNativeArchitectureAndIdentity(t *testing.T) {
	root := t.TempDir()
	hostClass, hostMachine, foreignClass, foreignMachine := hostAndForeignELF(t)
	file := filepath.Join(root, "libSDL3_ttf.so.0")
	writeTestELF(t, file, foreignClass, foreignMachine)
	usable, wrong := sharedLibraryDirLibs(root, "SDL3_ttf")
	if len(usable) != 0 || len(wrong) != 1 {
		t.Fatal(usable, wrong)
	}
	writeTestELF(t, file, hostClass, hostMachine)
	usable, wrong = sharedLibraryDirLibs(root, "SDL3_ttf")
	if len(usable) != 1 || len(wrong) != 0 {
		t.Fatal(usable, wrong)
	}
	if sdlLibDirHasLib(root) {
		t.Fatal("SDL3_ttf cannot stand in for SDL3")
	}
	if !(Manifest{Link: []string{"-lm", "-lSDL3_ttf"}}).UsesSDL3Ttf() || (Manifest{Link: []string{"-lSDL3"}}).UsesSDL3Ttf() {
		t.Fatal("manifest dependency selection")
	}
	if runtime.GOOS != "windows" {
		include := filepath.Join(root, "include")
		writeTestFile(t, filepath.Join(include, "SDL3_ttf", "SDL_ttf.h"), "header")
		if _, _, err := resolveSDL3Ttf(HermeticOptions{SDLIncludeDir: include, SDLLibDir: root}, false); err == nil {
			t.Fatal("versioned runtime alone is not a development link library")
		}
		linkName := "libSDL3_ttf.so"
		if runtime.GOOS == "darwin" {
			linkName = "libSDL3_ttf.dylib"
		}
		writeTestELF(t, filepath.Join(root, linkName), hostClass, hostMachine)
		got, _, err := resolveSDL3Ttf(HermeticOptions{SDLIncludeDir: filepath.Join(include, "SDL3"), SDLLibDir: root}, false)
		if err != nil || got != include {
			t.Fatal(got, err)
		}
	}
}

func TestSDL3TtfPreflightUsesManifest(t *testing.T) {
	paths, err := DefaultPaths(t.TempDir()).Resolve()
	if err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, paths.ROM, "fixture")
	manifest := filepath.Join(paths.Root, ManifestFileName)
	writeTestFile(t, manifest, "name=Test\nsource=main.c\nsdl3=true\nlink=-lSDL3_ttf\n")
	checks := Preflight(PreflightOptions{Paths: paths, Target: "x86_64-windows-gnu"})
	found := false
	for _, check := range checks {
		if check.Name == "SDL3_ttf development files" {
			found = true
			if check.Status != PreflightFail {
				t.Fatal(check)
			}
		}
	}
	if !found {
		t.Fatal("manifest's required SDL3_ttf not preflighted", checks)
	}
}

func TestSDL3TtfCrossELFUsesTargetArchitecture(t *testing.T) {
	root := t.TempDir()
	include, lib := filepath.Join(root, "include"), filepath.Join(root, "lib")
	writeTestFile(t, filepath.Join(include, "SDL3_ttf", "SDL_ttf.h"), "header")
	if err := os.MkdirAll(lib, 0755); err != nil {
		t.Fatal(err)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		class, machine, _ := elfArchitectureForGOARCH(arch)
		writeTestELF(t, filepath.Join(lib, "libSDL3_ttf.so"), class, machine)
		target := map[string]string{"amd64": "x86_64-linux-gnu", "arm64": "aarch64-linux-gnu"}[arch]
		if _, _, err := resolveSDL3Ttf(HermeticOptions{SDLIncludeDir: include, SDLLibDir: lib, Target: target}, false); err != nil {
			t.Fatal(arch, err)
		}
	}
}
