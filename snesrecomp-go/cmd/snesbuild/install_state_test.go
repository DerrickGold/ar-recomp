package main

import (
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/buildgui"
	"github.com/DerrickGold/snesrecomp-go/internal/project"
)

// A bundle-shaped fixture: build inputs under utils/, the playable game in the
// root, and the RUNTIME files that share utils/ with the build inputs. That
// sharing is the whole hazard the cleanup has to navigate -- the launcher cd's
// into utils/ before running, so config.ini, diorama-layers.ini, game-assets/
// and saves/ must survive a cleanup even though they sit beside the tools.
func bundleFixture(t *testing.T) (root, utils string) {
	t.Helper()
	root = t.TempDir()
	utils = filepath.Join(root, "utils")

	write := func(path, contents string) {
		t.Helper()
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(contents), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	// The builder itself, KEPT by the cleanup because run-build gates on it.
	// Mode 0755 like the real bundle (packaging installs it with PROGRAMS), so
	// the scripts' own `[ -x ]` predicate is what the tests can assert against.
	if err := os.MkdirAll(filepath.Join(utils, "tools"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(utils, "tools", "snesbuild"),
		[]byte("binary"), 0o755); err != nil {
		t.Fatal(err)
	}

	// Build inputs (removable).
	write(filepath.Join(utils, "tools", "toolchain", "zig", "zig"), "zig")
	write(filepath.Join(utils, "tools", "sdl3", "lib", "libSDL3.dylib"), "lib")
	write(filepath.Join(utils, "src", "main.c"), "int main(void){return 0;}")
	write(filepath.Join(utils, "src", "gen", "bank00.c"), "generated")
	write(filepath.Join(utils, "recomp", "bank00.cfg"), "cfg")
	write(filepath.Join(utils, "snesrecomp-go", "runtime", "include", "snesrecomp", "runner.h"), "header")
	runtimeArchive, err := project.VendedRuntimeArchivePath(
		filepath.Join(utils, "snesrecomp-go", "runtime"), "")
	if err != nil {
		t.Fatal(err)
	}
	write(runtimeArchive, "archive")
	write(filepath.Join(utils, "third_party", "stb", "stb_image.h"), "header")
	write(filepath.Join(utils, "snesbuild.ini"), "[project]")

	// Runtime files that MUST survive (they live in utils/ too).
	write(filepath.Join(utils, "config.ini"), "[video]")
	write(filepath.Join(utils, "diorama-layers.ini"), "[layers:01:02]")
	write(filepath.Join(utils, "game-assets", "manifest.ini"), "[hd]")
	write(filepath.Join(utils, "game-assets", "manual.pdf"), "manual")
	write(filepath.Join(utils, "saves", "battery.srm"), "save")

	// The playable game in the bundle root.
	installGameFixture(t, root)
	return root, utils
}

func installGameFixture(t *testing.T, root string) {
	t.Helper()
	binaryName, launcherName := "ActRaiserRecomp", "run-game.sh"
	if runtime.GOOS == "windows" {
		binaryName, launcherName = "ActRaiserRecomp.exe", "run-game.bat"
	} else if runtime.GOOS == "darwin" {
		launcherName = "run-game.command"
	}
	if err := os.WriteFile(filepath.Join(root, binaryName), []byte("game"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, launcherName), []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
}

func TestDetectReportsBothCapabilitiesOnAFullBundle(t *testing.T) {
	root, utils := bundleFixture(t)
	state := detectInstallState(utils, root)
	if !state.CanLaunch {
		t.Fatal("CanLaunch = false on a bundle with a built game")
	}
	if !state.CanRebuild {
		t.Fatal("CanRebuild = false with every build input present")
	}
	if !state.CanSlim {
		t.Fatal("CanSlim = false with build-only subtrees present")
	}
	// Detect reports PRESENCE and leaves the size to measureSlimBytes, because
	// it runs on every 500ms status poll and summing the trees means walking
	// them. A size here would mean the walk crept back in.
	if state.SlimBytes != 0 {
		t.Fatalf("SlimBytes = %d from detectInstallState, want 0: sizing walks the "+
			"tree and must stay out of the poll path (see measureSlimBytes)",
			state.SlimBytes)
	}
	if state.Result.OutputPath == "" {
		t.Fatal("no launcher path reported, so Play could not work")
	}
}

// The size the offer quotes comes from measureSlimBytes, which walks. It must
// count exactly the subtrees the cleanup removes -- quoting a figure that
// includes files the cleanup keeps would promise space it cannot reclaim.
func TestMeasureSlimBytesCountsOnlyTheRemovableSubtrees(t *testing.T) {
	_, utils := bundleFixture(t)
	payload := strings.Repeat("x", 4096)
	write := func(relative string) {
		t.Helper()
		path := filepath.Join(utils, relative)
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(payload), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	base := measureSlimBytes(utils)
	if base == 0 {
		t.Fatal("measureSlimBytes = 0 on a full bundle")
	}

	// A file in a REMOVED subtree must raise the figure...
	write(filepath.Join("build", "objects", "big.o"))
	withBuildFile := measureSlimBytes(utils)
	if withBuildFile != base+int64(len(payload)) {
		t.Fatalf("adding %d bytes under build/ moved the total by %d, want exactly that",
			len(payload), withBuildFile-base)
	}

	// ...and a file the cleanup KEEPS must not, or the offer over-promises.
	write(filepath.Join("game-assets", "hd", "tile.png"))
	write(filepath.Join("saves", "another.srm"))
	if kept := measureSlimBytes(utils); kept != withBuildFile {
		t.Fatalf("total moved by %d after writing only KEPT files; the offer would "+
			"promise space the cleanup does not reclaim", kept-withBuildFile)
	}
}

// Nothing built yet: rebuild is possible, launching is not, and the cleanup must
// NOT be offered -- removing the tools before there is a game would leave the
// user with neither.
func TestDetectOnFreshBundleDoesNotOfferCleanup(t *testing.T) {
	root, utils := bundleFixture(t)
	removeBuiltGame(t, root)
	state := detectInstallState(utils, root)
	if state.CanLaunch {
		t.Fatal("CanLaunch = true with no game present")
	}
	if !state.CanRebuild {
		t.Fatal("CanRebuild = false on a complete source tree")
	}
	if state.CanSlim {
		t.Fatal("cleanup offered before a game exists")
	}
}

// THE CASE THIS FEATURE EXISTS FOR: after a cleanup the game still launches and
// a rebuild is correctly reported as impossible.
func TestSlimLeavesALaunchableGameAndDisablesRebuild(t *testing.T) {
	root, utils := bundleFixture(t)
	if err := slimInstall(utils, root, io.Discard); err != nil {
		t.Fatalf("slimInstall: %v", err)
	}
	state := detectInstallState(utils, root)
	if !state.CanLaunch {
		t.Fatal("the game is no longer launchable after cleanup")
	}
	if state.CanRebuild {
		t.Fatal("CanRebuild = true after the inputs were removed")
	}
	if state.CanSlim {
		t.Fatal("cleanup still offered after it ran")
	}
}

// The allowlist must remove the build trees and NOTHING else. Runtime files
// share utils/ with them, and losing config.ini or saves/ would be a far worse
// outcome than keeping a few hundred MB.
func TestSlimRemovesOnlyBuildFiles(t *testing.T) {
	root, utils := bundleFixture(t)
	if err := slimInstall(utils, root, io.Discard); err != nil {
		t.Fatalf("slimInstall: %v", err)
	}

	mustBeGone := []string{
		filepath.Join(utils, "tools", "toolchain"),
		filepath.Join(utils, "tools", "sdl3"),
		filepath.Join(utils, "src"),
		filepath.Join(utils, "recomp"),
		filepath.Join(utils, "snesrecomp-go"),
		filepath.Join(utils, "third_party"),
	}
	for _, path := range mustBeGone {
		if _, err := os.Stat(path); !os.IsNotExist(err) {
			t.Fatalf("%s survived the cleanup", path)
		}
	}

	mustSurvive := []string{
		// snesbuild ITSELF. Every run-build script gates on this before doing
		// anything ([ -x "$UTILS/tools/snesbuild" ] || fail "This package looks
		// incomplete"), so deleting it makes the launcher mode this cleanup
		// transitions INTO unreachable -- and isSlimmableBundle uses it as the
		// proof that this is a bundle at all, so the cleanup would destroy its
		// own marker. It is 7.2 MB against the toolchain's 415 MB.
		filepath.Join(utils, "tools", "snesbuild"),
		// Runtime files inside utils/ -- the launcher cd's here.
		filepath.Join(utils, "config.ini"),
		filepath.Join(utils, "diorama-layers.ini"),
		filepath.Join(utils, "game-assets", "manifest.ini"),
		filepath.Join(utils, "game-assets", "manual.pdf"),
		filepath.Join(utils, "saves", "battery.srm"),
		// And the game itself.
		filepath.Join(root, gameBinaryName()),
	}
	for _, path := range mustSurvive {
		if _, err := os.Stat(path); err != nil {
			t.Fatalf("cleanup removed %s: %v", path, err)
		}
	}
}

// removeBuiltGame deletes what makes a tree "built". The BINARY is the load
// bearing part now: the GUI runs it directly, so its presence is what CanLaunch
// keys on, and the run-game script is a convenience for playing without the GUI.
func removeBuiltGame(t *testing.T, root string) {
	t.Helper()
	if err := os.Remove(filepath.Join(root, gameBinaryName())); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"run-game.sh", "run-game.command", "run-game.bat"} {
		_ = os.Remove(filepath.Join(root, name))
	}
}

func gameBinaryName() string {
	if runtime.GOOS == "windows" {
		return "ActRaiserRecomp.exe"
	}
	return "ActRaiserRecomp"
}

// Refusing to slim without a built game is the guard that stops a mis-click
// leaving the user with neither a game nor the means to build one.
func TestSlimRefusesWithoutABuiltGame(t *testing.T) {
	root, utils := bundleFixture(t)
	removeBuiltGame(t, root)
	if err := slimInstall(utils, root, io.Discard); err == nil {
		t.Fatal("slimInstall succeeded with no built game")
	}
	// And nothing was removed.
	if _, err := os.Stat(filepath.Join(utils, "tools")); err != nil {
		t.Fatalf("tools/ removed despite the refusal: %v", err)
	}
}

// A script without its binary is a half-deleted install: nothing can run, so
// offering Play would produce a failure.
func TestDetectRejectsALauncherWithoutItsBinary(t *testing.T) {
	root, utils := bundleFixture(t)
	if err := os.Remove(filepath.Join(root, gameBinaryName())); err != nil {
		t.Fatal(err)
	}
	if state := detectInstallState(utils, root); state.CanLaunch {
		t.Fatal("CanLaunch = true with the game binary missing")
	}
}

// The INVERSE, and the reason detection moved off the script: the GUI runs the
// binary itself, so a deleted run-game script must NOT cost the user their Play
// button. It is reported when present and simply omitted when not.
func TestDetectLaunchesWithoutTheRunGameScript(t *testing.T) {
	root, utils := bundleFixture(t)
	for _, name := range []string{"run-game.sh", "run-game.command", "run-game.bat"} {
		_ = os.Remove(filepath.Join(root, name))
	}
	state := detectInstallState(utils, root)
	if !state.CanLaunch {
		t.Fatal("CanLaunch = false with the binary present but no script")
	}
	if state.Result.BinaryPath == "" {
		t.Fatal("no binary path reported, so the GUI could not launch it")
	}
	if state.Result.OutputPath != "" {
		t.Fatalf("OutputPath = %q, want empty with no script present",
			state.Result.OutputPath)
	}
	// The working directory has to be the project root, or the game will not
	// find config.ini, its assets, or its saves.
	if state.Result.WorkingDir != utils {
		t.Fatalf("WorkingDir = %q, want %q", state.Result.WorkingDir, utils)
	}
}

// The ROM is passed as a bare leaf, exactly as the generated script passes it, so
// the game resolves it against its working directory the same way in both paths.
func TestInstalledROMIsFoundAsABareLeaf(t *testing.T) {
	_, utils := bundleFixture(t)
	if got := findInstalledROM(utils); got != "" {
		t.Fatalf("findInstalledROM = %q with no ROM staged", got)
	}
	if err := os.WriteFile(filepath.Join(utils, "user-rom.sfc"),
		[]byte("rom"), 0o600); err != nil {
		t.Fatal(err)
	}
	if got := findInstalledROM(utils); got != "user-rom.sfc" {
		t.Fatalf("findInstalledROM = %q, want the bare leaf user-rom.sfc", got)
	}
}

// Each rebuild input is individually load-bearing: removing any ONE must
// disable rebuild. Guards against a probe that only checks the first path, or
// one that drifts out of step with what the build actually reads.
func TestEveryRebuildInputIsRequired(t *testing.T) {
	for _, relative := range rebuildInputs {
		t.Run(relative, func(t *testing.T) {
			root, utils := bundleFixture(t)
			if err := os.RemoveAll(filepath.Join(utils, relative)); err != nil {
				t.Fatal(err)
			}
			if state := detectInstallState(utils, root); state.CanRebuild {
				t.Fatalf("CanRebuild = true without %s", relative)
			}
		})
	}
}

func TestMismatchedRuntimeArchiveCannotClaimRebuild(t *testing.T) {
	root, utils := bundleFixture(t)
	runtimeDir := filepath.Join(utils, "snesrecomp-go", "runtime")
	archive, err := project.VendedRuntimeArchivePath(runtimeDir, "")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(archive); err != nil {
		t.Fatal(err)
	}
	writeTestArchive := filepath.Join(runtimeDir, "lib", "wrong-target",
		"libsnesrecomp_runtime.a")
	if err := os.MkdirAll(filepath.Dir(writeTestArchive), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(writeTestArchive, []byte("archive"), 0o644); err != nil {
		t.Fatal(err)
	}
	if state := detectInstallState(utils, root); state.CanRebuild {
		t.Fatal("CanRebuild = true with only a mismatched runner archive")
	}
}

// The bundle README tells users which folders they may delete by hand. That list
// and the allowlist this code deletes must be the SAME list: if they drift, the
// README either tells someone to remove a file the game needs, or omits one the
// cleanup silently takes. Pinned against the real template rather than a copy.
func TestBundleReadmeMatchesTheCleanupAllowlist(t *testing.T) {
	readme, err := os.ReadFile(filepath.Join("..", "..", "packaging", "README.txt.in"))
	if err != nil {
		t.Skipf("bundle README not readable from here: %v", err)
	}
	text := string(readme)
	for _, relative := range buildOnlySubtrees {
		if !strings.Contains(text, "utils/"+relative) {
			t.Errorf("README does not list utils/%s as safe to delete, but the "+
				"cleanup removes it", relative)
		}
	}
	// And the runtime files must be named as keep-these, since they share utils/
	// with the build inputs and are the whole reason this is an allowlist.
	for _, keep := range []string{
		"utils/config.ini", "utils/diorama-layers.ini",
		"utils/game-assets", "utils/saves",
	} {
		if !strings.Contains(text, keep) {
			t.Errorf("README does not tell the user to keep %s", keep)
		}
	}
}

// src/gen and the toolchain are REGENERATED or re-fetched by the build, so their
// absence must NOT disable rebuild -- gating on them would refuse a build that
// would have succeeded. This is the inverse of the test above and just as
// important.
func TestRegenerableInputsDoNotGateRebuild(t *testing.T) {
	for _, relative := range []string{
		filepath.Join("src", "gen"),
		filepath.Join("tools", "toolchain"),
		"build",
	} {
		t.Run(relative, func(t *testing.T) {
			root, utils := bundleFixture(t)
			if err := os.RemoveAll(filepath.Join(utils, relative)); err != nil {
				t.Fatal(err)
			}
			if state := detectInstallState(utils, root); !state.CanRebuild {
				t.Fatalf("CanRebuild = false without %s, which the build recreates",
					relative)
			}
		})
	}
}

// launchBuiltGame had NO test, which let two mutation probes through: skipping
// the binary-existence check, and dropping the working directory. Both matter --
// the first is the entire reason the GUI stopped shelling out to the script (so a
// broken game reports a real error), and the second decides whether the game
// finds config.ini, its assets and its saves at all.
//
// Uses a stub executable that records how it was invoked, so this asserts the
// real exec path rather than a paraphrase of it.
func TestLaunchRunsTheBinaryFromTheProjectDirectory(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the stub game is a shell script")
	}
	root := t.TempDir()
	utils := filepath.Join(root, "utils")
	if err := os.MkdirAll(utils, 0o755); err != nil {
		t.Fatal(err)
	}
	record := filepath.Join(root, "invocation.txt")
	stub := "#!/bin/sh\n{ echo \"cwd=$(pwd)\"; echo \"args=$*\"; } > " + record + "\n"
	binary := filepath.Join(root, "ActRaiserRecomp")
	if err := os.WriteFile(binary, []byte(stub), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(utils, "user-rom.sfc"), []byte("rom"), 0o600); err != nil {
		t.Fatal(err)
	}

	if err := launchBuiltGame(buildgui.Result{
		BinaryPath: binary, WorkingDir: utils,
	}); err != nil {
		t.Fatalf("launchBuiltGame: %v", err)
	}

	// Started asynchronously; poll briefly rather than sleeping a fixed time.
	var contents []byte
	for attempt := 0; attempt < 100; attempt++ {
		if data, err := os.ReadFile(record); err == nil && len(data) > 0 {
			contents = data
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if len(contents) == 0 {
		t.Fatal("the game was never invoked")
	}
	text := string(contents)
	// THE working-directory assertion: the game must run from the project dir or
	// it will not find config.ini.
	//
	// Accepts either spelling of the path. macOS TempDirs live under /var, which
	// is a symlink to /private/var, and `pwd` in sh reports the LOGICAL path
	// while EvalSymlinks reports the physical one -- so pinning just one would
	// fail on macOS for a reason that has nothing to do with the code.
	resolved, err := filepath.EvalSymlinks(utils)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(text, "cwd="+utils+"\n") &&
		!strings.Contains(text, "cwd="+resolved+"\n") {
		t.Fatalf("wrong working directory:\n%s\nwant cwd=%s (or %s)",
			text, utils, resolved)
	}
	// And the same arguments the generated script passes, ROM as a bare leaf.
	if !strings.Contains(text, "args=user-rom.sfc --config config.ini\n") {
		t.Fatalf("wrong arguments:\n%s", text)
	}
}

// A missing or unrunnable game must FAIL rather than report success. This is the
// defect the direct-launch change exists to fix: `open`/`start` succeed as soon
// as the OS accepts the handoff, so a deleted game looked like it launched.
func TestLaunchReportsAMissingOrUnrunnableGame(t *testing.T) {
	root := t.TempDir()

	if err := launchBuiltGame(buildgui.Result{
		BinaryPath: filepath.Join(root, "does-not-exist"), WorkingDir: root,
	}); err == nil {
		t.Fatal("launching a missing game reported success")
	}

	directory := filepath.Join(root, "a-directory")
	if err := os.Mkdir(directory, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := launchBuiltGame(buildgui.Result{
		BinaryPath: directory, WorkingDir: root,
	}); err == nil {
		t.Fatal("launching a directory reported success")
	}
}

// The cleanup deletes directory trees, so it must not be able to follow a symlink
// out of the bundle. os.RemoveAll removes the LINK rather than walking into its
// target, which is what makes the allowlist safe -- but that is a property of the
// stdlib rather than of this code, so it is pinned here: if a future refactor
// swapped in a hand-rolled walker, this is what would catch it.
func TestCleanupDoesNotFollowSymlinksOutOfTheBundle(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("symlink creation needs elevation on Windows")
	}
	root, utils := bundleFixture(t)

	// Something valuable OUTSIDE the bundle, reachable only via a symlink that
	// occupies one of the allowlisted names.
	outside := t.TempDir()
	treasure := filepath.Join(outside, "do-not-delete.txt")
	if err := os.WriteFile(treasure, []byte("precious"), 0o600); err != nil {
		t.Fatal(err)
	}
	// Point one of the OTHER allowlisted names at the outside directory, leaving
	// utils/tools (and the tools/snesbuild bundle marker) intact -- otherwise the
	// install would no longer look like a bundle and the cleanup would refuse for
	// an unrelated reason, which would make this test pass vacuously.
	linked := filepath.Join(utils, "third_party")
	if err := os.RemoveAll(linked); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, linked); err != nil {
		t.Skipf("cannot create a symlink here: %v", err)
	}

	if err := slimInstall(utils, root, io.Discard); err != nil {
		t.Fatalf("slimInstall: %v", err)
	}

	// The link is gone; what it pointed at is untouched.
	if _, err := os.Lstat(linked); !os.IsNotExist(err) {
		t.Fatal("the symlink survived the cleanup")
	}
	if _, err := os.Stat(treasure); err != nil {
		t.Fatalf("cleanup followed the symlink and deleted outside the bundle: %v", err)
	}
}

// A FRESH bundle -- extracted, never built -- must report itself unlaunchable and
// must not offer the cleanup. Found by an audit probe, and it was severe: the
// bundle root ships run-build.command/.sh as executables (install(PROGRAMS) => 0755)
// and the old exclusion-based heuristic only skipped run-game.*, so
// findGameBinary returned run-build.command as "the game". That made CanLaunch and
// CanSlim both true on a bundle with no game -- Play would fail, and accepting the
// cleanup would delete the build tools, leaving the user unable to build at all.
//
// This is why findGameBinary now prefers the known target name and rejects script
// extensions outright rather than trusting a deny-list.
func TestFreshNeverBuiltBundleIsNotLaunchable(t *testing.T) {
	root, utils := bundleFixture(t)
	// Remove the built game, leaving a bundle as it ships.
	removeBuiltGame(t, root)
	// Re-create exactly what the packaging puts in the root.
	for _, name := range []string{"run-build.command", "run-build.sh", "run-build.bat"} {
		if err := os.WriteFile(filepath.Join(root, name),
			[]byte("#!/bin/sh\n"), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(root, "README.txt"),
		[]byte("hi"), 0o644); err != nil {
		t.Fatal(err)
	}

	if got := findGameBinary(root); got != "" {
		t.Fatalf("findGameBinary = %q on a never-built bundle, want \"\"", got)
	}
	state := detectInstallState(utils, root)
	if state.CanLaunch {
		t.Fatal("CanLaunch = true on a bundle that has never been built")
	}
	if state.CanSlim {
		t.Fatal("cleanup offered on a bundle that has never been built")
	}
	if !state.CanRebuild {
		t.Fatal("CanRebuild = false on a complete fresh bundle")
	}
	// And the cleanup refuses outright, so even a forced call cannot strand them.
	if err := slimInstall(utils, root, io.Discard); err == nil {
		t.Fatal("slimInstall succeeded on a never-built bundle")
	}
	if _, err := os.Stat(filepath.Join(utils, "tools")); err != nil {
		t.Fatalf("build tools were deleted from a never-built bundle: %v", err)
	}
}

// The real game binary must still be found when it IS present, alongside the
// bundle scripts -- the fix must not have thrown out the detection it exists for.
func TestGameBinaryFoundAlongsideBundleScripts(t *testing.T) {
	root, utils := bundleFixture(t)
	for _, name := range []string{"run-build.command", "run-build.sh"} {
		if err := os.WriteFile(filepath.Join(root, name),
			[]byte("#!/bin/sh\n"), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	got := findGameBinary(root)
	if filepath.Base(got) != gameBinaryName() {
		t.Fatalf("findGameBinary = %q, want the game binary %q", got, gameBinaryName())
	}
	if state := detectInstallState(utils, root); !state.CanLaunch {
		t.Fatal("CanLaunch = false with the real game present")
	}
}

// A checkout-shaped root must never qualify for destructive bundle cleanup.
// False negatives only hide the offer; false positives can delete source.
func TestCleanupRefusesInASourceCheckout(t *testing.T) {
	repo := t.TempDir()
	for _, dir := range []string{"src", "recomp", "tools", "third_party",
		"snesrecomp-go/runtime", "build"} {
		if err := os.MkdirAll(filepath.Join(repo, dir), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	precious := map[string]string{
		filepath.Join("src", "main.c"):        "authored source",
		filepath.Join("recomp", "bank00.cfg"): "cfg",
		filepath.Join("tools", "canary.sh"):   "#!/bin/sh\n",
		"snesbuild.ini":                       "x",
		"CMakeLists.txt":                      "project()", // the checkout marker
	}
	for name, body := range precious {
		if err := os.WriteFile(filepath.Join(repo, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	// A checkout can legitimately have a built game in it too -- that is what
	// `--root .` produces -- so its presence must not be what unlocks deletion.
	installGameFixture(t, repo)
	// Even the bundled tool being present must not be enough on its own.
	if err := os.WriteFile(filepath.Join(repo, "tools", "snesbuild"),
		[]byte("bin"), 0o755); err != nil {
		t.Fatal(err)
	}

	state := detectInstallState(repo, repo)
	if state.CanSlim {
		t.Error("cleanup offered inside a source checkout")
	}
	if err := slimInstall(repo, repo, io.Discard); err == nil {
		t.Error("slimInstall succeeded inside a source checkout")
	}
	// Nothing was touched.
	for name := range precious {
		if _, err := os.Stat(filepath.Join(repo, name)); err != nil {
			t.Errorf("cleanup removed the checkout's %s: %v", name, err)
		}
	}
}

// The mirror: a real bundle IS slimmable, so the guard has not simply disabled
// the feature.
func TestCleanupStillWorksInARealBundle(t *testing.T) {
	root, utils := bundleFixture(t)
	if !isSlimmableBundle(utils) {
		t.Fatal("a bundle-shaped fixture was not recognised as slimmable")
	}
	state := detectInstallState(utils, root)
	if !state.CanSlim {
		t.Fatal("cleanup not offered on a real bundle with a built game")
	}
	if err := slimInstall(utils, root, io.Discard); err != nil {
		t.Fatalf("slimInstall refused a real bundle: %v", err)
	}
}

// The cleanup transitions the install INTO launcher mode, so the way back into
// launcher mode must survive it. Every run-build script opens with
//
//	[ -x "$UTILS/tools/snesbuild" ] || fail "This package looks incomplete."
//
// and that predicate is read from the REAL shipped scripts here rather than
// restated, so moving the guard cannot silently unpin this test. Deleting all
// of utils/tools passed every other assertion -- the game still ran, the state
// still reported "launcher" -- while making that mode unreachable through the
// documented entry point, and telling the user their package was corrupt.
func TestCleanupLeavesRunBuildAbleToStart(t *testing.T) {
	guards := map[string]string{
		"run-build.sh":      "tools/snesbuild",
		"run-build.command": "tools/snesbuild",
		"run-build.bat":     `tools\snesbuild.exe`,
	}
	checked := 0
	for script, needs := range guards {
		source, err := os.ReadFile(filepath.Join("..", "..", "packaging", "scripts", script))
		if err != nil {
			t.Fatalf("read %s: %v", script, err)
		}
		if !strings.Contains(string(source), needs) {
			t.Fatalf("%s no longer references %q -- this test is unpinned from the "+
				"real guard and must be updated", script, needs)
		}
		checked++
	}
	if checked != 3 {
		t.Fatalf("checked %d scripts, want 3", checked)
	}

	root, utils := bundleFixture(t)
	if err := slimInstall(utils, root, io.Discard); err != nil {
		t.Fatalf("slimInstall: %v", err)
	}
	// The POSIX guard's own test: -x on utils/tools/snesbuild.
	info, err := os.Stat(filepath.Join(utils, "tools", "snesbuild"))
	if err != nil {
		t.Fatalf("utils/tools/snesbuild is gone after the cleanup, so every "+
			"run-build script aborts with \"This package looks incomplete\" and "+
			"launcher mode cannot be reached: %v", err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0o111 == 0 {
		t.Fatal("utils/tools/snesbuild survived but is not executable, so the " +
			"scripts' [ -x ] guard still fails")
	}
	// And the cleanup must still have been worth doing.
	state := detectInstallState(utils, root)
	if state.CanRebuild {
		t.Fatal("CanRebuild is still true after the cleanup")
	}
	if !state.CanLaunch {
		t.Fatal("CanLaunch is false after the cleanup, so nothing can be played")
	}
}
