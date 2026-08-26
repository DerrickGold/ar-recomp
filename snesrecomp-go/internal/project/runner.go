package project

import (
	"path/filepath"
)

const RunnerName = "next"

// RunnerDirectory is the single independently authored runtime shipped by the
// toolchain. The legacy runner was retired after the parity cutover.
func RunnerDirectory(toolchainDir string) string {
	return filepath.Join(toolchainDir, "runtime-next")
}

// HermeticOutputDir returns the canonical target-specific hermetic build tree.
func HermeticOutputDir(buildDir, target string) string {
	output := filepath.Join(buildDir, "hermetic")
	if target != "" {
		output = filepath.Join(output, target)
	}
	return output
}
