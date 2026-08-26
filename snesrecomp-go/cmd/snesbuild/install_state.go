package main

import (
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/buildgui"
	"github.com/DerrickGold/snesrecomp-go/internal/project"
)

// Install-state probing and the "keep just the game" cleanup.
//
// This lives in cmd/snesbuild rather than in internal/buildgui because it is
// entirely about WHERE the packaging rules put things, and the GUI package is
// deliberately free of filesystem layout knowledge so it stays unit-testable.
//
// THE BUNDLE LAYOUT, which the two file sets below follow from
// (packaging/CMakeLists.txt and packaging/scripts/run-build.sh):
//
//	<root>/                     the playable game -- KEEP
//	  ActRaiserRecomp[.exe]     the binary
//	  libSDL3*                  its libraries
//	  run-game.{sh,command,bat} the launcher
//	  user-rom.sfc              may live here or under utils/
//	<root>/utils/               the GUI's --root
//	  config.ini                RUNTIME: the launcher cd's here first
//	  diorama-layers.ini        RUNTIME: authored layer overrides
//	  game-assets/              RUNTIME: manual and HD/audio replacement assets
//	  saves/                    RUNTIME: battery saves and generated metadata
//	  src/  recomp/  third_party/  snesrecomp-go/          BUILD ONLY
//	  tools/snesbuild           KEEP: run-build gates on it, and it is the
//	                            only way back into launcher mode
//	  tools/toolchain  tools/sdl3   BUILD ONLY (415 MB and 8.5 MB)
//
// The trap: utils/ is NOT a pure source drop. The generated launcher does
// `cd "$ROOT/utils"` before exec'ing the binary from the bundle root, so
// config.ini, diorama-layers.ini, game-assets/ and saves/ are all resolved
// relative to it. Deleting utils/ wholesale would leave a game that starts
// without its settings, its authored layers, or its saves. The cleanup is
// therefore an ALLOWLIST of build-only subtrees, never a "delete utils/".

// buildOnlySubtrees are the paths under the GUI's root that exist solely to
// support a rebuild. Everything not named here is either a runtime file or
// something we do not own, and is left alone.
//
// Ordered biggest-first so the log reads usefully rather than alphabetically.
// NOT "tools": that directory holds snesbuild ITSELF, and every run-build
// script gates on it before doing anything else --
//
//	[ -x "$UTILS/tools/snesbuild" ] || fail "This package looks incomplete."
//
// so removing it makes the launcher mode this cleanup transitions INTO
// unreachable through the documented entry point. isSlimmableBundle also uses
// that binary as its proof-of-bundle, so a "delete tools" cleanup destroys its
// own marker. The toolchain is the size anyway: 415 MB of the real bundle's
// 431 MB under tools/, against 7.2 MB for snesbuild.
//
// tools/sdl3 IS removable: copySDLRuntime copies libSDL3 next to the game binary
// at link time, and the rpath is
// @executable_path/$ORIGIN, so the game never reads it from here.
var buildOnlySubtrees = []string{
	"tools/toolchain", // the pinned Zig toolchain -- 415 of tools/'s 431 MB
	"tools/sdl3",      // headers + the lib the build links; the game has its own copy
	"build",           // CMake/Zig scratch, incl. the fetched toolchain cache
	"src",             // authored C sources, and src/gen's regenerated output
	"recomp",          // the recompiler's per-bank .cfg inputs
	"snesrecomp-go",   // the runtime headers the hermetic build compiles against
	"third_party",     // stb, vendored for the build
}

// rebuildInputs must ALL exist for a rebuild to be possible. Deliberately the
// things that cannot be regenerated or re-fetched:
//
//   - recomp/ holds the per-bank configuration the generator reads.
//   - snesbuild.ini is the source manifest (project.ManifestFileName).
//   - snesrecomp-go/runtime-next/ is what the generated C compiles against.
//   - src/ is the authored game code.
//
// Deliberately NOT gated on: the Zig toolchain (project.HermeticBuild fetches
// it when absent) and src/gen (regenerated from the ROM every build). Gating on
// those would refuse a rebuild that would in fact have succeeded.
var rebuildInputs = []string{
	"recomp",
	project.ManifestFileName,
	filepath.Join("snesrecomp-go", "runtime-next"),
	"src",
}

// detectInstallState reports what this copy can do. root is the GUI's project
// root (utils/ in a bundle); outputDir is where a built game is installed (the
// bundle root).
func detectInstallState(root, outputDir string) buildgui.InstallState {
	state := buildgui.InstallState{}

	// CAN LAUNCH: keyed on the BINARY, because that is what the GUI now runs.
	// The run-game script is reported when present (it is how you play without
	// the GUI) but its absence no longer hides the Play button -- the GUI does
	// not need it, and someone who deleted the script should not lose the
	// ability to launch from here.
	if binary := findGameBinary(outputDir); binary != "" {
		state.CanLaunch = true
		state.Result = buildgui.Result{
			Message:    "Your game is built and ready.",
			OutputPath: findLauncherScript(outputDir),
			BinaryPath: binary,
			WorkingDir: root,
		}
	}

	// CAN REBUILD: every non-regenerable input is present.
	state.CanRebuild = true
	for _, relative := range rebuildInputs {
		if _, err := os.Stat(filepath.Join(root, relative)); err != nil {
			state.CanRebuild = false
			break
		}
	}

	// CAN SLIM: there is a build-only subtree left to remove. Offered only when
	// the game is playable, since reclaiming space before there is a working
	// build would trade away the ability to make one for nothing -- AND only in a
	// real bundle, never a source checkout (see isSlimmableBundle).
	//
	// PRESENCE only, no sizing: this function runs on every status poll, and
	// summing the trees means walking them. measureSlimBytes does that, from the
	// two places the size can actually change.
	if state.CanLaunch && isSlimmableBundle(root) {
		for _, relative := range buildOnlySubtrees {
			if info, err := os.Stat(filepath.Join(root, relative)); err == nil && info.IsDir() {
				state.CanSlim = true
				break
			}
		}
	}
	return state
}

// measureSlimBytes sums the build-only subtrees. Split from detectInstallState
// because it WALKS: ~24ms for 900 files, ~74ms for 3600. Wired to the GUI's
// MeasureSlim hook, which runs at session start and after a build or cleanup --
// not at the poll interval.
func measureSlimBytes(root string) int64 {
	var total int64
	for _, relative := range buildOnlySubtrees {
		path := filepath.Join(root, relative)
		if info, err := os.Stat(path); err == nil && info.IsDir() {
			total += directorySize(path)
		}
	}
	return total
}

// isSlimmableBundle reports whether `root` is a shipped bundle's utils/ directory
// rather than a developer's source checkout.
//
// This guard is mandatory. docs/BUILD_TOOLING.md documents
// `snesbuild gui --root .` for a checkout, where the cleanup allowlist names
// tracked source. A permissive check would delete that source; an audit fixture
// reproduced exactly that failure.
//
// Require positive proof of a bundle rather than trying to recognize every
// possible checkout shape:
//
//   - the bundled snesbuild lives at tools/snesbuild[.exe]; a checkout builds it
//     into build/ or a GOPATH instead, and never has it there.
//   - a checkout marker (.git, or the top-level CMakeLists.txt/Makefile that only
//     a source tree carries) is ABSENT.
//
// A false negative merely hides the space-saving offer, which costs nothing. A
// false positive deletes someone's source, so the asymmetry is deliberate.
func isSlimmableBundle(root string) bool {
	bundled := false
	for _, name := range []string{"snesbuild", "snesbuild.exe"} {
		if info, err := os.Stat(filepath.Join(root, "tools", name)); err == nil &&
			info.Mode().IsRegular() {
			bundled = true
			break
		}
	}
	if !bundled {
		return false
	}
	// Anything that marks a source tree disqualifies it, even if tools/snesbuild
	// happens to exist there.
	for _, marker := range []string{".git", "CMakeLists.txt", "Makefile", "go.mod"} {
		if _, err := os.Lstat(filepath.Join(root, marker)); err == nil {
			return false
		}
	}
	return true
}

// findLauncherScript returns the generated run-game script in dir, or "".
//
// Every platform spelling is probed rather than only this host's, so a bundle
// copied between machines still reports what is actually there.
func findLauncherScript(dir string) string {
	for _, name := range []string{"run-game.command", "run-game.sh", "run-game.bat"} {
		candidate := filepath.Join(dir, name)
		if info, err := os.Stat(candidate); err == nil && info.Mode().IsRegular() {
			return candidate
		}
	}
	return ""
}

// findInstalledROM returns the ROM the launcher would pass to the game, or "".
//
// The GUI stores the user's ROM as user-rom.sfc in its project root
// (buildgui.storeROM); game.sfc is the CLI's default name (project.DefaultPaths)
// and is accepted so a GUI opened over a CLI-built tree still launches. Returned
// as a BARE LEAF, matching what the generated script passes, so the game resolves
// it against its working directory exactly as it would there.
func findInstalledROM(dir string) string {
	if dir == "" {
		return ""
	}
	for _, name := range []string{"user-rom.sfc", "game.sfc"} {
		if info, err := os.Stat(filepath.Join(dir, name)); err == nil &&
			info.Mode().IsRegular() {
			return name
		}
	}
	return ""
}

// bundleScriptPrefixes are the shipped entry-point scripts that live in the
// bundle root alongside the game. They are executable and present BEFORE any
// build, so a heuristic that only excluded run-game.* mistook run-build.command
// for the game itself -- reporting a never-built bundle as launchable and, worse,
// making the cleanup offer to delete the build tools of a bundle that had never
// produced a game. Found by an audit probe, and the reason the check below is
// now an ALLOWLIST rather than a list of exclusions.
var bundleScriptPrefixes = []string{"run-game.", "run-build."}

// findGameBinary returns the installed game executable in dir, or "".
//
// ALLOWLIST, not exclusion. The previous version accepted any executable that
// was not on a deny-list, which meant every new file shipped in the bundle root
// was a potential false positive -- and a false positive here is severe: it makes
// CanLaunch true for a bundle with no game, so Play fails and the cleanup offers
// to remove the only means of building one.
//
// The name comes from the CMake target rather than from anything this package
// controls, so it cannot simply be hardcoded. What IS known is that the game is
// the executable whose name matches the project's binary, so that is what is
// looked for -- with the bundle's own scripts explicitly rejected either way.
func findGameBinary(dir string) string {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return ""
	}
	var fallback string
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		lower := strings.ToLower(name)

		// The bundle's own entry-point scripts are never the game.
		isScript := false
		for _, prefix := range bundleScriptPrefixes {
			if strings.HasPrefix(lower, prefix) {
				isScript = true
				break
			}
		}
		if isScript || strings.Contains(lower, "sdl3") {
			continue
		}
		switch filepath.Ext(lower) {
		case ".dll", ".so", ".dylib", ".ini", ".txt", ".md", ".sfc", ".smc",
			".webp", ".pdf", ".command", ".sh", ".bat", ".cmd", ".ps1":
			continue
		}
		info, statErr := entry.Info()
		if statErr != nil || !info.Mode().IsRegular() {
			continue
		}
		if runtime.GOOS == "windows" {
			if filepath.Ext(lower) != ".exe" {
				continue
			}
		} else if info.Mode().Perm()&0o111 == 0 {
			continue
		}

		// Prefer the expected name outright; keep any other candidate only as a
		// fallback, so a renamed target still works but a stray executable never
		// wins over the real game.
		if strings.HasPrefix(lower, strings.ToLower(gameBinaryStem)) {
			return filepath.Join(dir, name)
		}
		if fallback == "" {
			fallback = filepath.Join(dir, name)
		}
	}
	return fallback
}

// gameBinaryStem is the CMake target name (CMakeLists.txt: add_executable). Used
// as a preference rather than a requirement, so a rename does not silently break
// detection -- but a match wins over any other executable in the folder.
const gameBinaryStem = "ActRaiserRecomp"

// directorySize sums the regular-file bytes under path. Errors are ignored on
// purpose: this figure exists only to answer "is this worth doing?", and a
// partial total is far better than refusing to show one.
func directorySize(path string) int64 {
	var total int64
	_ = filepath.WalkDir(path, func(_ string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return nil
		}
		if info, infoErr := entry.Info(); infoErr == nil && info.Mode().IsRegular() {
			total += info.Size()
		}
		return nil
	})
	return total
}

// slimInstall removes the build-only subtrees, leaving a copy that still runs
// the game. Narrates what it removes, because deleting hundreds of megabytes
// should not be silent.
func slimInstall(root, outputDir string, output io.Writer) error {
	// Refuse unless the game is actually playable. Otherwise a mis-click could
	// remove the only means of producing one -- the user would be left with
	// neither a game nor a way to build it.
	if findGameBinary(outputDir) == "" {
		return fmt.Errorf(
			"no built game was found in %s, so the build tools are still needed",
			outputDir)
	}
	// Enforced here as well as in the offer, because this function DELETES. The
	// offer is presentation; a caller reaching this directly (a stale page, a
	// future CLI subcommand, a test) must not be able to remove a developer's
	// tracked source. See isSlimmableBundle.
	if !isSlimmableBundle(root) {
		return fmt.Errorf(
			"%s does not look like a packaged bundle -- refusing to remove build "+
				"files from what may be a source checkout", root)
	}
	fmt.Fprintln(output, "Removing build-only files; the game and its settings are kept.")
	var removed int64
	for _, relative := range buildOnlySubtrees {
		path := filepath.Join(root, relative)
		info, err := os.Stat(path)
		if err != nil || !info.IsDir() {
			continue
		}
		size := directorySize(path)
		if err := os.RemoveAll(path); err != nil {
			return fmt.Errorf("remove %s: %w", relative, err)
		}
		removed += size
		fmt.Fprintf(output, "  removed %s\n", relative)
	}
	fmt.Fprintf(output,
		"\nDone — about %d MB reclaimed. To rebuild later, download the package "+
			"again from the repository.\n", removed>>20)
	return nil
}
