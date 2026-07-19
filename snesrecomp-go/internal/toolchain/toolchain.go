// Package toolchain locates and provisions the pinned hermetic C toolchain
// (Zig) used by snesbuild's CMake-free build path. The pin keeps user-side
// builds reproducible: one known compiler version per platform, verified by
// checksum, extracted into the project's build tree without touching PATH or
// package-manager state.
package toolchain

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// PinnedZigVersion is the single Zig release every platform bundle carries.
const PinnedZigVersion = "0.16.0"

type pin struct {
	Archive string // release archive file name
	SHA256  string
}

// Pinned archives for ziglang.org/download/<version>/<archive>, keyed by
// GOOS/GOARCH. Checksums come from the official release index.
var pinnedZig = map[string]pin{
	"darwin/arm64":  {"zig-aarch64-macos-0.16.0.tar.xz", "b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489"},
	"darwin/amd64":  {"zig-x86_64-macos-0.16.0.tar.xz", "0387557ed1877bc6a2e1802c8391953baddba76081876301c522f52977b52ba7"},
	"linux/amd64":   {"zig-x86_64-linux-0.16.0.tar.xz", "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00"},
	"linux/arm64":   {"zig-aarch64-linux-0.16.0.tar.xz", "ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17"},
	"windows/amd64": {"zig-x86_64-windows-0.16.0.zip", "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e"},
	"windows/arm64": {"zig-aarch64-windows-0.16.0.zip", "aee38316ee4111717900f45dd3130145c39289e105541d737eb8c5ed653c78ef"},
}

// Zig describes a usable compiler and where it came from.
type Zig struct {
	Path    string
	Version string
	Source  string // "env", "cache", or "path"
}

// EnvOverride names the environment variable that force-selects a Zig binary.
const EnvOverride = "SNESBUILD_ZIG"

func hostPin() (pin, error) {
	return pinFor(runtime.GOOS, runtime.GOARCH)
}

func pinFor(goos, goarch string) (pin, error) {
	key := goos + "/" + goarch
	entry, ok := pinnedZig[key]
	if !ok {
		return pin{}, fmt.Errorf("no pinned Zig for platform %s", key)
	}
	return entry, nil
}

// Pin reports the pinned release for an arbitrary target platform. Used by
// the packaging pipeline to download and bundle the right toolchain archive
// from the same single source of truth the runtime lookup uses.
func Pin(goos, goarch string) (url, sha, archive string, err error) {
	entry, err := pinFor(goos, goarch)
	if err != nil {
		return "", "", "", err
	}
	return "https://ziglang.org/download/" + PinnedZigVersion + "/" + entry.Archive, entry.SHA256, entry.Archive, nil
}

// PinnedURL returns the download URL and checksum for the current host.
func PinnedURL() (url, sha string, err error) {
	entry, err := hostPin()
	if err != nil {
		return "", "", err
	}
	return "https://ziglang.org/download/" + PinnedZigVersion + "/" + entry.Archive, entry.SHA256, nil
}

func zigExecutableName() string {
	if runtime.GOOS == "windows" {
		return "zig.exe"
	}
	return "zig"
}

// cachedZigPath is where Fetch extracts the pinned toolchain inside cacheDir.
func cachedZigPath(cacheDir string) (string, error) {
	entry, err := hostPin()
	if err != nil {
		return "", err
	}
	release := strings.TrimSuffix(strings.TrimSuffix(entry.Archive, ".tar.xz"), ".zip")
	return filepath.Join(cacheDir, release, zigExecutableName()), nil
}

// Locate finds a usable Zig: the SNESBUILD_ZIG override first, then a
// toolchain bundled beside the snesbuild executable itself (the distribution
// bundle layout: <exe dir>/toolchain/zig-*/zig), then the project cache
// directory, then PATH. Every candidate must answer `zig version` before it
// is accepted.
func Locate(cacheDir string) (Zig, error) {
	if override := os.Getenv(EnvOverride); override != "" {
		version, err := probeVersion(override)
		if err != nil {
			return Zig{}, fmt.Errorf("%s=%s: %w", EnvOverride, override, err)
		}
		return Zig{Path: override, Version: version, Source: "env"}, nil
	}
	if bundled := bundledZigPath(); bundled != "" {
		if version, probeErr := probeVersion(bundled); probeErr == nil {
			return Zig{Path: bundled, Version: version, Source: "bundled"}, nil
		}
	}
	if cacheDir != "" {
		if cached, err := cachedZigPath(cacheDir); err == nil {
			if version, probeErr := probeVersion(cached); probeErr == nil {
				return Zig{Path: cached, Version: version, Source: "cache"}, nil
			}
		}
	}
	if fromPath, err := exec.LookPath(zigExecutableName()); err == nil {
		if version, probeErr := probeVersion(fromPath); probeErr == nil {
			return Zig{Path: fromPath, Version: version, Source: "path"}, nil
		}
	}
	url, _, _ := PinnedURL()
	return Zig{}, fmt.Errorf("no Zig toolchain found: set %s, run `snesbuild toolchain fetch`, or install zig on PATH (pinned release: %s)", EnvOverride, url)
}

// bundledZigPath looks for a toolchain shipped beside the running executable
// (distribution bundles place it at toolchain/zig-<target>-<version>/).
// Returns "" when there is none.
func bundledZigPath() string {
	executable, err := os.Executable()
	if err != nil {
		return ""
	}
	matches, _ := filepath.Glob(filepath.Join(filepath.Dir(executable), "toolchain", "zig-*", zigExecutableName()))
	if len(matches) == 0 {
		return ""
	}
	return matches[0]
}

func probeVersion(path string) (string, error) {
	output, err := exec.Command(path, "version").Output()
	if err != nil {
		return "", fmt.Errorf("not runnable: %w", err)
	}
	return strings.TrimSpace(string(output)), nil
}

// Fetch downloads the pinned Zig release into cacheDir, verifies its SHA-256
// against the embedded pin, and extracts it. Extraction uses archive/zip for
// .zip releases and the host `tar` for .tar.xz releases (present on modern
// macOS, Linux, and Windows). Idempotent: an already-working cached toolchain
// is left untouched.
func Fetch(cacheDir string, stdout io.Writer) (Zig, error) {
	if stdout == nil {
		stdout = io.Discard
	}
	cached, err := cachedZigPath(cacheDir)
	if err != nil {
		return Zig{}, err
	}
	if version, probeErr := probeVersion(cached); probeErr == nil {
		fmt.Fprintf(stdout, "toolchain: pinned Zig %s already present (%s)\n", version, cached)
		return Zig{Path: cached, Version: version, Source: "cache"}, nil
	}
	entry, err := hostPin()
	if err != nil {
		return Zig{}, err
	}
	if err := os.MkdirAll(cacheDir, 0o755); err != nil {
		return Zig{}, err
	}
	url, want, err := PinnedURL()
	if err != nil {
		return Zig{}, err
	}
	archivePath := filepath.Join(cacheDir, entry.Archive)
	if !checksumMatches(archivePath, want) {
		fmt.Fprintf(stdout, "toolchain: downloading %s\n", url)
		if err := download(url, archivePath); err != nil {
			return Zig{}, err
		}
	}
	if !checksumMatches(archivePath, want) {
		return Zig{}, fmt.Errorf("checksum mismatch for %s (expected %s)", archivePath, want)
	}
	fmt.Fprintf(stdout, "toolchain: checksum ok; extracting %s\n", entry.Archive)
	if err := extract(archivePath, cacheDir); err != nil {
		return Zig{}, err
	}
	version, err := probeVersion(cached)
	if err != nil {
		return Zig{}, fmt.Errorf("extracted toolchain is not runnable: %w", err)
	}
	fmt.Fprintf(stdout, "toolchain: Zig %s ready (%s)\n", version, cached)
	return Zig{Path: cached, Version: version, Source: "cache"}, nil
}

func checksumMatches(path, want string) bool {
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	defer file.Close()
	digest := sha256.New()
	if _, err := io.Copy(digest, file); err != nil {
		return false
	}
	return hex.EncodeToString(digest.Sum(nil)) == want
}

func download(url, destination string) error {
	response, err := http.Get(url)
	if err != nil {
		return fmt.Errorf("download %s: %w", url, err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("download %s: HTTP %s", url, response.Status)
	}
	file, err := os.Create(destination)
	if err != nil {
		return err
	}
	defer file.Close()
	if _, err := io.Copy(file, response.Body); err != nil {
		return fmt.Errorf("download %s: %w", url, err)
	}
	return nil
}

func extract(archivePath, destination string) error {
	if strings.HasSuffix(archivePath, ".zip") {
		return extractZip(archivePath, destination)
	}
	command := exec.Command("tar", "-xf", archivePath, "-C", destination)
	var stderr bytes.Buffer
	command.Stderr = &stderr
	if err := command.Run(); err != nil {
		return fmt.Errorf("extract %s: %w (%s)", archivePath, err, strings.TrimSpace(stderr.String()))
	}
	return nil
}

func extractZip(archivePath, destination string) error {
	reader, err := zip.OpenReader(archivePath)
	if err != nil {
		return err
	}
	defer reader.Close()
	for _, entry := range reader.File {
		target := filepath.Join(destination, filepath.FromSlash(entry.Name))
		if !strings.HasPrefix(target, filepath.Clean(destination)+string(os.PathSeparator)) {
			return fmt.Errorf("zip entry escapes destination: %s", entry.Name)
		}
		if entry.FileInfo().IsDir() {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		source, err := entry.Open()
		if err != nil {
			return err
		}
		file, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, entry.Mode())
		if err != nil {
			source.Close()
			return err
		}
		_, copyErr := io.Copy(file, source)
		source.Close()
		file.Close()
		if copyErr != nil {
			return copyErr
		}
	}
	return nil
}
