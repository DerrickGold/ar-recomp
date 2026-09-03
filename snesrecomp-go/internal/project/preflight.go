package project

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// Preflight is the cheap dependency scan a long build should run first. Every
// check here answers a question the build itself only answers much later: the
// SDL3 one in particular used to surface as `ld.lld: ... is incompatible with
// elf64-x86-64` after a full regeneration and a 44-second compile, on a host
// where it was knowable in milliseconds. Nothing in here compiles, downloads,
// or writes anything lasting, so it is safe to run on every build.

type PreflightStatus string

const (
	PreflightOK   PreflightStatus = "ok"
	PreflightWarn PreflightStatus = "warn"
	PreflightFail PreflightStatus = "fail"
)

// PreflightCheck is one dependency and what was found. Remedy is the sentence a
// user can act on, and is set only when the check did not pass.
type PreflightCheck struct {
	Name   string
	Status PreflightStatus
	Detail string
	Remedy string
}

type PreflightOptions struct {
	Paths
	ManifestPath string // defaults to <root>/snesbuild.ini
	Target       string // Zig target triple; empty scans for the host
	OutputDir    string // playable install destination; empty skips the check
}

// Preflight runs the scan and returns one entry per dependency, in the order a
// build consumes them.
func Preflight(options PreflightOptions) []PreflightCheck {
	paths, err := options.Paths.Resolve()
	if err != nil {
		return []PreflightCheck{{
			Name: "project root", Status: PreflightFail,
			Detail: err.Error(),
			Remedy: "point --root at the game project directory",
		}}
	}
	manifestPath := options.ManifestPath
	if manifestPath == "" {
		manifestPath = filepath.Join(paths.Root, ManifestFileName)
	}

	var checks []PreflightCheck
	manifest, manifestErr := LoadManifest(manifestPath)
	if manifestErr != nil {
		checks = append(checks, PreflightCheck{
			Name: ManifestFileName, Status: PreflightFail,
			Detail: manifestErr.Error(),
			Remedy: "run the builder from a complete project checkout",
		})
	} else {
		checks = append(checks, PreflightCheck{
			Name: ManifestFileName, Status: PreflightOK,
			Detail: fmt.Sprintf("%d game sources (%s)", len(manifest.Sources), manifest.Name),
		})
	}

	checks = append(checks, preflightROM(paths.ROM))
	if options.OutputDir != "" {
		checks = append(checks, preflightWritableDir(options.OutputDir))
	}
	if manifestErr == nil && manifest.UseSDL3 {
		checks = append(checks, PreflightSDL3(paths, options.Target))
	}
	return checks
}

func preflightROM(romPath string) PreflightCheck {
	const name = "game ROM"
	if romPath == "" {
		return PreflightCheck{Name: name, Status: PreflightFail,
			Detail: "no ROM selected",
			Remedy: "choose the game ROM before building"}
	}
	info, err := os.Stat(romPath)
	switch {
	case err != nil:
		return PreflightCheck{Name: name, Status: PreflightFail,
			Detail: err.Error(),
			Remedy: "choose a readable ROM file"}
	case info.IsDir() || !info.Mode().IsRegular():
		return PreflightCheck{Name: name, Status: PreflightFail,
			Detail: romPath + " is not a regular file",
			Remedy: "choose the ROM file itself, not a folder"}
	case info.Size() == 0:
		return PreflightCheck{Name: name, Status: PreflightFail,
			Detail: romPath + " is empty",
			Remedy: "choose a complete ROM dump"}
	}
	return PreflightCheck{Name: name, Status: PreflightOK,
		Detail: fmt.Sprintf("%s (%s)", romPath, humanBytes(info.Size()))}
}

// preflightWritableDir proves the install destination accepts a write rather
// than inferring it from permission bits, which read wrong on network shares,
// read-only mounts, and a bundle opened from a disk image.
func preflightWritableDir(dir string) PreflightCheck {
	const name = "output folder"
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return PreflightCheck{Name: name, Status: PreflightFail,
			Detail: err.Error(),
			Remedy: "choose a folder you can write to"}
	}
	probe, err := os.CreateTemp(dir, ".snesbuild-write-probe-*")
	if err != nil {
		return PreflightCheck{Name: name, Status: PreflightFail,
			Detail: fmt.Sprintf("%s is not writable (%v)", dir, err),
			Remedy: "choose a folder you can write to, or copy the bundle out of a read-only location"}
	}
	probeName := probe.Name()
	probe.Close()
	os.Remove(probeName)
	return PreflightCheck{Name: name, Status: PreflightOK, Detail: dir + " (writable)"}
}

// PreflightSDL3 is the SDL3 half of the scan on its own, for callers that
// already report the other inputs their own way (`snesbuild doctor`). Target is
// a Zig triple, or empty for the host.
func PreflightSDL3(paths Paths, target string) PreflightCheck {
	const name = "SDL3 development files"
	includeDir, libDir, bundled, err := resolveSDL3(
		HermeticOptions{Paths: paths, Target: target})
	if err != nil {
		return PreflightCheck{Name: name, Status: PreflightFail,
			Detail: err.Error(), Remedy: sdl3InstallHint(target)}
	}
	detail := fmt.Sprintf("headers %s, libraries %s", includeDir, libDir)
	if bundled {
		detail += " (bundled)"
	}
	return PreflightCheck{Name: name, Status: PreflightOK, Detail: detail}
}

// sdl3InstallHint names the package rather than the concept: the reported
// failure was a machine that HAD SDL3, just not for its own architecture, so
// "install SDL3" on its own would have read as already done.
func sdl3InstallHint(target string) string {
	if target != "" {
		return fmt.Sprintf("run `snesbuild sdl stage --target %s`, "+
			"or pass --sdl-include and --sdl-lib", target)
	}
	switch runtime.GOOS {
	case "darwin":
		return "install SDL3 with `brew install sdl3`, or pass --sdl-include and --sdl-lib"
	case "linux":
		return fmt.Sprintf(
			"install the %s SDL3 development package "+
				"(`sudo dnf install SDL3-devel`, `sudo apt install libsdl3-dev`, "+
				"`sudo pacman -S sdl3`), or pass --sdl-include and --sdl-lib",
			runtime.GOARCH)
	}
	return "install the SDL3 development files, or pass --sdl-include and --sdl-lib"
}

// PreflightFailures returns only the checks that block a build.
func PreflightFailures(checks []PreflightCheck) []PreflightCheck {
	var failures []PreflightCheck
	for _, check := range checks {
		if check.Status == PreflightFail {
			failures = append(failures, check)
		}
	}
	return failures
}

// WritePreflightReport renders the scan and returns a non-nil error when any
// check failed, so a caller can hand the result straight back and stop.
func WritePreflightReport(writer io.Writer, checks []PreflightCheck) error {
	fmt.Fprintf(writer, "Dependency scan (%s/%s)\n", runtime.GOOS, runtime.GOARCH)
	width := 0
	for _, check := range checks {
		if len(check.Name) > width {
			width = len(check.Name)
		}
	}
	for _, check := range checks {
		fmt.Fprintf(writer, "  %-4s  %-*s  %s\n",
			check.Status, width, check.Name, check.Detail)
		if check.Remedy != "" {
			fmt.Fprintf(writer, "  %-4s  %-*s  -> %s\n", "", width, "", check.Remedy)
		}
	}
	failures := PreflightFailures(checks)
	if len(failures) == 0 {
		fmt.Fprintln(writer, "Dependency scan passed.")
		return nil
	}
	names := make([]string, 0, len(failures))
	for _, failure := range failures {
		names = append(names, failure.Name)
	}
	fmt.Fprintf(writer, "Dependency scan failed; the build was not started.\n")
	return fmt.Errorf("dependency scan failed: %s", strings.Join(names, ", "))
}

func humanBytes(size int64) string {
	const unit = 1024
	if size < unit {
		return fmt.Sprintf("%d B", size)
	}
	value, exponent := float64(size)/unit, 0
	for value >= unit && exponent < 3 {
		value /= unit
		exponent++
	}
	return fmt.Sprintf("%.1f %ciB", value, "KMGT"[exponent])
}
