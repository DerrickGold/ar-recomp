package project

import (
	"fmt"
	"path/filepath"
	"strings"
)

const (
	RunnerLegacy = "legacy"
	RunnerNext   = "next"
)

// RunnerInfo is the build-system identity and filesystem location of one
// selectable runner. LegacyFallback is a disclosure about the complete game
// manifest, not about the license of independently authored files.
type RunnerInfo struct {
	Name           string
	Directory      string
	LegacyFallback bool
}

// ResolveRunner validates a user-facing runner name. The empty value remains
// an alias for legacy so existing callers and release builds do not change.
func ResolveRunner(toolchainDir, name string) (RunnerInfo, error) {
	normalized := strings.ToLower(strings.TrimSpace(name))
	if normalized == "" {
		normalized = RunnerLegacy
	}
	switch normalized {
	case RunnerLegacy:
		return RunnerInfo{
			Name:      RunnerLegacy,
			Directory: filepath.Join(toolchainDir, "runtime"),
		}, nil
	case RunnerNext:
		return RunnerInfo{
			Name:      RunnerNext,
			Directory: filepath.Join(toolchainDir, "runtime-next"),
		}, nil
	default:
		return RunnerInfo{}, fmt.Errorf("unknown runner %q (expected %s or %s)", name, RunnerLegacy, RunnerNext)
	}
}

// HermeticOutputDir isolates object caches for incompatible runner source
// sets while preserving the historical path for the default legacy build.
func HermeticOutputDir(buildDir, target, runner string) (string, error) {
	selected, err := ResolveRunner("", runner)
	if err != nil {
		return "", err
	}
	directoryName := "hermetic"
	if selected.Name == RunnerNext {
		directoryName = "hermetic-next"
	}
	output := filepath.Join(buildDir, directoryName)
	if target != "" {
		output = filepath.Join(output, target)
	}
	return output, nil
}
