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

func TestRunnerSourcesFromIncludedManifest(t *testing.T) {
	runtimeDir := t.TempDir()
	writeTestFile(t, filepath.Join(runtimeDir, "runner.cmake"), `
set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})
include(${SNESRECOMP_RUNNER_ROOT}/sources.cmake)
set(SNESRECOMP_RUNNER_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
)
`)
	writeTestFile(t, filepath.Join(runtimeDir, "sources.cmake"), `
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/a.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/b.c
)
`)

	sources, includes, err := RunnerSources(runtimeDir)
	if err != nil {
		t.Fatal(err)
	}
	if len(sources) != 2 || !strings.HasSuffix(sources[1], filepath.FromSlash("src/snes/b.c")) {
		t.Fatalf("sources: %v", sources)
	}
	if len(includes) != 1 || !strings.HasSuffix(includes[0], "src") {
		t.Fatalf("includes: %v", includes)
	}
}

func TestRealNextRunnerSources(t *testing.T) {
	// internal/project -> snesrecomp-go/runtime-next.
	runtimeDir := filepath.Join("..", "..", "runtime-next")
	sources, includes, err := RunnerSources(runtimeDir)
	if err != nil {
		t.Fatalf("parse real runtime-next manifest: %v", err)
	}
	if len(sources) == 0 || len(includes) == 0 {
		t.Fatalf("real runtime-next manifest is incomplete: %v %v", sources, includes)
	}
	for _, source := range sources {
		if _, err := os.Stat(source); err != nil {
			t.Errorf("runtime-next lists %s, which does not exist: %v", source, err)
		}
	}
}

func TestRunnerSourcesCleansRelativePaths(t *testing.T) {
	toolchain := t.TempDir()
	nextDir := filepath.Join(toolchain, "runtime-next")
	writeTestFile(t, filepath.Join(nextDir, "runner.cmake"), `
set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/new.c
    ${SNESRECOMP_RUNNER_ROOT}/../shared/src/shared.c
)
set(SNESRECOMP_RUNNER_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/../shared/src
)
`)
	sources, includes, err := RunnerSources(nextDir)
	if err != nil {
		t.Fatal(err)
	}
	wantShared := filepath.Join(toolchain, "shared", "src", "shared.c")
	if sources[1] != wantShared {
		t.Fatalf("relative source = %s, want %s", sources[1], wantShared)
	}
	if strings.Contains(sources[1], "..") || strings.Contains(includes[1], "..") {
		t.Fatalf("relative paths were not cleaned: %v %v", sources, includes)
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

// snesbuild.ini is now the ONLY game source list: CMakeLists.txt reads it with
// file(STRINGS ... REGEX "^source =") instead of repeating it. These two tests pin
// that arrangement, and they exist because of how the duplication failed.
//
// src/atomic_replace.c was added to CMakeLists.txt and not to snesbuild.ini.
// Nothing caught it until a user's bundle rebuild reached the LINKER:
//
//	error: undefined symbol: _AtomicReplaceFile
//
// ManifestDriftWarnings would have named the file exactly, but neither caller can
// see it in practice: `doctor` is advisory, and the hermetic build's hard check
// reads CMakeLists.txt from its own --root, which in a shipped bundle is utils/
// and carries no CMakeLists.txt -- the read fails, the function returns nil, and
// the guard is a silent no-op precisely where a user's rebuild runs.
//
// Note that a drift ASSERTION here would now be vacuous: with no literal sources
// left in CMakeLists.txt, ManifestDriftWarnings has nothing to compare and returns
// nil, so such a test would pass no matter what. It is replaced by the two real
// invariants below rather than left to look like protection it no longer provides.

// Every source the manifest names must exist. This is the check that has teeth in
// a BUNDLE, where snesbuild.ini is the only list present and CMake never runs.
func TestRealManifestSourcesAllExist(t *testing.T) {
	// internal/project -> snesrecomp-go -> repo root.
	root := filepath.Join("..", "..", "..")
	manifest, err := LoadManifest(filepath.Join(root, ManifestFileName))
	if err != nil {
		t.Skipf("repository %s not readable from here: %v", ManifestFileName, err)
	}
	if len(manifest.Sources) == 0 {
		t.Fatalf("%s declares no sources", ManifestFileName)
	}
	for _, source := range manifest.Sources {
		if _, err := os.Stat(filepath.Join(root, source)); err != nil {
			t.Errorf("%s lists %s, which does not exist", ManifestFileName, source)
		}
	}
}

// CMakeLists.txt must NOT carry its own copy of the game source list. If a literal
// list is ever pasted back into the target, the two can silently diverge again --
// so the single-source-of-truth property is asserted, not just documented.
//
// Scoped to the add_executable(ActRaiserRecomp ...) block: the unit-test targets
// below it legitimately name their own sources.
func TestCMakeListsDoesNotDuplicateTheSourceList(t *testing.T) {
	root := filepath.Join("..", "..", "..")
	content, err := os.ReadFile(filepath.Join(root, "CMakeLists.txt"))
	if err != nil {
		t.Skipf("repository CMakeLists.txt not readable from here: %v", err)
	}
	start := strings.Index(string(content), "add_executable(ActRaiserRecomp")
	if start < 0 {
		t.Skip("game target not found")
	}
	block := string(content)[start:]
	if end := strings.Index(block, "\n)"); end >= 0 {
		block = block[:end]
	}
	for _, line := range strings.Split(block, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasSuffix(line, ".c") && !strings.ContainsAny(line, "${}() ") {
			t.Errorf("CMakeLists.txt hardcodes %s in the game target; add it to %s "+
				"instead -- CMake reads the list from there and a second copy is how "+
				"src/atomic_replace.c went missing from a bundle rebuild", line,
				ManifestFileName)
		}
	}
}
