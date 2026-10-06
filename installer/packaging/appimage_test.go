package packaging_test

import (
	"crypto/sha256"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestAppImageDownloadsVerifyAndRepairTheCache(t *testing.T) {
	cmake := releaseCMake(t)
	helper, err := filepath.Abs("appimage-download.cmake")
	if err != nil {
		t.Fatal(err)
	}
	for _, mode := range []string{"download", "valid-cache", "corrupt-cache", "bad-download", "missing-download", "cache-link", "partial-link", "lock-link", "cache-directory"} {
		t.Run(mode, func(t *testing.T) {
			root := t.TempDir()
			cache := filepath.Join(root, "cache with spaces")
			if err := os.Mkdir(cache, 0700); err != nil {
				t.Fatal(err)
			}
			source := filepath.Join(root, "upstream")
			destination := filepath.Join(cache, "tool")
			outside := filepath.Join(root, "unrelated")
			content := []byte("verified upstream tool\n")
			digest := fmt.Sprintf("%x", sha256.Sum256(content))
			write := func(path string, data []byte) {
				t.Helper()
				if err := os.WriteFile(path, data, 0600); err != nil {
					t.Fatal(err)
				}
			}
			write(source, content)
			write(outside, []byte("keep me"))
			switch mode {
			case "valid-cache":
				write(destination, content)
				source = filepath.Join(root, "unavailable")
			case "corrupt-cache":
				write(destination, []byte("corrupt cache"))
				write(destination+".part", []byte("interrupted transfer"))
			case "bad-download":
				write(source, []byte("unexpected upstream replacement"))
				write(destination, []byte("corrupt cache"))
			case "missing-download":
				source = filepath.Join(root, "unavailable")
			case "cache-link", "partial-link", "lock-link":
				path := destination
				if mode == "partial-link" {
					path += ".part"
				} else if mode == "lock-link" {
					path += ".lock"
				}
				if err := os.Symlink(outside, path); err != nil {
					t.Skip(err)
				}
			case "cache-directory":
				if err := os.Mkdir(destination, 0700); err != nil {
					t.Fatal(err)
				}
			}
			script := filepath.Join(root, "check.cmake")
			write(script, []byte(fmt.Sprintf("cmake_minimum_required(VERSION 3.25)\ninclude([[%s]])\nappimage_download([[file://%s]] [[%s]] [[%s]])\n",
				filepath.ToSlash(helper), filepath.ToSlash(source), filepath.ToSlash(destination), digest)))
			output, err := exec.Command(cmake, "-P", script).CombinedOutput()
			valid := mode == "download" || mode == "valid-cache" || mode == "corrupt-cache"
			if (err == nil) != valid {
				t.Fatalf("wrong download result: %v\n%s", err, output)
			}
			if valid {
				data, err := os.ReadFile(destination)
				if err != nil || string(data) != string(content) {
					t.Fatalf("unverified cache: %q %v", data, err)
				}
			}
			if valid || mode == "bad-download" || mode == "missing-download" {
				if _, err := os.Lstat(destination + ".part"); !os.IsNotExist(err) {
					t.Fatal("temporary download retained", err)
				}
			}
			if mode == "bad-download" || mode == "missing-download" {
				if _, err := os.Lstat(destination); !os.IsNotExist(err) {
					t.Fatal("invalid download published", err)
				}
			}
			if mode == "bad-download" && !strings.Contains(string(output), "Invalid download removed") {
				t.Fatal("missing actionable checksum error", string(output))
			}
			if data, err := os.ReadFile(outside); err != nil || string(data) != "keep me" {
				t.Fatal("unrelated file modified", err)
			}
		})
	}
}

func TestAppImageReleasePinsUseNamedVersionsForBothArchitectures(t *testing.T) {
	cmake := releaseCMake(t)
	module, err := filepath.Abs("appimage.cmake")
	if err != nil {
		t.Fatal(err)
	}
	for _, arch := range []struct{ goarch, asset string }{{"amd64", "x86_64"}, {"arm64", "aarch64"}} {
		t.Run(arch.goarch, func(t *testing.T) {
			root := t.TempDir()
			// Replace only downloads; configure the actual release/install rules.
			body := fmt.Sprintf(`cmake_minimum_required(VERSION 3.25)
project(AppImagePinCheck NONE)
include([[%s]])
function(appimage_download url destination sha256)
  file(APPEND "${CMAKE_BINARY_DIR}/downloads.txt" "${url}\n${destination}\n${sha256}\n")
endfunction()
set(SNESBUILD_GOOS linux)
set(SNESBUILD_GOARCH %s)
set(_cache_dir "${CMAKE_BINARY_DIR}/cache")
include([[%s]])
`, filepath.ToSlash(filepath.Join(filepath.Dir(module), "appimage-download.cmake")), arch.goarch, filepath.ToSlash(module))
			if err := os.WriteFile(filepath.Join(root, "CMakeLists.txt"), []byte(body), 0600); err != nil {
				t.Fatal(err)
			}
			output, err := exec.Command(cmake, "-S", root, "-B", filepath.Join(root, "build")).CombinedOutput()
			if err != nil {
				t.Fatal(err, string(output))
			}
			data, err := os.ReadFile(filepath.Join(root, "build", "downloads.txt"))
			if err != nil {
				t.Fatal(err)
			}
			text := string(data)
			if strings.Contains(text, "/continuous/") ||
				!strings.Contains(text, "/1.9.1/appimagetool-"+arch.asset+".AppImage") ||
				!strings.Contains(text, "/20251108/runtime-"+arch.asset) {
				t.Fatal("mutable or mismatched AppImage release pins", text)
			}
			lines := strings.Split(strings.TrimSpace(text), "\n")
			if len(lines) != 6 || len(lines[2]) != 64 || len(lines[5]) != 64 ||
				!strings.Contains(lines[1], lines[2]) || !strings.Contains(lines[4], lines[5]) {
				t.Fatal("cache names do not identify the pinned digests", text)
			}
		})
	}
}
