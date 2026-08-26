package project

import (
	"os"
	"path/filepath"
	"runtime"
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

func TestObjectArchiveFormat(t *testing.T) {
	for targetOS, want := range map[string]string{
		"darwin":  "darwin",
		"linux":   "gnu",
		"windows": "coff",
	} {
		if got := objectArchiveFormat(targetOS); got != want {
			t.Errorf("objectArchiveFormat(%q) = %q, want %q", targetOS, got, want)
		}
	}
}

func TestForceLoadArchiveArgs(t *testing.T) {
	archive := filepath.FromSlash("/tmp/build with spaces/objects.a")
	darwin := forceLoadArchiveArgs("darwin", archive)
	if len(darwin) != 1 || darwin[0] != "-Wl,-force_load,"+archive {
		t.Fatalf("Darwin force-load args: %#v", darwin)
	}
	for _, targetOS := range []string{"linux", "windows"} {
		got := forceLoadArchiveArgs(targetOS, archive)
		want := []string{"-Wl,--whole-archive", archive, "-Wl,--no-whole-archive"}
		if strings.Join(got, "\x00") != strings.Join(want, "\x00") {
			t.Errorf("%s force-load args: %#v, want %#v", targetOS, got, want)
		}
	}
}

func TestArchiveResponseFileQuoting(t *testing.T) {
	response, err := archiveResponseFile([]string{
		filepath.FromSlash("/tmp/plain.o"),
		filepath.FromSlash("/tmp/build with spaces/quoted\"name.o"),
		`C:\build dir\bank00.o`,
	})
	if err != nil {
		t.Fatal(err)
	}
	want := "\"/tmp/plain.o\"\n" +
		"\"/tmp/build with spaces/quoted\\\"name.o\"\n" +
		"\"C:\\\\build dir\\\\bank00.o\"\n"
	if string(response) != want {
		t.Fatalf("response file:\n%q\nwant:\n%q", response, want)
	}
	if _, err := archiveResponseFile([]string{"bad\npath.o"}); err == nil {
		t.Fatal("newline-containing object path was accepted")
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
	if err := os.WriteFile(object, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(object, newer.Add(3*time.Hour), newer.Add(3*time.Hour)); err != nil {
		t.Fatal(err)
	}
	if objectFresh(source, object, time.Time{}) {
		t.Fatal("zero-byte object left by a failed compiler must not count as fresh")
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
	newest := newestHeaderTime([]string{directory, filepath.Join(directory, "missing")}, "")
	if !newest.Equal(when.Truncate(time.Second)) && !newest.After(when.Add(-time.Second)) {
		t.Fatalf("newest: %v want ~%v", newest, when)
	}
	// Scanning the nested dir directly still finds its own header.
	nestedOnly := newestHeaderTime([]string{nested}, "")
	if nestedOnly.IsZero() {
		t.Fatal("nested dir scan found nothing")
	}
	// Recursive by contract: a header one level down (nested/deep.h) is found
	// when scanning the PARENT, so a nested SDL3/*.h update is not missed.
	deepWhen := time.Now().Add(90 * time.Minute)
	if err := os.Chtimes(filepath.Join(nested, "deep.h"), deepWhen, deepWhen); err != nil {
		t.Fatal(err)
	}
	recursive := newestHeaderTime([]string{directory}, "")
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

func TestTargetOSFromTriple(t *testing.T) {
	for triple, want := range map[string]string{
		"x86_64-windows-gnu":  "windows",
		"aarch64-windows-gnu": "windows",
		"x86_64-linux-gnu":    "linux",
		// Zig spells Darwin "macos"; the rest of the build switches on GOOS
		// names, so this is the one field that needs translating.
		"aarch64-macos-none": "darwin",
	} {
		if got := TargetOS(triple); got != want {
			t.Errorf("TargetOS(%q) = %q, want %q", triple, got, want)
		}
	}
	// An empty target is a host build, and a malformed one must not silently
	// pick some other platform's link flags.
	if got := TargetOS(""); got != runtime.GOOS {
		t.Errorf("TargetOS(\"\") = %q, want host %q", got, runtime.GOOS)
	}
	if got := TargetOS("nonsense"); got != runtime.GOOS {
		t.Errorf("TargetOS(\"nonsense\") = %q, want host %q", got, runtime.GOOS)
	}
}

func TestResolveSDL3CrossRequiresStagedCopy(t *testing.T) {
	buildDir := t.TempDir()
	options := HermeticOptions{Target: "x86_64-windows-gnu"}
	options.BuildDir = buildDir

	// Nothing staged: the error must name the fix rather than letting the
	// build reach a link failure, and must never fall back to the host SDL.
	_, _, _, err := resolveSDL3(options)
	if err == nil {
		t.Fatal("expected an error when no cross SDL3 is staged")
	}
	if !strings.Contains(err.Error(), "sdl stage") {
		t.Fatalf("error should point at `snesbuild sdl stage`: %v", err)
	}

	staged := CrossSDL3Dir(buildDir, options.Target)
	if err := os.MkdirAll(filepath.Join(staged, "include", "SDL3"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(staged, "lib"), 0o755); err != nil {
		t.Fatal(err)
	}
	include, lib, bundled, err := resolveSDL3(options)
	if err != nil {
		t.Fatal(err)
	}
	if include != filepath.Join(staged, "include") || lib != filepath.Join(staged, "lib") {
		t.Fatalf("staged dirs not used: %s %s", include, lib)
	}
	// Reported as bundled so the DLL is copied beside the cross-built binary,
	// matching what the distribution bundle does.
	if !bundled {
		t.Error("a staged cross SDL3 is a bundled copy")
	}
}

func TestNewestHeaderTimeSkipsBuildDir(t *testing.T) {
	root := t.TempDir()
	buildDir := filepath.Join(root, "build")
	writeTestFile(t, filepath.Join(root, "src", "game.h"), "int a;\n")
	// A cross target stages SDL3 headers under the build directory. Because
	// the manifest lists `include = .`, an unpruned walk would see these and
	// mark every native object stale on the next build.
	staged := filepath.Join(buildDir, "hermetic", "x86_64-windows-gnu", "sdl3", "include", "SDL3")
	writeTestFile(t, filepath.Join(staged, "SDL.h"), "int b;\n")
	future := time.Now().Add(time.Hour)
	if err := os.Chtimes(filepath.Join(staged, "SDL.h"), future, future); err != nil {
		t.Fatal(err)
	}

	pruned := newestHeaderTime([]string{root}, buildDir)
	if !pruned.Before(future) {
		t.Fatalf("build dir was not pruned: got %v, staged header is %v", pruned, future)
	}
	// The cross build passes its staged include dir explicitly; that walk
	// starts below the build directory and must still see the header.
	direct := newestHeaderTime([]string{filepath.Dir(staged)}, buildDir)
	if !direct.Equal(future) {
		t.Fatalf("explicit staged include dir should still count: got %v", direct)
	}
}
