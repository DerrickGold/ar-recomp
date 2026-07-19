package project

import (
	"os"
	"path/filepath"
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
sdl2 = true
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
	if manifest.Name != "MyGame" || !manifest.UseSDL2 || manifest.Std != "c11" {
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

func TestRunnerSources(t *testing.T) {
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

set(SNESRECOMP_RUNNER_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/src/snes
)
`)
	sources, includes, err := RunnerSources(runtimeDir)
	if err != nil {
		t.Fatal(err)
	}
	if len(sources) != 2 || !strings.HasSuffix(sources[1], filepath.FromSlash("src/snes/b.c")) {
		t.Fatalf("sources: %v", sources)
	}
	for _, source := range sources {
		if strings.Contains(source, "debug_only") {
			t.Fatalf("conditional trace source must be excluded: %v", sources)
		}
	}
	if len(includes) != 2 || !strings.HasSuffix(includes[0], "src") {
		t.Fatalf("includes: %v", includes)
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
