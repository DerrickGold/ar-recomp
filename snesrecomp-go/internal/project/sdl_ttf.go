package project

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/subprocess"
	"github.com/DerrickGold/snesrecomp-go/internal/toolchain"
)

// UsesSDL3Ttf keeps dependency reporting and build selection in agreement.
func (manifest Manifest) UsesSDL3Ttf() bool {
	return slices.Contains(manifest.Link, "-lSDL3_ttf")
}

func sdlIncludeParent(dir string) string {
	if strings.EqualFold(filepath.Base(dir), "SDL3") || strings.EqualFold(filepath.Base(dir), "SDL3_ttf") {
		return filepath.Dir(dir)
	}
	return dir
}

func sdlTtfCandidate(candidate sdlCandidate, target string) bool {
	if candidate.include == "" || candidate.lib == "" {
		return false
	}
	header, err := os.Stat(filepath.Join(sdlIncludeParent(candidate.include), "SDL3_ttf", "SDL_ttf.h"))
	if err != nil || !header.Mode().IsRegular() || header.Size() == 0 {
		return false
	}
	goarch := runtime.GOARCH
	if target != "" {
		_, arch, err := toolchain.TargetGo(target)
		if err != nil {
			return false
		}
		goarch = arch
	}
	// Cross builds must not compare the DLL architecture with the host. The
	// pinned staging adapter owns target identity; still require link + runtime.
	if TargetOS(target) == "windows" {
		regular := func(name string) bool {
			f, err := os.Stat(filepath.Join(candidate.lib, name))
			return err == nil && f.Mode().IsRegular() && f.Size() > 0
		}
		okay, known := sdlLibraryArchitectureMatchesTarget(filepath.Join(candidate.lib, "SDL3_ttf.dll"), goarch)
		return (!known || okay) && regular("SDL3_ttf.dll") && (regular("SDL3_ttf.lib") || regular("libSDL3_ttf.dll.a"))
	}
	linkName := "libSDL3_ttf.so"
	if TargetOS(target) == "darwin" {
		linkName = "libSDL3_ttf.dylib"
	}
	linkPath := filepath.Join(candidate.lib, linkName)
	link, err := os.Stat(linkPath)
	if err != nil || !link.Mode().IsRegular() || link.Size() == 0 {
		return false
	}
	okay, known := sdlLibraryArchitectureMatchesTarget(linkPath, goarch)
	return okay || !known
}

// resolveSDL3Ttf shares the early preflight/build boundary. A native system
// SDL3 and SDL3_ttf may have separate package prefixes (notably Homebrew).
// Bundled, explicit and cross-target SDKs are closed: never fill their gaps
// from a developer machine and accidentally ship a host-library dependency.
func resolveSDL3Ttf(options HermeticOptions, allowSystem bool) (string, string, error) {
	candidate := sdlCandidate{sdlIncludeParent(options.SDLIncludeDir), options.SDLLibDir}
	if sdlTtfCandidate(candidate, options.Target) {
		return candidate.include, candidate.lib, nil
	}
	if !allowSystem || options.Target != "" {
		return "", "", fmt.Errorf("SDL3_ttf headers and link/runtime libraries are missing from the selected SDK (%s, %s); use a complete SDL3 + SDL3_ttf SDK for this target", candidate.include, candidate.lib)
	}
	if pkg, err := exec.LookPath("pkg-config"); err == nil {
		directory := func(name string) string {
			data, err := subprocess.Command(pkg, "--variable="+name, "sdl3-ttf").Output()
			if err != nil {
				return ""
			}
			return strings.TrimSpace(string(data))
		}
		candidate = sdlCandidate{sdlIncludeParent(directory("includedir")), directory("libdir")}
		if sdlTtfCandidate(candidate, "") {
			return candidate.include, candidate.lib, nil
		}
	}
	candidates := []sdlCandidate{
		{"/opt/homebrew/opt/sdl3_ttf/include", "/opt/homebrew/opt/sdl3_ttf/lib"},
		{"/usr/local/opt/sdl3_ttf/include", "/usr/local/opt/sdl3_ttf/lib"},
		{"/opt/homebrew/include", "/opt/homebrew/lib"},
	}
	if runtime.GOOS == "linux" {
		candidates = append(candidates, sdlCandidate{"/usr/local/include", "/usr/local/lib64"}, sdlCandidate{"/usr/include", "/usr/lib64"})
		if arch := gnuMultiarchDir(); arch != "" {
			candidates = append(candidates, sdlCandidate{"/usr/local/include", filepath.Join("/usr/local/lib", arch)}, sdlCandidate{"/usr/include", filepath.Join("/usr/lib", arch)})
		}
	}
	candidates = append(candidates, sdlCandidate{"/usr/local/include", "/usr/local/lib"}, sdlCandidate{"/usr/include", "/usr/lib"})
	for _, candidate := range candidates {
		if sdlTtfCandidate(candidate, "") {
			return candidate.include, candidate.lib, nil
		}
	}
	return "", "", fmt.Errorf("SDL3_ttf development files for %s/%s not found; install the SDL3_ttf development package or select a complete SDK with --sdl-include/--sdl-lib", runtime.GOOS, runtime.GOARCH)
}

func PreflightSDL3Ttf(paths Paths, target string) PreflightCheck {
	check := PreflightCheck{Name: "SDL3_ttf development files", Status: PreflightFail,
		Remedy: "install SDL3_ttf development files, or use a complete SDL3 + SDL3_ttf bundle for this target"}
	include, lib, bundled, err := resolveSDL3(HermeticOptions{Paths: paths, Target: target})
	if err == nil {
		include, lib, err = resolveSDL3Ttf(HermeticOptions{Paths: paths, Target: target, SDLIncludeDir: include, SDLLibDir: lib}, !bundled)
	}
	if err != nil {
		check.Detail = err.Error()
		return check
	}
	check.Status, check.Remedy = PreflightOK, ""
	check.Detail = fmt.Sprintf("headers %s, libraries %s", include, lib)
	return check
}
