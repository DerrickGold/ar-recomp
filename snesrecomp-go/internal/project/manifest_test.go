package project

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func writeTestFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestLoadManifest(t *testing.T) {
	path := filepath.Join(t.TempDir(), "snesbuild.ini")
	writeTestFile(t, path, `
# comment
[project]
name = MyGame
sdl3 = true
link = -lm
define = FOO=1
define = BAR
include = src
source = src/main.c
source = src/game.c
`)
	manifest, err := LoadManifest(path)
	if err != nil {
		t.Fatal(err)
	}
	if manifest.Name != "MyGame" || !manifest.UseSDL3 || manifest.Std != "c11" {
		t.Fatalf("unexpected manifest: %+v", manifest)
	}
	if len(manifest.Sources) != 2 || manifest.Sources[1] != "src/game.c" {
		t.Fatalf("sources: %v", manifest.Sources)
	}
	if len(manifest.Defines) != 2 || len(manifest.Link) != 1 {
		t.Fatalf("defines/link: %v %v", manifest.Defines, manifest.Link)
	}
}

func TestLoadManifestRejects(t *testing.T) {
	cases := map[string]string{
		"missing name": "source = a.c\n",
		"no sources":   "name = X\n",
		"unknown key":  "name = X\nsource = a.c\nbogus = 1\n",
		"no equals":    "name X\n",
	}
	for label, content := range cases {
		path := filepath.Join(t.TempDir(), "snesbuild.ini")
		writeTestFile(t, path, content)
		if _, err := LoadManifest(path); err == nil {
			t.Errorf("%s: expected error", label)
		}
	}
}

func TestLoadRunnerManifest(t *testing.T) {
	runtimeDir := t.TempDir()
	writeTestFile(t, filepath.Join(runtimeDir, "runner.cmake"), `
set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})

set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/a.c
    # comment
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/b.c
)

option(SNESRECOMP_ENABLE_TRACE "dev" OFF)
if(SNESRECOMP_ENABLE_TRACE)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/debug_only.c
    )
endif()

set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/src/snes
)
`)
	manifest, err := LoadRunnerManifest(runtimeDir)
	if err != nil {
		t.Fatal(err)
	}
	if len(manifest.Sources) != 2 || !strings.HasSuffix(manifest.Sources[1], filepath.FromSlash("src/snes/b.c")) {
		t.Fatalf("sources: %v", manifest.Sources)
	}
	for _, source := range manifest.Sources {
		if strings.Contains(source, "debug_only") {
			t.Fatalf("conditional trace source must be excluded: %v", manifest.Sources)
		}
	}
	if len(manifest.PublicIncludes) != 1 ||
		!strings.HasSuffix(manifest.PublicIncludes[0], "include") ||
		len(manifest.PrivateIncludes) != 2 {
		t.Fatalf("includes: public=%v private=%v",
			manifest.PublicIncludes, manifest.PrivateIncludes)
	}
}

func TestLoadRunnerManifestFromIncludedManifest(t *testing.T) {
	runtimeDir := t.TempDir()
	writeTestFile(t, filepath.Join(runtimeDir, "runner.cmake"), `
set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})
include(${SNESRECOMP_RUNNER_ROOT}/sources.cmake)
set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
)
`)
	writeTestFile(t, filepath.Join(runtimeDir, "sources.cmake"), `
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/a.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/b.c
)
`)

	manifest, err := LoadRunnerManifest(runtimeDir)
	if err != nil {
		t.Fatal(err)
	}
	if len(manifest.Sources) != 2 || !strings.HasSuffix(manifest.Sources[1], filepath.FromSlash("src/snes/b.c")) {
		t.Fatalf("sources: %v", manifest.Sources)
	}
	if len(manifest.PrivateIncludes) != 1 ||
		!strings.HasSuffix(manifest.PrivateIncludes[0], "src") {
		t.Fatalf("private includes: %v", manifest.PrivateIncludes)
	}
}

func TestLoadRunnerManifestClassifiedIncludes(t *testing.T) {
	runtimeDir := t.TempDir()
	writeTestFile(t, filepath.Join(runtimeDir, "runner.cmake"), `
set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})
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

	manifest, err := LoadRunnerManifest(runtimeDir)
	if err != nil {
		t.Fatal(err)
	}
	if !slices.Equal(manifest.PublicIncludes,
		[]string{filepath.Join(runtimeDir, "include")}) ||
		!slices.Equal(manifest.PrivateIncludes,
			[]string{filepath.Join(runtimeDir, "src")}) {
		t.Fatalf("classified includes = public:%v private:%v",
			manifest.PublicIncludes, manifest.PrivateIncludes)
	}
}

func TestRealRunnerManifest(t *testing.T) {
	// internal/project -> snesrecomp-go/runtime.
	runtimeDir := filepath.Join("..", "..", "runtime")
	manifest, err := LoadRunnerManifest(runtimeDir)
	if err != nil {
		t.Fatalf("parse real runtime manifest: %v", err)
	}
	if len(manifest.Sources) == 0 || len(manifest.PublicIncludes) == 0 ||
		len(manifest.PrivateIncludes) == 0 {
		t.Fatalf("real runtime manifest is incomplete: %+v", manifest)
	}
	for _, include := range append(manifest.PublicIncludes, manifest.PrivateIncludes...) {
		if _, err := os.Stat(include); err != nil {
			t.Errorf("runtime lists include root %s, which does not exist: %v",
				include, err)
		}
	}
	for _, source := range manifest.Sources {
		if _, err := os.Stat(source); err != nil {
			t.Errorf("runtime lists %s, which does not exist: %v", source, err)
		}
	}
}

func TestLoadRunnerManifestCleansRelativePaths(t *testing.T) {
	toolchain := t.TempDir()
	runtimeDir := filepath.Join(toolchain, "runtime")
	writeTestFile(t, filepath.Join(runtimeDir, "runner.cmake"), `
set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/new.c
    ${SNESRECOMP_RUNNER_ROOT}/../shared/src/shared.c
)
set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/../shared/src
)
`)
	manifest, err := LoadRunnerManifest(runtimeDir)
	if err != nil {
		t.Fatal(err)
	}
	wantShared := filepath.Join(toolchain, "shared", "src", "shared.c")
	if manifest.Sources[1] != wantShared {
		t.Fatalf("relative source = %s, want %s", manifest.Sources[1], wantShared)
	}
	if strings.Contains(manifest.Sources[1], "..") ||
		strings.Contains(manifest.PrivateIncludes[1], "..") {
		t.Fatalf("relative paths were not cleaned: %+v", manifest)
	}
}

func TestManifestDriftWarnings(t *testing.T) {
	root := t.TempDir()
	writeTestFile(t, filepath.Join(root, "CMakeLists.txt"), `
add_executable(Game
    ${SNESRECOMP_RUNNER_SOURCES}
    src/main.c
    src/only_cmake.c
    ${GEN_SOURCES}
)
add_executable(game_unit_test
    tests/unit_test.c
)
`)
	manifest := Manifest{Name: "Game", Sources: []string{"src/main.c", "src/only_manifest.c"}}
	warnings := ManifestDriftWarnings(root, manifest)
	if len(warnings) != 2 {
		t.Fatalf("warnings: %v", warnings)
	}
	joined := strings.Join(warnings, "\n")
	if !strings.Contains(joined, "only_cmake.c") || !strings.Contains(joined, "only_manifest.c") {
		t.Fatalf("warnings: %v", warnings)
	}

	agreeing := Manifest{Name: "Game", Sources: []string{"src/main.c", "src/only_cmake.c"}}
	if warnings := ManifestDriftWarnings(root, agreeing); len(warnings) != 0 {
		t.Fatalf("expected no warnings: %v", warnings)
	}
	// Sources of other targets (unit tests) must not count as drift, and an
	// absent target means no basis for comparison.
	if warnings := ManifestDriftWarnings(root, Manifest{Name: "Missing", Sources: []string{"src/x.c"}}); warnings != nil {
		t.Fatalf("missing target: %v", warnings)
	}
}
