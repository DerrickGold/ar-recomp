package project

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// HermeticOptions drives the CMake-free build path: every translation unit is
// compiled with the pinned Zig toolchain and linked into the game executable
// directly, so end users need neither CMake nor a system compiler. See
// docs/PROJECT_INTEGRATION.md for the snesbuild.ini contract.
type HermeticOptions struct {
	Paths
	ManifestPath  string // defaults to <root>/snesbuild.ini
	ZigPath       string // required: resolved zig executable
	Jobs          int
	Optimize      string // defaults to -O2
	SDLIncludeDir string // discovered when empty and the manifest wants SDL2
	SDLLibDir     string
	Verbose       bool
	Stdout        io.Writer
	Stderr        io.Writer
}

// HermeticBuild compiles the full project (runtime + game + generated
// sources) with `zig cc` and links the executable. Returns the binary path.
//
// Incrementality is deliberately simple and safe: an object is reused only if
// it is newer than its source, newer than every header in every include
// directory, and the compile flags are unchanged. Anything else recompiles.
func HermeticBuild(options HermeticOptions) (string, error) {
	paths, err := options.Paths.Resolve()
	if err != nil {
		return "", err
	}
	options.Paths = paths
	if options.Stdout == nil {
		options.Stdout = io.Discard
	}
	if options.Stderr == nil {
		options.Stderr = io.Discard
	}
	if options.Jobs <= 0 {
		options.Jobs = runtime.NumCPU()
	}
	if options.Optimize == "" {
		options.Optimize = "-O2"
	}
	if options.ZigPath == "" {
		return "", fmt.Errorf("hermetic build requires a Zig toolchain (see `snesbuild toolchain`)")
	}
	manifestPath := options.ManifestPath
	if manifestPath == "" {
		manifestPath = filepath.Join(paths.Root, ManifestFileName)
	}
	manifest, err := LoadManifest(manifestPath)
	if err != nil {
		return "", err
	}

	runtimeDir := filepath.Join(paths.ToolchainDir, "runtime")
	runnerSources, runnerIncludes, err := RunnerSources(runtimeDir)
	if err != nil {
		return "", err
	}
	generated, err := filepath.Glob(filepath.Join(paths.GeneratedDir, "*.c"))
	if err != nil {
		return "", err
	}
	if len(generated) == 0 {
		return "", fmt.Errorf("%s is empty; regenerate before building", paths.GeneratedDir)
	}
	sort.Strings(generated)

	var sources []string
	sources = append(sources, runnerSources...)
	for _, source := range manifest.Sources {
		sources = append(sources, resolveUnder(paths.Root, source))
	}
	sources = append(sources, generated...)
	for _, source := range sources {
		if _, statErr := os.Stat(source); statErr != nil {
			return "", fmt.Errorf("missing source %s (listed in %s or runner.cmake)", source, manifestPath)
		}
	}

	includeDirs := append([]string(nil), runnerIncludes...)
	for _, include := range manifest.Includes {
		includeDirs = append(includeDirs, resolveUnder(paths.Root, include))
	}
	sdlBundled := false
	if manifest.UseSDL2 {
		if options.SDLIncludeDir == "" || options.SDLLibDir == "" {
			includeDir, libDir, bundled, sdlErr := discoverSDL2()
			if sdlErr != nil {
				return "", sdlErr
			}
			sdlBundled = bundled
			if options.SDLIncludeDir == "" {
				options.SDLIncludeDir = includeDir
			}
			if options.SDLLibDir == "" {
				options.SDLLibDir = libDir
			}
		}
		includeDirs = append(includeDirs, options.SDLIncludeDir)
		// The game includes <SDL.h> (needs the dir that holds SDL.h) while
		// the SDL headers may self-reference <SDL2/...> (needs its parent).
		// Add both so every SDL source layout resolves.
		if strings.EqualFold(filepath.Base(options.SDLIncludeDir), "SDL2") {
			includeDirs = append(includeDirs, filepath.Dir(options.SDLIncludeDir))
		}
		fmt.Fprintf(options.Stdout, "hermetic: SDL2 headers %s, libraries %s%s\n",
			options.SDLIncludeDir, options.SDLLibDir, map[bool]string{true: " (bundled)", false: ""}[sdlBundled])
	}

	compileArgs := []string{"cc", "-std=" + manifest.Std, options.Optimize, "-g",
		"-w", "-Wno-implicit-function-declaration"}
	for _, define := range manifest.Defines {
		compileArgs = append(compileArgs, "-D"+define)
	}
	for _, include := range includeDirs {
		compileArgs = append(compileArgs, "-I"+include)
	}

	objectDir := filepath.Join(paths.BuildDir, "hermetic", "obj")
	if err := os.MkdirAll(objectDir, 0o755); err != nil {
		return "", err
	}

	// Invalidate every object when the flag set (or compiler) changes.
	flagsDigest := sha256.Sum256([]byte(options.ZigPath + "\x00" + strings.Join(compileArgs, "\x00")))
	flagsHash := hex.EncodeToString(flagsDigest[:])
	flagsPath := filepath.Join(paths.BuildDir, "hermetic", "flags.sha256")
	previousFlags, _ := os.ReadFile(flagsPath)
	flagsChanged := strings.TrimSpace(string(previousFlags)) != flagsHash

	newestHeader := newestHeaderTime(includeDirs)

	type job struct{ source, object string }
	var jobs []job
	cached := 0
	for _, source := range sources {
		object := filepath.Join(objectDir, objectName(paths.Root, source))
		if !flagsChanged && objectFresh(source, object, newestHeader) {
			cached++
			continue
		}
		jobs = append(jobs, job{source, object})
	}
	fmt.Fprintf(options.Stdout, "hermetic: %d translation units (%d cached, %d to compile, %d jobs)\n",
		len(sources), cached, len(jobs), options.Jobs)

	started := time.Now()
	var failed atomic.Bool
	var firstError error
	var errorOnce sync.Once
	semaphore := make(chan struct{}, options.Jobs)
	var waitGroup sync.WaitGroup
	for _, item := range jobs {
		if failed.Load() {
			break
		}
		waitGroup.Add(1)
		semaphore <- struct{}{}
		go func(item job) {
			defer waitGroup.Done()
			defer func() { <-semaphore }()
			if failed.Load() {
				return
			}
			if options.Verbose {
				fmt.Fprintf(options.Stdout, "  cc %s\n", item.source)
			}
			command := exec.Command(options.ZigPath, append(append([]string(nil), compileArgs...), "-c", item.source, "-o", item.object)...)
			output, err := command.CombinedOutput()
			if err != nil {
				failed.Store(true)
				errorOnce.Do(func() {
					firstError = fmt.Errorf("compile %s: %w\n%s", item.source, err, strings.TrimSpace(string(output)))
				})
			}
		}(item)
	}
	waitGroup.Wait()
	if firstError != nil {
		return "", firstError
	}
	if err := os.WriteFile(flagsPath, []byte(flagsHash+"\n"), 0o644); err != nil {
		return "", err
	}
	fmt.Fprintf(options.Stdout, "hermetic: compile done in %.1fs; linking\n", time.Since(started).Seconds())

	binary := filepath.Join(paths.BuildDir, "hermetic", manifest.Name)
	if runtime.GOOS == "windows" {
		binary += ".exe"
	}
	linkArgs := []string{"cc", "-o", binary}
	for _, source := range sources {
		linkArgs = append(linkArgs, filepath.Join(objectDir, objectName(paths.Root, source)))
	}
	if manifest.UseSDL2 {
		linkArgs = append(linkArgs, "-L"+options.SDLLibDir, "-lSDL2")
		// Look for the SDL runtime beside the game binary first so a copied
		// (bundled) library wins over system search paths.
		switch runtime.GOOS {
		case "darwin":
			linkArgs = append(linkArgs, "-Wl,-rpath,@executable_path")
		case "linux":
			linkArgs = append(linkArgs, "-Wl,-rpath,$ORIGIN")
		}
	}
	linkArgs = append(linkArgs, manifest.Link...)
	command := exec.Command(options.ZigPath, linkArgs...)
	output, err := command.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("link %s: %w\n%s", binary, err, strings.TrimSpace(string(output)))
	}
	if sdlBundled {
		copied, copyErr := copySDLRuntime(options.SDLLibDir, filepath.Dir(binary))
		if copyErr != nil {
			return "", copyErr
		}
		for _, name := range copied {
			fmt.Fprintf(options.Stdout, "hermetic: bundled SDL runtime %s copied beside the binary\n", name)
		}
	}
	fmt.Fprintf(options.Stdout, "hermetic: built %s\n", binary)
	return binary, nil
}

// copySDLRuntime places the bundled SDL shared libraries next to the built
// game binary so it runs on machines with no system SDL at all (the rpath /
// DLL search path already prefers the binary's own directory).
func copySDLRuntime(libDir, binaryDir string) ([]string, error) {
	var patterns []string
	switch runtime.GOOS {
	case "darwin":
		patterns = []string{"libSDL2*.dylib"}
	case "windows":
		patterns = []string{"SDL2*.dll"}
	default:
		patterns = []string{"libSDL2*.so*"}
	}
	var copied []string
	for _, pattern := range patterns {
		matches, _ := filepath.Glob(filepath.Join(libDir, pattern))
		for _, source := range matches {
			data, err := os.ReadFile(source)
			if err != nil {
				return nil, err
			}
			target := filepath.Join(binaryDir, filepath.Base(source))
			if err := os.WriteFile(target, data, 0o755); err != nil {
				return nil, err
			}
			copied = append(copied, filepath.Base(source))
		}
	}
	return copied, nil
}

// objectName maps a source path to a unique flat object file name. Sources
// under the project root use their root-relative path; others (the engine
// runtime living elsewhere in odd layouts) fall back to a hashed suffix.
func objectName(root, source string) string {
	relative, err := filepath.Rel(root, source)
	if err != nil || strings.HasPrefix(relative, "..") {
		digest := sha256.Sum256([]byte(source))
		return "ext_" + hex.EncodeToString(digest[:8]) + "_" + filepath.Base(source) + ".o"
	}
	mangled := strings.NewReplacer("/", "_", "\\", "_").Replace(relative)
	return mangled + ".o"
}

func objectFresh(source, object string, newestHeader time.Time) bool {
	objectInfo, err := os.Stat(object)
	if err != nil {
		return false
	}
	sourceInfo, err := os.Stat(source)
	if err != nil {
		return false
	}
	modified := objectInfo.ModTime()
	return modified.After(sourceInfo.ModTime()) && modified.After(newestHeader)
}

// newestHeaderTime scans each include directory (non-recursively — every
// header-bearing directory is listed explicitly) for the newest *.h mtime.
func newestHeaderTime(includeDirs []string) time.Time {
	var newest time.Time
	for _, directory := range includeDirs {
		entries, err := os.ReadDir(directory)
		if err != nil {
			continue
		}
		for _, entry := range entries {
			if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".h") {
				continue
			}
			info, err := entry.Info()
			if err != nil {
				continue
			}
			if info.ModTime().After(newest) {
				newest = info.ModTime()
			}
		}
	}
	return newest
}

// discoverSDL2 finds SDL2 development files: a copy bundled beside the
// running executable first (distribution bundle layout: <exe dir>/sdl2/
// include + lib), then pkg-config, then well-known platform prefixes.
// Explicit --sdl-include/--sdl-lib flags always win; this is only the
// fallback so both bundles and developer machines work out of the box.
func discoverSDL2() (includeDir, libDir string, bundled bool, err error) {
	if executable, exeErr := os.Executable(); exeErr == nil {
		base := filepath.Join(filepath.Dir(executable), "sdl2")
		include := filepath.Join(base, "include")
		lib := filepath.Join(base, "lib")
		// Bundled headers live under include/SDL2/; point at that leaf so
		// the game's <SDL.h> resolves (the parent is added by the caller).
		if directoryExists(filepath.Join(include, "SDL2")) {
			include = filepath.Join(include, "SDL2")
		}
		if directoryExists(include) && directoryExists(lib) {
			return include, lib, true, nil
		}
	}
	if pkgConfig, lookErr := exec.LookPath("pkg-config"); lookErr == nil {
		includeOut, includeErr := exec.Command(pkgConfig, "--cflags-only-I", "sdl2").Output()
		libOut, libErr := exec.Command(pkgConfig, "--libs-only-L", "sdl2").Output()
		include := sdlIncludeFlagValue(string(includeOut))
		lib := firstFlagValue(string(libOut), "-L")
		if includeErr == nil && libErr == nil && include != "" && lib != "" {
			return include, lib, false, nil
		}
	}
	prefixes := []string{
		"/opt/homebrew/opt/sdl2", "/opt/homebrew/opt/sdl2-compat",
		"/usr/local/opt/sdl2", "/usr/local/opt/sdl2-compat",
		"/usr", "/usr/local",
	}
	for _, prefix := range prefixes {
		include := filepath.Join(prefix, "include", "SDL2")
		lib := filepath.Join(prefix, "lib")
		if directoryExists(include) && directoryExists(lib) {
			return include, lib, false, nil
		}
	}
	return "", "", false, fmt.Errorf("SDL2 development files not found; pass --sdl-include and --sdl-lib")
}

// sdlIncludeFlagValue picks the include directory that actually contains
// SDL.h: project sources use `#include <SDL.h>`, so the .../SDL2 entry wins
// over a bare parent include root when pkg-config reports both.
func sdlIncludeFlagValue(output string) string {
	fallback := ""
	for _, field := range strings.Fields(output) {
		value := strings.TrimPrefix(field, "-I")
		if value == field || value == "" {
			continue
		}
		if filepath.Base(value) == "SDL2" {
			return value
		}
		if fallback == "" {
			fallback = value
		}
	}
	return fallback
}

func firstFlagValue(output, prefix string) string {
	for _, field := range strings.Fields(output) {
		if strings.HasPrefix(field, prefix) && len(field) > len(prefix) {
			return field[len(prefix):]
		}
	}
	return ""
}

func directoryExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}
