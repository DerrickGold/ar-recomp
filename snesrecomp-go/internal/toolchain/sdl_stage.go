package toolchain

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/fsutil"
)

// TargetGo maps a Zig target triple ("x86_64-windows-gnu") to the GOOS/GOARCH
// pair the pin tables are keyed by. Only the architecture and OS fields
// matter; the ABI suffix is Zig's business.
func TargetGo(target string) (goos, goarch string, err error) {
	fields := strings.Split(target, "-")
	if len(fields) < 2 {
		return "", "", fmt.Errorf("not a Zig target triple: %q", target)
	}
	switch fields[0] {
	case "x86_64":
		goarch = "amd64"
	case "aarch64":
		goarch = "arm64"
	default:
		return "", "", fmt.Errorf("unsupported target architecture %q in %q", fields[0], target)
	}
	switch fields[1] {
	case "macos":
		goos = "darwin"
	case "windows", "linux":
		goos = fields[1]
	default:
		return "", "", fmt.Errorf("unsupported target OS %q in %q", fields[1], target)
	}
	return goos, goarch, nil
}

// StageSDL3 downloads the pinned SDL3 redistributable for a Zig target triple
// into cacheDir and stages it at stageDir as include/SDL3 + lib, the same
// layout the distribution bundle ships. A cross build links against this, so
// the check exercises the exact headers and import library a real user's
// machine would use rather than whatever the host happens to have.
//
// Idempotent: an already-staged directory is left alone.
func StageSDL3(target, cacheDir, stageDir string, stdout io.Writer) error {
	if stdout == nil {
		stdout = io.Discard
	}
	goos, goarch, err := TargetGo(target)
	if err != nil {
		return err
	}
	if fsutil.DirectoryExists(filepath.Join(stageDir, "include", "SDL3")) &&
		fsutil.DirectoryExists(filepath.Join(stageDir, "lib")) {
		fmt.Fprintf(stdout, "sdl: %s already staged (%s)\n", target, stageDir)
		return nil
	}
	url, want, archive, kind, err := SDL3Pin(goos, goarch)
	if err != nil {
		return err
	}
	// The macOS framework lives in a .dmg, which needs 7zz to open (hdiutil is
	// blocked in some build environments -- see packaging/scripts). macOS is
	// also the one platform a developer on a Mac can already build natively,
	// so the cost of carrying that path here buys nothing.
	if kind != "mingw" && kind != "vc" {
		return fmt.Errorf("staging the %s SDL3 redistributable for %s is not supported here; "+
			"build natively for macOS, or pass --sdl-include/--sdl-lib", kind, target)
	}
	if err := os.MkdirAll(cacheDir, 0o755); err != nil {
		return err
	}
	archivePath := filepath.Join(cacheDir, archive)
	if !checksumMatches(archivePath, want) {
		fmt.Fprintf(stdout, "sdl: downloading %s\n", url)
		if err := download(url, archivePath, stdout); err != nil {
			return err
		}
	}
	if !checksumMatches(archivePath, want) {
		return fmt.Errorf("checksum mismatch for %s (expected %s)", archivePath, want)
	}
	unpacked := filepath.Join(cacheDir, "sdl3-"+target+"-unpacked")
	if err := os.RemoveAll(unpacked); err != nil {
		return err
	}
	if err := os.MkdirAll(unpacked, 0o755); err != nil {
		return err
	}
	fmt.Fprintf(stdout, "sdl: checksum ok; extracting %s\n", archive)
	if err := extract(archivePath, unpacked); err != nil {
		return err
	}
	root := filepath.Join(unpacked, "SDL3-"+PinnedSDL3Version)
	headerRoot := filepath.Join(root, "include", "SDL3")
	type stagedFile struct{ from, to string }
	var files []stagedFile
	switch kind {
	case "mingw":
		// The MinGW archive currently contains x86 and x86_64 GNU ABI
		// subtrees. Keep the target mapping explicit rather than guessing one.
		abi := map[string]string{"amd64": "x86_64-w64-mingw32", "arm64": "aarch64-w64-mingw32"}[goarch]
		root = filepath.Join(root, abi)
		headerRoot = filepath.Join(root, "include", "SDL3")
		files = []stagedFile{
			{filepath.Join(root, "lib", "libSDL3.dll.a"), filepath.Join(stageDir, "lib", "libSDL3.dll.a")},
			{filepath.Join(root, "bin", "SDL3.dll"), filepath.Join(stageDir, "lib", "SDL3.dll")},
		}
	case "vc":
		arch := map[string]string{"amd64": "x64", "arm64": "arm64"}[goarch]
		libRoot := filepath.Join(root, "lib", arch)
		files = []stagedFile{
			{filepath.Join(libRoot, "SDL3.lib"), filepath.Join(stageDir, "lib", "SDL3.lib")},
			{filepath.Join(libRoot, "SDL3.dll"), filepath.Join(stageDir, "lib", "SDL3.dll")},
		}
	}
	if !fsutil.DirectoryExists(headerRoot) {
		return fmt.Errorf("%s has no SDL3 headers for %s (looked in %s)", archive, target, headerRoot)
	}
	if err := os.RemoveAll(stageDir); err != nil {
		return err
	}
	if err := copyTree(headerRoot,
		filepath.Join(stageDir, "include", "SDL3")); err != nil {
		return err
	}
	// Both files land in lib/: lld resolves -lSDL3 through either GNU's
	// libSDL3.dll.a or VC's SDL3.lib, and copySDLRuntime places the DLL beside
	// the built game.
	for _, item := range files {
		if err := copyFile(item.from, item.to); err != nil {
			return err
		}
	}
	if err := os.RemoveAll(unpacked); err != nil {
		return err
	}
	fmt.Fprintf(stdout, "sdl: SDL3 %s staged for %s (%s)\n", PinnedSDL3Version, target, stageDir)
	return nil
}

func copyFile(from, to string) error {
	data, err := os.ReadFile(from)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(to), 0o755); err != nil {
		return err
	}
	return os.WriteFile(to, data, 0o644)
}

func copyTree(from, to string) error {
	entries, err := os.ReadDir(from)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(to, 0o755); err != nil {
		return err
	}
	for _, entry := range entries {
		source, target := filepath.Join(from, entry.Name()), filepath.Join(to, entry.Name())
		if entry.IsDir() {
			if err := copyTree(source, target); err != nil {
				return err
			}
			continue
		}
		if err := copyFile(source, target); err != nil {
			return err
		}
	}
	return nil
}
