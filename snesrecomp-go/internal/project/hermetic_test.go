package project

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestObjectNameMangling(t *testing.T) {
	root := filepath.FromSlash("/proj")
	inside := objectName(root, filepath.FromSlash("/proj/src/gen/bank00_part00_v2.c"))
	if inside != "src_gen_bank00_part00_v2.c.o" {
		t.Fatalf("inside: %s", inside)
	}
	outside := objectName(root, filepath.FromSlash("/elsewhere/runtime/src/a.c"))
	if !strings.HasPrefix(outside, "ext_") || !strings.HasSuffix(outside, "a.c.o") {
		t.Fatalf("outside: %s", outside)
	}
	// Distinct outside paths with the same basename must not collide.
	other := objectName(root, filepath.FromSlash("/elsewhere2/runtime/src/a.c"))
	if other == outside {
		t.Fatalf("collision: %s", other)
	}
}

func TestObjectFresh(t *testing.T) {
	directory := t.TempDir()
	source := filepath.Join(directory, "a.c")
	object := filepath.Join(directory, "a.o")
	writeTestFile(t, source, "int x;\n")
	writeTestFile(t, object, "obj")

	old := time.Now().Add(-time.Hour)
	newer := time.Now().Add(time.Hour)
	if err := os.Chtimes(source, old, old); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(object, newer, newer); err != nil {
		t.Fatal(err)
	}
	if !objectFresh(source, object, time.Time{}) {
		t.Fatal("object newer than source should be fresh")
	}
	if objectFresh(source, object, time.Now().Add(2*time.Hour)) {
		t.Fatal("newer header must invalidate the object")
	}
	if err := os.Chtimes(source, newer.Add(time.Hour), newer.Add(time.Hour)); err != nil {
		t.Fatal(err)
	}
	if objectFresh(source, object, time.Time{}) {
		t.Fatal("newer source must invalidate the object")
	}
	if objectFresh(filepath.Join(directory, "missing.c"), object, time.Time{}) {
		t.Fatal("missing source must not count as fresh")
	}
	if objectFresh(source, filepath.Join(directory, "missing.o"), time.Time{}) {
		t.Fatal("missing object must not count as fresh")
	}
}

func TestNewestHeaderTime(t *testing.T) {
	directory := t.TempDir()
	writeTestFile(t, filepath.Join(directory, "a.h"), "x")
	writeTestFile(t, filepath.Join(directory, "b.c"), "x")
	nested := filepath.Join(directory, "nested")
	writeTestFile(t, filepath.Join(nested, "deep.h"), "x")

	when := time.Now().Add(30 * time.Minute)
	if err := os.Chtimes(filepath.Join(directory, "a.h"), when, when); err != nil {
		t.Fatal(err)
	}
	newest := newestHeaderTime([]string{directory, filepath.Join(directory, "missing")})
	if !newest.Equal(when.Truncate(time.Second)) && !newest.After(when.Add(-time.Second)) {
		t.Fatalf("newest: %v want ~%v", newest, when)
	}
	// Scanning the nested dir directly still finds its own header.
	nestedOnly := newestHeaderTime([]string{nested})
	if nestedOnly.IsZero() {
		t.Fatal("nested dir scan found nothing")
	}
	// Recursive by contract: a header one level down (nested/deep.h) is found
	// when scanning the PARENT, so a nested SDL3/*.h update is not missed.
	deepWhen := time.Now().Add(90 * time.Minute)
	if err := os.Chtimes(filepath.Join(nested, "deep.h"), deepWhen, deepWhen); err != nil {
		t.Fatal(err)
	}
	recursive := newestHeaderTime([]string{directory})
	if !recursive.After(when.Add(-time.Second)) || recursive.Before(deepWhen.Add(-time.Second)) {
		t.Fatalf("recursive scan missed nested/deep.h: got %v want ~%v", recursive, deepWhen)
	}
}

func TestFirstFlagValue(t *testing.T) {
	// SDL3's sdl3.pc reports the parent include dir (the game includes
	// <SDL3/SDL.h>), so the first -I is taken verbatim.
	if got := firstFlagValue("-I/opt/homebrew/include -D_THREAD_SAFE", "-I"); got != "/opt/homebrew/include" {
		t.Fatalf("got %q", got)
	}
	if got := firstFlagValue("", "-I"); got != "" {
		t.Fatalf("got %q", got)
	}
	if got := firstFlagValue("-I", "-I"); got != "" {
		t.Fatalf("bare prefix: %q", got)
	}
}

func TestSdlLibDirHasLib(t *testing.T) {
	dir := t.TempDir()
	// Empty dir: exists but holds no SDL3 library -> must be rejected. This is
	// the /usr/lib case on Debian/Ubuntu where the .so lives in a multiarch dir.
	if sdlLibDirHasLib(dir) {
		t.Fatal("empty lib dir accepted")
	}
	// A directory that does not exist is likewise rejected.
	if sdlLibDirHasLib(filepath.Join(dir, "missing")) {
		t.Fatal("missing lib dir accepted")
	}
	// A Linux-style versioned shared object is accepted.
	writeTestFile(t, filepath.Join(dir, "libSDL3.so.0"), "x")
	if !sdlLibDirHasLib(dir) {
		t.Fatal("libSDL3.so.0 not accepted")
	}
	// A macOS-style dylib in a fresh dir is accepted too.
	macDir := t.TempDir()
	writeTestFile(t, filepath.Join(macDir, "libSDL3.dylib"), "x")
	if !sdlLibDirHasLib(macDir) {
		t.Fatal("libSDL3.dylib not accepted")
	}
}

func TestHermeticBuildRejectsSourceDrift(t *testing.T) {
	// A source list that has drifted from CMakeLists.txt must stop the build
	// up front, not surface as an undefined-symbol wall at link time.
	root := t.TempDir()
	writeTestFile(t, filepath.Join(root, "snesbuild.ini"), `
[project]
name = MyGame
source = src/main.c
`)
	writeTestFile(t, filepath.Join(root, "CMakeLists.txt"), `
add_executable(MyGame
    src/main.c
    src/late_addition.c
)
`)
	_, err := HermeticBuild(HermeticOptions{Paths: DefaultPaths(root), ZigPath: "zig"})
	if err == nil {
		t.Fatal("drifted source list built without complaint")
	}
	if !strings.Contains(err.Error(), "src/late_addition.c") {
		t.Fatalf("error names no drifted source: %v", err)
	}
}
