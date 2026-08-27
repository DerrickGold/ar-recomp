package project

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

const RunnerName = "portable"

// RunnerDirectory is the single independently authored runtime shipped by the
// toolchain. The legacy runner was retired after the parity cutover.
func RunnerDirectory(toolchainDir string) string {
	return filepath.Join(toolchainDir, "runtime")
}

// RuntimeTargetForPlatform returns the canonical Zig target directory used by
// a vended runner SDK. Archives are never treated as universal: the operating
// system, CPU architecture, object format, and ABI all follow this key.
func RuntimeTargetForPlatform(goos, goarch string) (string, error) {
	architecture := map[string]string{
		"amd64": "x86_64",
		"arm64": "aarch64",
	}[goarch]
	if architecture == "" {
		return "", fmt.Errorf("no vended runner target for architecture %s", goarch)
	}
	operatingSystem := map[string]string{
		"darwin":  "macos",
		"linux":   "linux-gnu",
		"windows": "windows-gnu",
	}[goos]
	if operatingSystem == "" {
		return "", fmt.Errorf("no vended runner target for operating system %s", goos)
	}
	return architecture + "-" + operatingSystem, nil
}

// RuntimeArchiveTarget converts an empty (native) Zig target to the same
// canonical spelling used by release packaging. Explicit cross targets retain
// their Zig spelling, after rejecting values that could escape runtime/lib.
func RuntimeArchiveTarget(target string) (string, error) {
	if target == "" {
		return RuntimeTargetForPlatform(runtime.GOOS, runtime.GOARCH)
	}
	if strings.ContainsAny(target, `/\\`) || target == "." ||
		strings.Contains(target, "..") {
		return "", fmt.Errorf("invalid runner archive target %q", target)
	}
	fields := strings.Split(target, "-")
	if len(fields) < 2 {
		return "", fmt.Errorf("invalid Zig target %q", target)
	}
	switch fields[1] {
	case "macos", "linux", "windows":
	default:
		return "", fmt.Errorf("unsupported runner archive target %q", target)
	}
	return target, nil
}

// VendedRuntimeArchivePath is the deterministic SDK location selected by a
// hermetic build. The archive name follows the target object format.
func VendedRuntimeArchivePath(runtimeDir, target string) (string, error) {
	archiveTarget, err := RuntimeArchiveTarget(target)
	if err != nil {
		return "", err
	}
	return filepath.Join(runtimeDir, "lib", archiveTarget,
		runtimeArchiveName(TargetOS(archiveTarget))), nil
}

type runtimeSelection struct {
	Archive        string
	PublicIncludes []string
	SourceManifest RunnerManifest
}

// selectRuntimeInputs prefers a matching vended SDK archive. Source manifests
// remain a development and unsupported-target fallback, but a present corrupt
// archive is an error instead of silently changing the build model.
func selectRuntimeInputs(runtimeDir, target string) (runtimeSelection, error) {
	archivePath, targetErr := VendedRuntimeArchivePath(runtimeDir, target)
	if targetErr == nil {
		if info, err := os.Stat(archivePath); err == nil {
			if !info.Mode().IsRegular() || info.Size() == 0 {
				return runtimeSelection{}, fmt.Errorf(
					"vended runner archive is invalid: %s", archivePath)
			}
			includeDir := filepath.Join(runtimeDir, "include")
			if info, err := os.Stat(includeDir); err != nil || !info.IsDir() {
				return runtimeSelection{}, fmt.Errorf(
					"vended runner headers are missing: %s", includeDir)
			}
			return runtimeSelection{
				Archive: archivePath, PublicIncludes: []string{includeDir},
			}, nil
		} else if !os.IsNotExist(err) {
			return runtimeSelection{}, err
		}
	}

	manifest, err := LoadRunnerManifest(runtimeDir)
	if err != nil {
		if targetErr != nil {
			return runtimeSelection{}, fmt.Errorf(
				"runner has neither a supported vended archive nor source fallback: %w",
				targetErr)
		}
		return runtimeSelection{}, fmt.Errorf(
			"runner archive %s is missing and the source fallback is unavailable: %w",
			archivePath, err)
	}
	return runtimeSelection{
		PublicIncludes: manifest.PublicIncludes, SourceManifest: manifest,
	}, nil
}

// HermeticOutputDir returns the canonical target-specific hermetic build tree.
func HermeticOutputDir(buildDir, target string) string {
	output := filepath.Join(buildDir, "hermetic")
	if target != "" {
		output = filepath.Join(output, target)
	}
	return output
}
