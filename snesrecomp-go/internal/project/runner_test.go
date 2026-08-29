package project

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRunnerDirectory(t *testing.T) {
	toolchain := filepath.FromSlash("/project/snesrecomp-go")
	if got := RunnerDirectory(toolchain); got != filepath.Join(toolchain, "runtime") {
		t.Fatalf("runner directory = %s", got)
	}
}

func TestHermeticOutputDir(t *testing.T) {
	buildDir := filepath.FromSlash("/project/build")
	if got := HermeticOutputDir(buildDir, ""); got != filepath.Join(buildDir, "hermetic") {
		t.Fatalf("native output: %s", got)
	}
	if got := HermeticOutputDir(buildDir, "x86_64-windows-gnu"); got != filepath.Join(buildDir, "hermetic", "x86_64-windows-gnu") {
		t.Fatalf("cross output: %s", got)
	}
}

func TestRuntimeTargetForPlatform(t *testing.T) {
	cases := map[string]string{
		"darwin/arm64":  "aarch64-macos",
		"darwin/amd64":  "x86_64-macos",
		"linux/arm64":   "aarch64-linux-gnu",
		"linux/amd64":   "x86_64-linux-gnu",
		"windows/arm64": "aarch64-windows-gnu",
		"windows/amd64": "x86_64-windows-gnu",
	}
	for platform, want := range cases {
		fields := strings.Split(platform, "/")
		got, err := RuntimeTargetForPlatform(fields[0], fields[1])
		if err != nil || got != want {
			t.Errorf("RuntimeTargetForPlatform(%s) = %q, %v; want %q",
				platform, got, err, want)
		}
	}
	if _, err := RuntimeTargetForPlatform("plan9", "amd64"); err == nil {
		t.Fatal("unsupported operating system accepted")
	}
	if _, err := RuntimeArchiveTarget("../x86_64-linux-gnu"); err == nil {
		t.Fatal("archive target path traversal accepted")
	}
}

func TestRuntimeSelectionPrefersTargetArchiveWithoutSources(t *testing.T) {
	runtimeDir := t.TempDir()
	writeTestFile(t, filepath.Join(runtimeDir, "include", "snesrecomp", "runner.h"),
		"/* public */\n")
	archive, err := VendedRuntimeArchivePath(runtimeDir, "x86_64-windows-gnu")
	if err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, archive, "archive")

	selection, err := selectRuntimeInputs(runtimeDir, "x86_64-windows-gnu")
	if err != nil {
		t.Fatal(err)
	}
	if selection.Archive != archive {
		t.Fatalf("archive = %q, want %q", selection.Archive, archive)
	}
	if len(selection.SourceManifest.Sources) != 0 {
		t.Fatal("vended selection unexpectedly loaded runner sources")
	}
	if len(selection.PublicIncludes) != 1 ||
		selection.PublicIncludes[0] != filepath.Join(runtimeDir, "include") {
		t.Fatalf("public includes = %#v", selection.PublicIncludes)
	}
}

func TestRuntimeSelectionFallsBackToSourceManifest(t *testing.T) {
	runtimeDir := t.TempDir()
	writeTestFile(t, filepath.Join(runtimeDir, "src", "runner.c"), "int runner;\n")
	writeTestFile(t, filepath.Join(runtimeDir, "include", "runner.h"), "/* public */\n")
	writeTestFile(t, filepath.Join(runtimeDir, "runner.cmake"), `
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/runner.c
)
set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
)
`)
	selection, err := selectRuntimeInputs(runtimeDir, "x86_64-linux-gnu")
	if err != nil {
		t.Fatal(err)
	}
	if selection.Archive != "" || len(selection.SourceManifest.Sources) != 1 {
		t.Fatalf("source fallback selection = %#v", selection)
	}
}

func TestRuntimeSelectionRejectsCorruptVendedArchive(t *testing.T) {
	runtimeDir := t.TempDir()
	writeTestFile(t, filepath.Join(runtimeDir, "include", "runner.h"), "public")
	archive, err := VendedRuntimeArchivePath(runtimeDir, "x86_64-linux-gnu")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(archive), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(archive, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := selectRuntimeInputs(runtimeDir, "x86_64-linux-gnu"); err == nil {
		t.Fatal("zero-byte vended archive accepted")
	}
}

func TestRuntimeArchiveFlagsAreGameIndependentAndSanitized(t *testing.T) {
	manifest := RunnerManifest{
		PublicIncludes:  []string{filepath.FromSlash("/sdk/runtime/include")},
		PrivateIncludes: []string{filepath.FromSlash("/sdk/runtime/src")},
	}
	args := runtimeCompileArgs(filepath.FromSlash("/sdk/runtime"),
		filepath.FromSlash("/tools/zig/zig"), "x86_64-windows-gnu", "-O2",
		true, manifest)
	joined := strings.Join(args, "\x00")
	for _, required := range []string{
		"-DSNESRECOMP_ENABLE_SIMD=1",
		"-DSNESRECOMP_TRACE=0",
		"-DSNESRECOMP_WATCHDOG=0",
		"-gno-codeview-command-line",
		"-fdebug-compilation-dir=snesrecomp-runtime",
	} {
		if !strings.Contains(joined, required) {
			t.Errorf("runtime compile arguments omit %s: %#v", required, args)
		}
	}
	if strings.Contains(joined, "AR_SIM3D") {
		t.Fatal("game-specific define leaked into the runner build")
	}
}

func TestRuntimeArchiveUsesStableWindowsDebugObjectNames(t *testing.T) {
	runtimeDir := filepath.FromSlash("/checkout/snesrecomp-go/runtime")
	source := filepath.FromSlash("/checkout/snesrecomp-go/runtime/src/runner/runner.c")
	args := runtimeDebugObjectArgs(runtimeDir, "x86_64-windows-gnu", source)
	want := []string{
		"-Xclang", "-object-file-name",
		"-Xclang", "snesrecomp-runtime/src_runner_runner.c.obj",
	}
	if strings.Join(args, "\x00") != strings.Join(want, "\x00") {
		t.Fatalf("Windows debug object args = %#v, want %#v", args, want)
	}
	joined := strings.Join(args, "\x00")
	if strings.Contains(joined, "/checkout/") || strings.Contains(joined, `C:\\`) {
		t.Fatalf("Windows debug object name contains a host path: %#v", args)
	}
	if got := runtimeDebugObjectArgs(runtimeDir, "x86_64-linux-gnu", source); len(got) != 0 {
		t.Fatalf("non-Windows debug object args = %#v, want none", got)
	}
}
