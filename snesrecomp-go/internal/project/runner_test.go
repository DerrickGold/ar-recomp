package project

import (
	"path/filepath"
	"strings"
	"testing"
)

func TestResolveRunner(t *testing.T) {
	toolchain := filepath.FromSlash("/project/snesrecomp-go")
	defaultRunner, err := ResolveRunner(toolchain, "")
	if err != nil {
		t.Fatal(err)
	}
	if RunnerDefault != RunnerNext {
		t.Fatalf("default runner = %q, want %q", RunnerDefault, RunnerNext)
	}
	if defaultRunner.Name != RunnerNext || defaultRunner.LegacyFallback {
		t.Fatalf("default runner: %+v", defaultRunner)
	}
	if defaultRunner.Directory != filepath.Join(toolchain, "runtime-next") {
		t.Fatalf("default runner directory: %s", defaultRunner.Directory)
	}

	legacy, err := ResolveRunner(toolchain, " LEGACY ")
	if err != nil {
		t.Fatal(err)
	}
	if legacy.Name != RunnerLegacy || legacy.LegacyFallback {
		t.Fatalf("legacy: %+v", legacy)
	}
	if legacy.Directory != filepath.Join(toolchain, "runtime") {
		t.Fatalf("legacy directory: %s", legacy.Directory)
	}

	next, err := ResolveRunner(toolchain, " NEXT ")
	if err != nil {
		t.Fatal(err)
	}
	if next.Name != RunnerNext || next.LegacyFallback {
		t.Fatalf("next: %+v", next)
	}
	if next.Directory != filepath.Join(toolchain, "runtime-next") {
		t.Fatalf("next directory: %s", next.Directory)
	}

	if _, err := ResolveRunner(toolchain, "fast"); err == nil || !strings.Contains(err.Error(), "legacy or next") {
		t.Fatalf("invalid runner error: %v", err)
	}
}

func TestHermeticOutputDirSeparatesRunnerCaches(t *testing.T) {
	buildDir := filepath.FromSlash("/project/build")
	legacy, err := HermeticOutputDir(buildDir, "", RunnerLegacy)
	if err != nil {
		t.Fatal(err)
	}
	if legacy != filepath.Join(buildDir, "hermetic") {
		t.Fatalf("legacy output: %s", legacy)
	}
	next, err := HermeticOutputDir(buildDir, "x86_64-windows-gnu", RunnerNext)
	if err != nil {
		t.Fatal(err)
	}
	if next != filepath.Join(buildDir, "hermetic-next", "x86_64-windows-gnu") {
		t.Fatalf("next output: %s", next)
	}
}
