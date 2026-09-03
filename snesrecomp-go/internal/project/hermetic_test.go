package project

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
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

func TestRuntimeArchiveName(t *testing.T) {
	for targetOS, want := range map[string]string{
		"darwin":  "libsnesrecomp_runtime.a",
		"linux":   "libsnesrecomp_runtime.a",
		"windows": "snesrecomp_runtime.lib",
	} {
		if got := runtimeArchiveName(targetOS); got != want {
			t.Errorf("runtimeArchiveName(%q) = %q, want %q",
				targetOS, got, want)
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

func TestNewestImplementationIncludeTimeWalksUnitySources(t *testing.T) {
	directory := t.TempDir()
	unit := filepath.Join(directory, "unit.cpp")
	core := filepath.Join(directory, "core.cpp")
	table := filepath.Join(directory, "tables.inc")
	writeTestFile(t, unit, "#include \"core.cpp\"\n")
	writeTestFile(t, core, "#include \"tables.inc\"\n")
	writeTestFile(t, table, "static const int table[] = { 1 };\n")

	when := time.Now().Add(2 * time.Hour)
	if err := os.Chtimes(table, when, when); err != nil {
		t.Fatal(err)
	}
	newest := newestImplementationIncludeTime(unit)
	if newest.Before(when.Add(-time.Second)) {
		t.Fatalf("unity dependency scan missed tables.inc: got %v want ~%v", newest, when)
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

// writeTestELF emits a minimal, well-formed ELF header (no program or section
// table) so the architecture checks can be exercised without shipping binary
// fixtures or shelling out to a cross compiler.
func writeTestELF(t *testing.T, path string, class elf.Class, machine elf.Machine) {
	t.Helper()
	var buffer bytes.Buffer
	identifier := make([]byte, 16)
	copy(identifier, []byte{0x7f, 'E', 'L', 'F'})
	identifier[4] = byte(class)
	identifier[5] = byte(elf.ELFDATA2LSB)
	identifier[6] = byte(elf.EV_CURRENT)
	buffer.Write(identifier)
	write16 := func(value uint16) { _ = binary.Write(&buffer, binary.LittleEndian, value) }
	write32 := func(value uint32) { _ = binary.Write(&buffer, binary.LittleEndian, value) }
	write16(uint16(elf.ET_DYN))
	write16(uint16(machine))
	write32(uint32(elf.EV_CURRENT))
	if class == elf.ELFCLASS64 {
		for range 3 { // e_entry, e_phoff, e_shoff
			_ = binary.Write(&buffer, binary.LittleEndian, uint64(0))
		}
		write32(0)  // e_flags
		write16(64) // e_ehsize
		write16(56) // e_phentsize
		write16(0)  // e_phnum
		write16(64) // e_shentsize
		write16(0)  // e_shnum
		write16(0)  // e_shstrndx
	} else {
		for range 3 {
			write32(0)
		}
		write32(0)
		write16(52)
		write16(32)
		write16(0)
		write16(40)
		write16(0)
		write16(0)
	}
	if err := os.WriteFile(path, buffer.Bytes(), 0o644); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
}

// hostAndForeignELF returns an architecture this build can link and one it
// cannot, so the tests read the same on an x86_64 CI box and an arm64 laptop.
func hostAndForeignELF(t *testing.T) (hostClass elf.Class, hostMachine elf.Machine,
	foreignClass elf.Class, foreignMachine elf.Machine) {
	t.Helper()
	class, machine, supported := elfArchitectureForGOARCH(runtime.GOARCH)
	if !supported {
		t.Skipf("no ELF architecture mapping for %s", runtime.GOARCH)
	}
	if class == elf.ELFCLASS64 && machine == elf.EM_X86_64 {
		return class, machine, elf.ELFCLASS32, elf.EM_386
	}
	return class, machine, elf.ELFCLASS64, elf.EM_X86_64
}

func TestSdlLibraryArchitectureMatches(t *testing.T) {
	dir := t.TempDir()
	hostClass, hostMachine, foreignClass, foreignMachine := hostAndForeignELF(t)

	host := filepath.Join(dir, "libSDL3.so.0")
	writeTestELF(t, host, hostClass, hostMachine)
	if matches, known := sdlLibraryArchitectureMatches(host); !matches || !known {
		t.Fatalf("host object rejected: matches=%v known=%v", matches, known)
	}

	foreign := filepath.Join(dir, "foreign.so")
	writeTestELF(t, foreign, foreignClass, foreignMachine)
	if matches, known := sdlLibraryArchitectureMatches(foreign); matches || !known {
		t.Fatalf("foreign object accepted: matches=%v known=%v", matches, known)
	}

	// Anything we cannot parse stays permissive: a linker script or a stub must
	// not be reported as the wrong architecture.
	stub := filepath.Join(dir, "stub.so")
	writeTestFile(t, stub, "INPUT(libSDL3.so.0)")
	if matches, known := sdlLibraryArchitectureMatches(stub); matches || known {
		t.Fatalf("unparseable object claimed: matches=%v known=%v", matches, known)
	}
}

func TestSdlLibDirRejectsForeignArchitecture(t *testing.T) {
	// The reported multilib failure: /usr/lib holds a 32-bit libSDL3.so while
	// the 64-bit one lives in /usr/lib64. Accepting the first directory that
	// merely has the file is what produced `ld.lld: ... is incompatible with
	// elf64-x86-64` late in the link.
	hostClass, hostMachine, foreignClass, foreignMachine := hostAndForeignELF(t)

	wrong := t.TempDir()
	writeTestELF(t, filepath.Join(wrong, "libSDL3.so"), foreignClass, foreignMachine)
	if sdlLibDirHasLib(wrong) {
		t.Fatal("foreign-architecture lib dir accepted")
	}
	usable, foreignFound := sdlLibDirLibs(wrong)
	if len(usable) != 0 || len(foreignFound) != 1 {
		t.Fatalf("usable=%v foreign=%v", usable, foreignFound)
	}

	right := t.TempDir()
	writeTestELF(t, filepath.Join(right, "libSDL3.so"), hostClass, hostMachine)
	if !sdlLibDirHasLib(right) {
		t.Fatal("host-architecture lib dir rejected")
	}
}

func TestSdlIncludeDirHasHeaders(t *testing.T) {
	root := t.TempDir()
	if sdlIncludeDirHasHeaders(root) {
		t.Fatal("include dir without SDL3/ accepted")
	}
	if sdlIncludeDirHasHeaders("") {
		t.Fatal("empty include dir accepted")
	}
	leaf := filepath.Join(root, "SDL3")
	if err := os.MkdirAll(leaf, 0o755); err != nil {
		t.Fatal(err)
	}
	// The parent form (<SDL3/SDL.h> resolves) and the SDL3/ leaf form that some
	// distributions report in Cflags are both accepted.
	if !sdlIncludeDirHasHeaders(root) {
		t.Fatal("parent include dir rejected")
	}
	if !sdlIncludeDirHasHeaders(leaf) {
		t.Fatal("SDL3 leaf include dir rejected")
	}
}

func TestGnuMultiarchDirTracksHost(t *testing.T) {
	switch runtime.GOARCH {
	case "amd64":
		if got := gnuMultiarchDir(); got != "x86_64-linux-gnu" {
			t.Fatalf("got %q", got)
		}
	case "arm64":
		if got := gnuMultiarchDir(); got != "aarch64-linux-gnu" {
			t.Fatalf("got %q", got)
		}
	}
}

func TestSdlLibDirLibsIgnoresEmptyDir(t *testing.T) {
	// An empty dir string must not be joined into a glob against the working
	// directory, which could match an unrelated libSDL3 there.
	if usable, wrong := sdlLibDirLibs(""); usable != nil || wrong != nil {
		t.Fatalf("usable=%v wrong=%v", usable, wrong)
	}
	if sdlLibDirHasLib("") {
		t.Fatal("empty lib dir accepted")
	}
}
