package project

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestInstallPlayableCreatesRelocatableLauncher(t *testing.T) {
	root := t.TempDir()
	projectRoot := filepath.Join(root, "utils")
	buildDir := filepath.Join(projectRoot, "build", "hermetic")
	if err := os.MkdirAll(buildDir, 0o755); err != nil {
		t.Fatal(err)
	}
	binaryName := "TestGame"
	if runtime.GOOS == "windows" {
		binaryName += ".exe"
	}
	binary := filepath.Join(buildDir, binaryName)
	rom := filepath.Join(projectRoot, "user-rom.sfc")
	if err := os.WriteFile(binary, []byte("binary"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(rom, []byte("rom"), 0o600); err != nil {
		t.Fatal(err)
	}
	libraryName := "libSDL3-test.so"
	if runtime.GOOS == "darwin" {
		libraryName = "libSDL3-test.dylib"
	} else if runtime.GOOS == "windows" {
		libraryName = "SDL3-test.dll"
	}
	if err := os.WriteFile(filepath.Join(buildDir, libraryName),
		[]byte("library"), 0o755); err != nil {
		t.Fatal(err)
	}

	installed, err := InstallPlayable(InstallOptions{
		BinaryPath: binary, ROMPath: rom,
		ProjectRoot: projectRoot, DestinationDir: root,
	})
	if err != nil {
		t.Fatal(err)
	}
	if data, err := os.ReadFile(installed.BinaryPath); err != nil || string(data) != "binary" {
		t.Fatalf("installed binary = %q, %v", data, err)
	}
	launcherData, err := os.ReadFile(installed.Launcher)
	if err != nil {
		t.Fatal(err)
	}
	launcherText := string(launcherData)
	if !strings.Contains(launcherText, binaryName) ||
		!strings.Contains(launcherText, "user-rom.sfc") ||
		!strings.Contains(launcherText, "utils") {
		t.Fatalf("unexpected launcher:\n%s", launcherText)
	}
	if len(installed.Libraries) != 1 {
		t.Fatalf("installed libraries = %v", installed.Libraries)
	}
	if data, err := os.ReadFile(filepath.Join(root, libraryName)); err != nil || string(data) != "library" {
		t.Fatalf("installed library = %q, %v", data, err)
	}
}

func TestInstallPlayableRejectsPathsOutsideDestination(t *testing.T) {
	base := t.TempDir()
	outside := t.TempDir()
	binary := filepath.Join(base, "game")
	rom := filepath.Join(base, "rom.sfc")
	_ = os.WriteFile(binary, []byte("binary"), 0o755)
	_ = os.WriteFile(rom, []byte("rom"), 0o600)
	_, err := InstallPlayable(InstallOptions{
		BinaryPath: binary, ROMPath: rom,
		ProjectRoot: outside, DestinationDir: base,
	})
	if err == nil || !strings.Contains(err.Error(), "must be inside") {
		t.Fatalf("error = %v", err)
	}
}
