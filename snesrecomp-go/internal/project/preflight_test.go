package project

import (
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// newPreflightProject writes the smallest project the scan accepts: a manifest
// that does NOT ask for SDL3, so these cases exercise the project-side inputs
// without depending on whether the machine running the tests has SDL3.
func newPreflightProject(t *testing.T) (root, romPath string) {
	t.Helper()
	root = t.TempDir()
	writeTestFile(t, filepath.Join(root, ManifestFileName), `
name = TestGame
source = src/main.c
`)
	romPath = filepath.Join(root, "game.sfc")
	writeTestFile(t, romPath, strings.Repeat("r", 1024))
	return root, romPath
}

func preflightCheckNamed(checks []PreflightCheck, name string) (PreflightCheck, bool) {
	for _, check := range checks {
		if check.Name == name {
			return check, true
		}
	}
	return PreflightCheck{}, false
}

func TestPreflightPasses(t *testing.T) {
	root, romPath := newPreflightProject(t)
	paths := DefaultPaths(root)
	paths.ROM = romPath
	checks := Preflight(PreflightOptions{
		Paths: paths, OutputDir: filepath.Join(root, "out"),
	})
	if failures := PreflightFailures(checks); len(failures) != 0 {
		t.Fatalf("unexpected failures: %+v", failures)
	}
	var report strings.Builder
	if err := WritePreflightReport(&report, checks); err != nil {
		t.Fatalf("report: %v", err)
	}
	if !strings.Contains(report.String(), "Dependency scan passed.") {
		t.Fatalf("report:\n%s", report.String())
	}
	// A manifest without sdl3 = true must not make the scan depend on SDL3
	// being installed on the machine running the tests.
	if _, found := preflightCheckNamed(checks, "SDL3 development files"); found {
		t.Fatal("SDL3 checked for a manifest that does not use it")
	}
	// The output folder is probed by writing, and the probe must not survive.
	entries, err := os.ReadDir(filepath.Join(root, "out"))
	if err != nil || len(entries) != 0 {
		t.Fatalf("probe left behind: %v %v", entries, err)
	}
}

func TestPreflightReportsMissingROM(t *testing.T) {
	root, romPath := newPreflightProject(t)
	if err := os.Remove(romPath); err != nil {
		t.Fatal(err)
	}
	paths := DefaultPaths(root)
	paths.ROM = romPath
	checks := Preflight(PreflightOptions{Paths: paths})

	rom, found := preflightCheckNamed(checks, "game ROM")
	if !found || rom.Status != PreflightFail {
		t.Fatalf("game ROM check: %+v found=%v", rom, found)
	}
	if rom.Remedy == "" {
		t.Fatal("a failing check must carry a remedy")
	}
	var report strings.Builder
	err := WritePreflightReport(&report, checks)
	if err == nil {
		t.Fatal("failing scan reported success")
	}
	// The error names the check, and the report explains what to do -- the
	// whole point is that neither is a linker message.
	if !strings.Contains(err.Error(), "game ROM") {
		t.Fatalf("error: %v", err)
	}
	if !strings.Contains(report.String(), rom.Remedy) ||
		!strings.Contains(report.String(), "the build was not started") {
		t.Fatalf("report:\n%s", report.String())
	}
}

func TestPreflightRejectsDirectoryAsROM(t *testing.T) {
	root, romPath := newPreflightProject(t)
	if err := os.Remove(romPath); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(romPath, 0o755); err != nil {
		t.Fatal(err)
	}
	paths := DefaultPaths(root)
	paths.ROM = romPath
	rom, _ := preflightCheckNamed(Preflight(PreflightOptions{Paths: paths}), "game ROM")
	if rom.Status != PreflightFail {
		t.Fatalf("directory accepted as ROM: %+v", rom)
	}
}

func TestPreflightReportsMissingManifest(t *testing.T) {
	root := t.TempDir()
	checks := Preflight(PreflightOptions{Paths: DefaultPaths(root)})
	manifest, found := preflightCheckNamed(checks, ManifestFileName)
	if !found || manifest.Status != PreflightFail {
		t.Fatalf("manifest check: %+v found=%v", manifest, found)
	}
}

func TestPreflightReportsUnwritableOutputDir(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX mode bits do not gate writes the same way on Windows")
	}
	if os.Geteuid() == 0 {
		t.Skip("root ignores the mode bits this case relies on")
	}
	root, romPath := newPreflightProject(t)
	locked := filepath.Join(root, "locked")
	if err := os.MkdirAll(locked, 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(locked, 0o755) })
	paths := DefaultPaths(root)
	paths.ROM = romPath
	output, found := preflightCheckNamed(
		Preflight(PreflightOptions{Paths: paths, OutputDir: locked}), "output folder")
	if !found || output.Status != PreflightFail {
		t.Fatalf("read-only output dir accepted: %+v found=%v", output, found)
	}
}

func TestSdl3InstallHintNamesTheArchitecture(t *testing.T) {
	// The reported machine HAD SDL3 -- just not for its own architecture -- so
	// a bare "install SDL3" would have read as already done.
	if runtime.GOOS == "linux" &&
		!strings.Contains(sdl3InstallHint(""), runtime.GOARCH) {
		t.Fatalf("hint omits the architecture: %s", sdl3InstallHint(""))
	}
	if hint := sdl3InstallHint("x86_64-windows-gnu"); !strings.Contains(hint, "sdl stage") {
		t.Fatalf("cross hint: %s", hint)
	}
}

func TestHumanBytes(t *testing.T) {
	for input, want := range map[int64]string{
		512: "512 B", 1024: "1.0 KiB", 1024 * 1024: "1.0 MiB",
	} {
		if got := humanBytes(input); got != want {
			t.Errorf("humanBytes(%d) = %s, want %s", input, got, want)
		}
	}
}
