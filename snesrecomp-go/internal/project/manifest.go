package project

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Manifest describes the game-specific half of a hermetic (CMake-free) build:
// everything the engine cannot know about a particular game project. The
// runtime's own source list stays in runtime/runner.cmake, which the engine
// module owns and parses directly (see RunnerSources).
//
// File format (snesbuild.ini at the project root): `key = value` lines with
// optional [section] headers and # comments. The list-valued keys (source,
// include, define, link) repeat, one entry per line, and keep file order.
// Paths are relative to the project root.
type Manifest struct {
	Name     string   // executable name
	Std      string   // C standard, e.g. "c11"
	Sources  []string // game translation units (generated sources are globbed separately)
	Includes []string // game include directories
	Defines  []string // NAME or NAME=VALUE
	Link     []string // extra linker arguments, e.g. -lm
	UseSDL2  bool     // discover SDL2 headers/libs and link -lSDL2
}

// ManifestFileName is the expected file name at the project root.
const ManifestFileName = "snesbuild.ini"

func LoadManifest(path string) (Manifest, error) {
	file, err := os.Open(path)
	if err != nil {
		return Manifest{}, err
	}
	defer file.Close()
	manifest := Manifest{Std: "c11"}
	scanner := bufio.NewScanner(file)
	lineNumber := 0
	for scanner.Scan() {
		lineNumber++
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			continue // sections are organizational only
		}
		key, value, found := strings.Cut(line, "=")
		if !found {
			return Manifest{}, fmt.Errorf("%s:%d: expected key = value, got %q", path, lineNumber, line)
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)
		switch key {
		case "name":
			manifest.Name = value
		case "std":
			manifest.Std = value
		case "source":
			manifest.Sources = append(manifest.Sources, value)
		case "include":
			manifest.Includes = append(manifest.Includes, value)
		case "define":
			manifest.Defines = append(manifest.Defines, value)
		case "link":
			manifest.Link = append(manifest.Link, value)
		case "sdl2":
			manifest.UseSDL2 = value == "true" || value == "1" || value == "on"
		default:
			return Manifest{}, fmt.Errorf("%s:%d: unknown key %q", path, lineNumber, key)
		}
	}
	if err := scanner.Err(); err != nil {
		return Manifest{}, err
	}
	if manifest.Name == "" {
		return Manifest{}, fmt.Errorf("%s: missing required key `name`", path)
	}
	if len(manifest.Sources) == 0 {
		return Manifest{}, fmt.Errorf("%s: no `source` entries", path)
	}
	return manifest, nil
}

// RunnerSources parses runtime/runner.cmake for the engine's shared source
// and include lists so the hermetic build and the CMake build cannot drift.
// Only the unconditional first set(...) block of each variable is read; the
// SNESRECOMP_ENABLE_TRACE-conditional append is a developer-only source that
// hermetic release builds never compile.
func RunnerSources(runtimeDir string) (sources, includeDirs []string, err error) {
	path := filepath.Join(runtimeDir, "runner.cmake")
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, err
	}
	sources, err = cmakeSetEntries(string(content), "SNESRECOMP_RUNNER_SOURCES", runtimeDir)
	if err != nil {
		return nil, nil, fmt.Errorf("%s: %w", path, err)
	}
	includeDirs, err = cmakeSetEntries(string(content), "SNESRECOMP_RUNNER_INCLUDE_DIRS", runtimeDir)
	if err != nil {
		return nil, nil, fmt.Errorf("%s: %w", path, err)
	}
	return sources, includeDirs, nil
}

// ManifestDriftWarnings cross-checks the manifest's game source list against
// the same-named target's add_executable block in the project's
// CMakeLists.txt (the developer build). It is a cheap text scan, not a CMake
// evaluation: within that block, any line that is a bare relative *.c path is
// treated as a source entry. Returns human-readable warnings; empty when the
// two lists agree or no matching CMake target exists.
func ManifestDriftWarnings(root string, manifest Manifest) []string {
	content, err := os.ReadFile(filepath.Join(root, "CMakeLists.txt"))
	if err != nil {
		return nil
	}
	start := strings.Index(string(content), "add_executable("+manifest.Name)
	if start < 0 {
		return nil
	}
	block := string(content)[start:]
	if end := strings.Index(block, ")"); end >= 0 {
		block = block[:end]
	}
	cmakeSources := make(map[string]bool)
	for _, line := range strings.Split(block, "\n") {
		line = strings.TrimSpace(line)
		if strings.HasSuffix(line, ".c") && !strings.ContainsAny(line, "${}() ") {
			cmakeSources[filepath.ToSlash(line)] = true
		}
	}
	if len(cmakeSources) == 0 {
		return nil
	}
	manifestSources := make(map[string]bool)
	for _, source := range manifest.Sources {
		manifestSources[filepath.ToSlash(source)] = true
	}
	var warnings []string
	for source := range manifestSources {
		if !cmakeSources[source] {
			warnings = append(warnings, fmt.Sprintf("%s is in snesbuild.ini but not CMakeLists.txt", source))
		}
	}
	for source := range cmakeSources {
		if !manifestSources[source] {
			warnings = append(warnings, fmt.Sprintf("%s is in CMakeLists.txt but not snesbuild.ini", source))
		}
	}
	sort.Strings(warnings)
	return warnings
}

// cmakeSetEntries extracts the entries of the first `set(<variable> ...)`
// block, resolving ${SNESRECOMP_RUNNER_ROOT} to runtimeDir.
func cmakeSetEntries(content, variable, runtimeDir string) ([]string, error) {
	marker := "set(" + variable
	start := strings.Index(content, marker)
	if start < 0 {
		return nil, fmt.Errorf("no set(%s ...) block found", variable)
	}
	block := content[start+len(marker):]
	end := strings.Index(block, ")")
	if end < 0 {
		return nil, fmt.Errorf("unterminated set(%s ...) block", variable)
	}
	var entries []string
	for _, line := range strings.Split(block[:end], "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		line = strings.ReplaceAll(line, "${SNESRECOMP_RUNNER_ROOT}", runtimeDir)
		entries = append(entries, filepath.FromSlash(line))
	}
	if len(entries) == 0 {
		return nil, fmt.Errorf("set(%s ...) block is empty", variable)
	}
	return entries, nil
}
