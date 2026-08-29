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

	"github.com/DerrickGold/snesrecomp-go/internal/fsutil"
)

// HermeticOptions drives the CMake-free build path. A distribution links its
// target-matched vended runtime archive; a source checkout builds that archive
// first as a fallback. The pinned Zig toolchain then compiles and links the
// game. End users need neither CMake nor a system compiler. See
// docs/PROJECT_INTEGRATION.md for the snesbuild.ini contract.
type HermeticOptions struct {
	Paths
	ManifestPath  string // defaults to <root>/snesbuild.ini
	ZigPath       string // required: resolved zig executable
	Jobs          int
	Optimize      string // defaults to -O2
	SDLIncludeDir string // discovered when empty and the manifest wants SDL3
	SDLLibDir     string
	// Target is a Zig target triple (e.g. "x86_64-windows-gnu"). Empty builds
	// for the host. A cross target exists to answer "would this even build
	// over there" without owning the hardware: `zig cc` carries the libc
	// headers for every target it supports, so the compile and the link are
	// the real ones, and only the run is missing. It never auto-discovers the
	// host SDL3 (linking macOS SDL into a Windows binary would fail in a way
	// that teaches nothing) -- see CrossSDL3Dir.
	Target  string
	Verbose bool
	Stdout  io.Writer
	Stderr  io.Writer
}

// toolLog forwards a subprocess's own output to the build log. Zig's
// diagnostics are the ONLY thing that can explain a failed compile or link, and
// they used to reach nowhere a reader could get at: captured and dropped on
// success, and on failure folded into an error string whose caller has a
// one-line status field to show twenty lines of linker output in. They belong
// in the log the reader is already watching.
//
// Everything is compiled with -w, so a healthy unit is silent and this costs
// nothing on a normal build -- but the linker is NOT silenced, and its "note:
// referenced by" lines are precisely what an undefined-symbol failure needs.
//
// Output is captured per command and emitted as a whole rather than streamed:
// up to Jobs compiles run at once, and interleaving their raw streams would
// shred every multi-line diagnostic into unreadable fragments. The mutex keeps
// one command's block contiguous.
type toolLog struct {
	mu     sync.Mutex
	writer io.Writer
}

func (log *toolLog) printf(format string, args ...any) {
	log.mu.Lock()
	defer log.mu.Unlock()
	fmt.Fprintf(log.writer, format, args...)
}

// block writes one command's output under a heading naming what produced it,
// indented so it reads as subordinate detail and, just as importantly, so no
// diagnostic line can imitate one of the "hermetic:"/"cc " status lines the
// progress model reads out of this same log.
func (log *toolLog) block(heading string, output []byte) {
	body := strings.TrimRight(string(output), "\n\r \t")
	if body == "" {
		return
	}
	var builder strings.Builder
	fmt.Fprintf(&builder, "  -- %s --\n", heading)
	for _, line := range strings.Split(body, "\n") {
		builder.WriteString("  | " + strings.TrimRight(line, "\r") + "\n")
	}
	log.mu.Lock()
	defer log.mu.Unlock()
	_, _ = io.WriteString(log.writer, builder.String())
}

// TargetOS maps a Zig target triple to the GOOS-style name the rest of this
// file switches on, so link flags follow the target rather than the host.
// An empty target means the host.
func TargetOS(target string) string {
	if target == "" {
		return runtime.GOOS
	}
	fields := strings.Split(target, "-")
	if len(fields) < 2 {
		return runtime.GOOS
	}
	switch fields[1] {
	case "macos":
		return "darwin"
	default:
		return fields[1]
	}
}

// CrossSDL3Dir is where `snesbuild sdl stage --target <t>` puts the staged
// redistributable, and where a cross build looks for SDL3 when no explicit
// --sdl-include/--sdl-lib was given. Laid out as include/SDL3 + lib to match
// what the distribution bundle ships, so the cross link exercises the same
// headers and import library a real user's build would.
func CrossSDL3Dir(buildDir, target string) string {
	return filepath.Join(buildDir, "hermetic", target, "sdl3")
}

// HermeticBuild selects or produces a standalone runtime archive, then compiles
// the game and generated sources with `zig cc` and links them against it.
// Returns the binary path.
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
	// Source lists that disagree produce either a link failure thousands of
	// object files later or a silently different binary from the developer
	// build. Both are far more expensive to read than this. `doctor` reports
	// the same drift as a warning; here it stops the build.
	if drift := ManifestDriftWarnings(paths.Root, manifest); len(drift) > 0 {
		return "", fmt.Errorf("%s and CMakeLists.txt disagree on the game source list:\n  %s\n"+
			"add the missing entries to whichever file is behind, then build again",
			ManifestFileName, strings.Join(drift, "\n  "))
	}

	runnerDirectory := RunnerDirectory(paths.ToolchainDir)
	runner, err := selectRuntimeInputs(runnerDirectory, options.Target)
	if err != nil {
		return "", err
	}
	if runner.Archive != "" {
		fmt.Fprintf(options.Stdout, "hermetic: runner %s (vended archive %s)\n",
			RunnerName, runner.Archive)
	} else {
		fmt.Fprintf(options.Stdout, "hermetic: runner %s (source fallback)\n",
			RunnerName)
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
	sources = append(sources, runner.SourceManifest.Sources...)
	runnerSourceCount := len(sources)
	for _, source := range manifest.Sources {
		sources = append(sources, resolveUnder(paths.Root, source))
	}
	sources = append(sources, generated...)
	for _, source := range sources {
		if _, statErr := os.Stat(source); statErr != nil {
			return "", fmt.Errorf("missing source %s (listed in %s or the runner source manifest)",
				source, manifestPath)
		}
	}

	includeDirs := append([]string(nil), runner.PublicIncludes...)
	for _, include := range manifest.Includes {
		includeDirs = append(includeDirs, resolveUnder(paths.Root, include))
	}
	targetOS := TargetOS(options.Target)
	sdlBundled := false
	if manifest.UseSDL3 {
		if options.SDLIncludeDir == "" || options.SDLLibDir == "" {
			includeDir, libDir, bundled, sdlErr := resolveSDL3(options)
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
		// The game includes <SDL3/SDL.h>, so the include dir must be the
		// parent that holds the SDL3/ folder. If a caller instead points at
		// the SDL3/ leaf itself, add its parent so <SDL3/...> still resolves.
		if strings.EqualFold(filepath.Base(options.SDLIncludeDir), "SDL3") {
			includeDirs = append(includeDirs, filepath.Dir(options.SDLIncludeDir))
		}
		fmt.Fprintf(options.Stdout, "hermetic: SDL3 headers %s, libraries %s%s\n",
			options.SDLIncludeDir, options.SDLLibDir, map[bool]string{true: " (bundled)", false: ""}[sdlBundled])
	}

	compileArgs := []string{"cc"}
	if options.Target != "" {
		compileArgs = append(compileArgs, "-target", options.Target)
	}
	compileArgs = append(compileArgs, "-std="+manifest.Std, options.Optimize, "-g",
		"-w", "-Wno-implicit-function-declaration")
	for _, define := range manifest.Defines {
		compileArgs = append(compileArgs, "-D"+define)
	}
	for _, include := range includeDirs {
		compileArgs = append(compileArgs, "-I"+include)
	}
	var runnerCompileArgs []string
	if runnerSourceCount > 0 {
		runnerCompileArgs = runtimeCompileArgs(runnerDirectory, options.ZigPath,
			options.Target, options.Optimize, true, runner.SourceManifest)
	}

	// Each target gets its own output tree. Sharing one would be worse than
	// slow: the flags hash below would invalidate everything on every switch,
	// so a cross-check would silently throw away the developer's native
	// objects and vice versa. Separate trees make a cross build cheap enough
	// to run as a gate.
	outputDir := HermeticOutputDir(paths.BuildDir, options.Target)
	objectDir := filepath.Join(outputDir, "obj")
	if err := os.MkdirAll(objectDir, 0o755); err != nil {
		return "", err
	}

	// Runtime-private include roots and flags invalidate only runtime objects.
	// The game side has its own cache key and never receives private includes.
	gameFlagsDigest := sha256.Sum256([]byte(options.ZigPath + "\x00" +
		strings.Join(compileArgs, "\x00")))
	gameFlagsHash := hex.EncodeToString(gameFlagsDigest[:])
	gameFlagsPath := filepath.Join(outputDir, "game-flags.sha256")
	runnerFlagsPath := filepath.Join(outputDir, "runner-flags.sha256")
	previousGameFlags, _ := os.ReadFile(gameFlagsPath)
	gameFlagsChanged := strings.TrimSpace(string(previousGameFlags)) != gameFlagsHash
	runnerFlagsHash := ""
	runnerFlagsChanged := false
	if runnerSourceCount > 0 {
		runnerFlagsDigest := sha256.Sum256([]byte(options.ZigPath + "\x00" +
			strings.Join(runnerCompileArgs, "\x00")))
		runnerFlagsHash = hex.EncodeToString(runnerFlagsDigest[:])
		previousRunnerFlags, _ := os.ReadFile(runnerFlagsPath)
		runnerFlagsChanged = strings.TrimSpace(string(previousRunnerFlags)) != runnerFlagsHash
	}

	newestGameHeader := newestHeaderTime(includeDirs, paths.BuildDir)
	newestRunnerHeader := time.Time{}
	if runnerSourceCount > 0 {
		runnerHeaderDirs := append([]string(nil), runner.SourceManifest.PublicIncludes...)
		runnerHeaderDirs = append(runnerHeaderDirs,
			runner.SourceManifest.PrivateIncludes...)
		newestRunnerHeader = newestHeaderTime(runnerHeaderDirs, paths.BuildDir)
	}

	type job struct {
		source, object string
		runner         bool
	}
	var jobs []job
	cached := 0
	for sourceIndex, source := range sources {
		object := filepath.Join(objectDir, objectName(paths.Root, source))
		isRunner := sourceIndex < runnerSourceCount
		flagsChanged := gameFlagsChanged
		newestHeader := newestGameHeader
		if isRunner {
			flagsChanged = runnerFlagsChanged
			newestHeader = newestRunnerHeader
		}
		if !flagsChanged && objectFresh(source, object, newestHeader) {
			cached++
			continue
		}
		jobs = append(jobs, job{
			source: source, object: object,
			runner: isRunner,
		})
	}
	fmt.Fprintf(options.Stdout, "hermetic: %d translation units (%d cached, %d to compile, %d jobs)\n",
		len(sources), cached, len(jobs), options.Jobs)

	started := time.Now()
	tools := &toolLog{writer: options.Stdout}
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
				tools.printf("  cc %s\n", item.source)
			}
			args := compileArgs
			if item.runner {
				args = runnerCompileArgs
			}
			command := exec.Command(options.ZigPath, append(append([]string(nil), args...), "-c", item.source, "-o", item.object)...)
			output, err := command.CombinedOutput()
			// Emitted whether or not the unit failed -- -w keeps a healthy
			// compile silent, so anything a tool does say here is worth reading.
			tools.block("cc "+item.source, output)
			if err != nil {
				failed.Store(true)
				errorOnce.Do(func() {
					firstError = fmt.Errorf("compile %s: %w (its output is in the build log above)",
						item.source, err)
				})
			}
		}(item)
	}
	waitGroup.Wait()
	if firstError != nil {
		return "", firstError
	}
	if err := os.WriteFile(gameFlagsPath, []byte(gameFlagsHash+"\n"), 0o644); err != nil {
		return "", err
	}
	if runnerSourceCount > 0 {
		if err := os.WriteFile(runnerFlagsPath,
			[]byte(runnerFlagsHash+"\n"), 0o644); err != nil {
			return "", err
		}
		fmt.Fprintf(options.Stdout,
			"hermetic: compile done in %.1fs; archiving runner\n",
			time.Since(started).Seconds())
	} else {
		fmt.Fprintf(options.Stdout,
			"hermetic: compile done in %.1fs; linking vended runner\n",
			time.Since(started).Seconds())
	}

	binary := filepath.Join(outputDir, manifest.Name)
	if targetOS == "windows" {
		binary += ".exe"
	}
	objects := make([]string, 0, len(sources))
	for _, source := range sources {
		objects = append(objects, filepath.Join(objectDir, objectName(paths.Root, source)))
	}
	runtimeArchive := runner.Archive
	if runtimeArchive == "" {
		runtimeArchive = filepath.Join(outputDir, runtimeArchiveName(targetOS))
		if err := writeObjectArchive(options.ZigPath, targetOS, runtimeArchive,
			objects[:runnerSourceCount]); err != nil {
			return "", err
		}
		fmt.Fprintf(options.Stdout, "hermetic: built runner archive %s; linking game\n",
			runtimeArchive)
	}
	gameArchive, cleanupArchive, err := createObjectArchive(
		options.ZigPath, targetOS, outputDir, objects[runnerSourceCount:])
	if err != nil {
		return "", err
	}
	defer cleanupArchive()

	linkArgs := []string{"cc"}
	if options.Target != "" {
		linkArgs = append(linkArgs, "-target", options.Target)
	}
	linkArgs = append(linkArgs, "-o", binary)
	linkArgs = append(linkArgs, forceLoadArchiveArgs(targetOS, gameArchive)...)
	linkArgs = append(linkArgs, runtimeArchive)
	if manifest.UseSDL3 {
		linkArgs = append(linkArgs, "-L"+options.SDLLibDir, "-lSDL3")
		// Look for the SDL runtime beside the game binary first so a copied
		// (bundled) library wins over system search paths.
		switch targetOS {
		case "darwin":
			linkArgs = append(linkArgs, "-Wl,-rpath,@executable_path")
		case "linux":
			linkArgs = append(linkArgs, "-Wl,-rpath,$ORIGIN")
		case "windows":
			// No rpath equivalent is needed -- Windows searches the
			// executable's own directory first -- but the ROM picker in
			// launcher.c calls GetOpenFileNameA, and the common dialog
			// library is not one lld links by default. Without this the
			// Windows link fails on an undefined symbol after every
			// translation unit has already compiled.
			linkArgs = append(linkArgs, "-lcomdlg32")
		}
	}
	linkArgs = append(linkArgs, manifest.Link...)
	command := exec.Command(options.ZigPath, linkArgs...)
	output, err := command.CombinedOutput()
	tools.block("link "+filepath.Base(binary), output)
	if err != nil {
		return "", fmt.Errorf("link %s: %w (its output is in the build log above)", binary, err)
	}
	if sdlBundled {
		copied, copyErr := copySDLRuntime(targetOS, options.SDLLibDir, filepath.Dir(binary))
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
func copySDLRuntime(targetOS, libDir, binaryDir string) ([]string, error) {
	var patterns []string
	switch targetOS {
	case "darwin":
		patterns = []string{"libSDL3*.dylib"}
	case "windows":
		patterns = []string{"SDL3*.dll"}
	default:
		patterns = []string{"libSDL3*.so*"}
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
	// A compiler can create/truncate its output before failing (for example
	// when its cache directory is unavailable). Never promote that placeholder
	// into the incremental cache merely because its timestamp is newest.
	if err != nil || objectInfo.Size() == 0 {
		return false
	}
	sourceInfo, err := os.Stat(source)
	if err != nil {
		return false
	}
	modified := objectInfo.ModTime()
	return modified.After(sourceInfo.ModTime()) && modified.After(newestHeader)
}

// newestHeaderTime scans each include directory recursively for the newest
// *.h mtime. Nested layouts like <include>/SDL3/SDL_render.h must count, so a
// staleness check that only listed the top level would miss a header updated
// one directory down. An unreadable subtree is skipped rather than fatal.
//
// skipDir prunes the build directory. The manifest lists `include = .`, so
// without this the walk descends into build/ and finds the SDL3 headers a
// cross target stages there -- freshly copied, hence newer than every object,
// so one `sdl stage` would make the NATIVE build recompile all 180 units
// forever after. Pruning is safe precisely because a cross build passes its
// staged include directory as its own entry in includeDirs: that walk starts
// below the build directory and is never pruned.
func newestHeaderTime(includeDirs []string, skipDir string) time.Time {
	var newest time.Time
	for _, directory := range includeDirs {
		_ = filepath.WalkDir(directory, func(path string, entry os.DirEntry, err error) error {
			if err != nil {
				if entry != nil && entry.IsDir() {
					return filepath.SkipDir
				}
				return nil
			}
			if entry.IsDir() && skipDir != "" && path == skipDir {
				return filepath.SkipDir
			}
			if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".h") {
				return nil
			}
			info, infoErr := entry.Info()
			if infoErr != nil {
				return nil
			}
			if info.ModTime().After(newest) {
				newest = info.ModTime()
			}
			return nil
		})
	}
	return newest
}

// sdlCandidate is one (include-parent, lib-dir) pair probed by discoverSDL3.
type sdlCandidate struct {
	include string
	lib     string
}

// sdlLibDirHasLib reports whether dir actually contains an SDL3 shared library
// (libSDL3*.dylib on macOS, libSDL3*.so* on Linux). A directory merely
// existing is not enough — /usr/lib exists everywhere but only the multiarch
// subdir holds the .so on Debian/Ubuntu.
func sdlLibDirHasLib(dir string) bool {
	for _, pattern := range []string{"libSDL3*.dylib", "libSDL3*.so*"} {
		matches, err := filepath.Glob(filepath.Join(dir, pattern))
		if err == nil && len(matches) > 0 {
			return true
		}
	}
	return false
}

// resolveSDL3 picks the SDL3 development files for the build's target. For a
// host build that is discoverSDL3's usual search. For a cross build the host's
// SDL3 is the one answer that is always wrong, so only the staged
// redistributable under CrossSDL3Dir is accepted and the error names the
// command that puts it there rather than letting the link fail with a wall of
// unresolved symbols.
func resolveSDL3(options HermeticOptions) (includeDir, libDir string, bundled bool, err error) {
	if options.Target == "" {
		return discoverSDL3()
	}
	base := CrossSDL3Dir(options.Paths.BuildDir, options.Target)
	include, lib := filepath.Join(base, "include"), filepath.Join(base, "lib")
	if fsutil.DirectoryExists(filepath.Join(include, "SDL3")) &&
		fsutil.DirectoryExists(lib) {
		return include, lib, true, nil
	}
	return "", "", false, fmt.Errorf(
		"no staged SDL3 for target %s at %s; run `snesbuild sdl stage --target %s` "+
			"or pass --sdl-include/--sdl-lib", options.Target, base, options.Target)
}

// discoverSDL3 finds SDL3 development files: a copy bundled beside the
// running executable first (distribution bundle layout: <exe dir>/sdl3/
// include + lib), then pkg-config, then well-known platform prefixes.
// The include directory returned is the PARENT that contains the SDL3/
// header folder, matching the game's `#include <SDL3/SDL.h>` convention
// (sdl3.pc's Cflags reports exactly this parent). Explicit
// --sdl-include/--sdl-lib flags always win; this is only the fallback so
// both bundles and developer machines work out of the box.
func discoverSDL3() (includeDir, libDir string, bundled bool, err error) {
	if executable, exeErr := os.Executable(); exeErr == nil {
		base := filepath.Join(filepath.Dir(executable), "sdl3")
		include := filepath.Join(base, "include")
		lib := filepath.Join(base, "lib")
		// Bundled headers live under include/SDL3/; the include dir stays the
		// parent so the game's <SDL3/SDL.h> resolves.
		if fsutil.DirectoryExists(filepath.Join(include, "SDL3")) &&
			fsutil.DirectoryExists(lib) {
			return include, lib, true, nil
		}
	}
	if pkgConfig, lookErr := exec.LookPath("pkg-config"); lookErr == nil {
		includeOut, includeErr := exec.Command(pkgConfig, "--cflags-only-I", "sdl3").Output()
		libOut, libErr := exec.Command(pkgConfig, "--libs-only-L", "sdl3").Output()
		include := firstFlagValue(string(includeOut), "-I")
		lib := firstFlagValue(string(libOut), "-L")
		if includeErr == nil && libErr == nil && include != "" && lib != "" {
			return include, lib, false, nil
		}
	}
	candidates := []sdlCandidate{
		{"/opt/homebrew/opt/sdl3/include", "/opt/homebrew/opt/sdl3/lib"},
		{"/usr/local/opt/sdl3/include", "/usr/local/opt/sdl3/lib"},
		{"/opt/homebrew/include", "/opt/homebrew/lib"},
		{"/usr/local/include", "/usr/local/lib"},
		{"/usr/include", "/usr/lib"},
	}
	if runtime.GOOS == "linux" {
		// Debian/Ubuntu install the SDL3 shared object under a multiarch dir
		// (e.g. /usr/lib/x86_64-linux-gnu or .../aarch64-linux-gnu), not the
		// plain /usr/lib the base candidates check. Add both arch dirs so an
		// ARM64 Debian/Ubuntu box is found, not just x86_64.
		candidates = append(candidates,
			sdlCandidate{"/usr/include", "/usr/lib/x86_64-linux-gnu"},
			sdlCandidate{"/usr/include", "/usr/lib/aarch64-linux-gnu"},
			sdlCandidate{"/usr/local/include", "/usr/local/lib/x86_64-linux-gnu"},
			sdlCandidate{"/usr/local/include", "/usr/local/lib/aarch64-linux-gnu"},
		)
	}
	for _, candidate := range candidates {
		if fsutil.DirectoryExists(filepath.Join(candidate.include, "SDL3")) &&
			sdlLibDirHasLib(candidate.lib) {
			return candidate.include, candidate.lib, false, nil
		}
	}
	return "", "", false, fmt.Errorf("SDL3 development files not found; pass --sdl-include and --sdl-lib")
}

func firstFlagValue(output, prefix string) string {
	for _, field := range strings.Fields(output) {
		if strings.HasPrefix(field, prefix) && len(field) > len(prefix) {
			return field[len(prefix):]
		}
	}
	return ""
}
