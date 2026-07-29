package main

import (
	"io"

	"github.com/DerrickGold/snesrecomp-go/internal/buildgui"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
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

	// Build inputs (removable).
	write(filepath.Join(utils, "tools", "snesbuild"), "binary")
	write(filepath.Join(utils, "tools", "toolchain", "zig", "zig"), "zig")
	write(filepath.Join(utils, "src", "main.c"), "int main(void){return 0;}")
	write(filepath.Join(utils, "src", "gen", "bank00.c"), "generated")
	write(filepath.Join(utils, "recomp", "bank00.cfg"), "cfg")
	write(filepath.Join(utils, "snesrecomp-go", "runtime", "snes", "ppu.h"), "header")
	write(filepath.Join(utils, "third_party", "stb", "stb_image.h"), "header")
	write(filepath.Join(utils, "snesbuild.ini"), "[project]")

	// Runtime files that MUST survive (they live in utils/ too).
	write(filepath.Join(utils, "config.ini"), "[video]")
	write(filepath.Join(utils, "diorama-layers.ini"), "[layers:01:02]")
	write(filepath.Join(utils, "game-assets", "manifest.ini"), "[hd]")
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
	if !state.CanSlim || state.SlimBytes == 0 {
		t.Fatalf("CanSlim = %v SlimBytes = %d, want a removable payload",
			state.CanSlim, state.SlimBytes)
	}
	if state.Result.OutputPath == "" {
		t.Fatal("no launcher path reported, so Play could not work")
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
		filepath.Join(utils, "tools"),
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
		// Runtime files inside utils/ -- the launcher cd's here.
		filepath.Join(utils, "config.ini"),
		filepath.Join(utils, "diorama-layers.ini"),
		filepath.Join(utils, "game-assets", "manifest.ini"),
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
	tools := filepath.Join(utils, "tools")
	if err := os.RemoveAll(tools); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, tools); err != nil {
		t.Skipf("cannot create a symlink here: %v", err)
	}

	if err := slimInstall(utils, root, io.Discard); err != nil {
		t.Fatalf("slimInstall: %v", err)
	}

	// The link is gone; what it pointed at is untouched.
	if _, err := os.Lstat(tools); !os.IsNotExist(err) {
		t.Fatal("the symlink survived the cleanup")
	}
	if _, err := os.Stat(treasure); err != nil {
		t.Fatalf("cleanup followed the symlink and deleted outside the bundle: %v", err)
	}
}
