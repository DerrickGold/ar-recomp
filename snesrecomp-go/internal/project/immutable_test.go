package project

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestImmutableCacheKeyTracksContentNotMountPath(t *testing.T) {
	id := strings.Repeat("a", 64)
	key := func(root, id, optimize string) string {
		return immutableCacheKey(HermeticOptions{InputID: id, Paths: DefaultPaths(root)}, strings.Join([]string{filepath.Join(root, "tools", "zig"), "cc", optimize, "-I" + filepath.Join(root, "src"), "-I/external/headers"}, "\x00"))
	}
	a, b := filepath.Join(t.TempDir(), "mount a"), filepath.Join(t.TempDir(), "mount b")
	if key(a, id, "-O2") != key(b, id, "-O2") {
		t.Fatal("mount path invalidates immutable cache")
	}
	if key(a, id, "-O2") == key(a, strings.Repeat("b", 64), "-O2") {
		t.Fatal("changed payload reused cache")
	}
	if key(a, id, "-O2") == key(a, id, "-O0") {
		t.Fatal("changed flags reused cache")
	}
}

func TestImmutableOutputsRejectAliasesBeforeWriting(t *testing.T) {
	root, scratch := t.TempDir(), t.TempDir()
	paths := DefaultPaths(root)
	paths.GeneratedDir, paths.FuncsHeader, paths.BuildDir = filepath.Join(scratch, "gen"), filepath.Join(scratch, "include", "funcs.h"), filepath.Join(scratch, "build")
	options := HermeticOptions{Paths: paths, InputID: strings.Repeat("a", 64)}
	if err := validateImmutableOutputs(options); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(root, paths.GeneratedDir); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	if err := validateImmutableOutputs(options); err == nil {
		t.Fatal("accepted output alias into bundle")
	}
}

// Real native compilation with immutable authored inputs and mutable generated
// inputs. No ROM is needed. Also simulates an AppImage remount and an upgrade.
func TestImmutableBuildReadOnlyRelocationAndInvalidation(t *testing.T) {
	zig := os.Getenv("SNESBUILD_ZIG")
	if zig == "" {
		zig, _ = filepath.Abs("../../../build/toolchain/zig-aarch64-macos-0.16.0/zig")
		if _, err := os.Stat(zig); err != nil {
			zig, _ = exec.LookPath("zig")
		}
	}
	if zig == "" {
		t.Skip("Zig unavailable")
	}
	parent, scratch := t.TempDir(), t.TempDir()
	root := filepath.Join(parent, "read only bundle")
	writeTestFile(t, filepath.Join(root, "snesbuild.ini"), "[project]\nname = MyGame\nsource = src/main.c\ninclude = recomp\n")
	writeTestFile(t, filepath.Join(root, "CMakeLists.txt"), "add_executable(MyGame\n    src/main.c\n)\n")
	writeTestFile(t, filepath.Join(root, "src", "main.c"), "#include <stdio.h>\n#include \"funcs.h\"\nint main(void) { printf(\"%d\\n\", GENERATED_VALUE); return 0; }\n")
	writeTestFile(t, filepath.Join(root, "recomp", "funcs.h"), "#error stale bundled generated header must not be used\n")
	runner := filepath.Join(root, "snesrecomp-go", "runtime")
	writeTestFile(t, filepath.Join(runner, "src", "runner.c"), "int runner_symbol;\n")
	writeTestFile(t, filepath.Join(runner, "include", "runner.h"), "/* immutable */\n")
	writeTestFile(t, filepath.Join(runner, "runner.cmake"), `
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/runner.c
)
set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
)
`)
	paths := DefaultPaths(root)
	paths.GeneratedDir, paths.FuncsHeader, paths.BuildDir = filepath.Join(scratch, "gen"), filepath.Join(scratch, "include", "funcs.h"), filepath.Join(scratch, "build")
	writeTestFile(t, filepath.Join(paths.GeneratedDir, "unit.c"), "int generated_symbol;\n")
	writeTestFile(t, paths.FuncsHeader, "#define GENERATED_VALUE 42\n")
	t.Setenv("ZIG_GLOBAL_CACHE_DIR", filepath.Join(scratch, "zig-global"))
	t.Setenv("ZIG_LOCAL_CACHE_DIR", filepath.Join(scratch, "zig-local"))
	snapshot := func() string {
		t.Helper()
		h := sha256.New()
		err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			rel, _ := filepath.Rel(root, path)
			fmt.Fprintln(h, rel)
			if !entry.IsDir() {
				data, err := os.ReadFile(path)
				if err != nil {
					return err
				}
				h.Write(data)
			}
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
		return fmt.Sprintf("%x", h.Sum(nil))
	}
	setReadOnly := func() {
		t.Helper()
		if err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			mode := os.FileMode(0444)
			if entry.IsDir() {
				mode = 0555
			}
			return os.Chmod(path, mode)
		}); err != nil {
			t.Fatal(err)
		}
	}
	t.Cleanup(func() {
		_ = filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
			if err == nil && entry.IsDir() {
				return os.Chmod(path, 0755)
			}
			return err
		})
	})
	before := snapshot()
	setReadOnly()
	id := before
	build := func(wantCompiled, wantOutput string) {
		t.Helper()
		var log bytes.Buffer
		binary, err := HermeticBuild(HermeticOptions{Paths: paths, InputID: id, ZigPath: zig, Jobs: 2, Stdout: &log, Stderr: &log})
		if err != nil {
			t.Fatalf("build: %v\n%s", err, &log)
		}
		if !strings.Contains(log.String(), wantCompiled) {
			t.Fatalf("cache: want %q\n%s", wantCompiled, &log)
		}
		data, err := exec.Command(binary).CombinedOutput()
		if err != nil || strings.TrimSpace(string(data)) != wantOutput {
			t.Fatalf("game: %q %v", data, err)
		}
		if snapshot() != before {
			t.Fatal("build mutated authored inputs")
		}
	}
	build("0 cached, 3 to compile", "42")
	build("3 cached, 0 to compile", "42")
	moved := filepath.Join(parent, "remounted bundle")
	// macOS requires the directory itself to be writable during rename.
	if err := os.Chmod(root, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(root, moved); err != nil {
		t.Fatal(err)
	}
	root, paths.Root = moved, moved
	setReadOnly()
	build("3 cached, 0 to compile", "42")
	writeTestFile(t, paths.FuncsHeader, "#define GENERATED_VALUE 43\n")
	future := time.Now().Add(time.Hour)
	if err := os.Chtimes(paths.FuncsHeader, future, future); err != nil {
		t.Fatal(err)
	}
	build("1 cached, 2 to compile", "43")
	id = strings.Repeat("b", 64)
	build("0 cached, 3 to compile", "43")
}
