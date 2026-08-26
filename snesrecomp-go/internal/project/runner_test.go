package project

import (
	"path/filepath"
	"testing"
)

func TestRunnerDirectory(t *testing.T) {
	toolchain := filepath.FromSlash("/project/snesrecomp-go")
	if got := RunnerDirectory(toolchain); got != filepath.Join(toolchain, "runtime-next") {
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
