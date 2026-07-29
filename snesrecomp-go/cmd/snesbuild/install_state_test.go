package main

import (
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
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
	for _, name := range []string{"run-game.sh", "run-game.command", "run-game.bat"} {
		_ = os.Remove(filepath.Join(root, name))
	}
	state := detectInstallState(utils, root)
	if state.CanLaunch {
		t.Fatal("CanLaunch = true with no launcher present")
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
	for _, name := range []string{"run-game.sh", "run-game.command", "run-game.bat"} {
		_ = os.Remove(filepath.Join(root, name))
	}
	if err := slimInstall(utils, root, io.Discard); err == nil {
		t.Fatal("slimInstall succeeded with no built game")
	}
	// And nothing was removed.
	if _, err := os.Stat(filepath.Join(utils, "tools")); err != nil {
		t.Fatalf("tools/ removed despite the refusal: %v", err)
	}
}

// A launcher whose binary has been deleted is a half-broken install. Reporting
// it as launchable would offer a Play button that fails.
func TestDetectRejectsALauncherWithoutItsBinary(t *testing.T) {
	root, utils := bundleFixture(t)
	if err := os.Remove(filepath.Join(root, gameBinaryName())); err != nil {
		t.Fatal(err)
	}
	if state := detectInstallState(utils, root); state.CanLaunch {
		t.Fatal("CanLaunch = true with the game binary missing")
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
