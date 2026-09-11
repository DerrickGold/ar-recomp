package packaging_test

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func releaseCMake(t *testing.T) string {
	t.Helper()
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("CMake required for release policy tests")
	}
	return cmake
}

func TestDefaultReleaseMatrixAndExplicitLegacyEscapeHatch(t *testing.T) {
	cmake := releaseCMake(t)
	for _, legacy := range []bool{false, true} {
		output, err := exec.Command(cmake, "-DBUILDER_PRINT_PLAN=ON", fmt.Sprintf("-DBUILDER_LEGACY_ARCHIVES=%t", legacy), "-P", "release.cmake").CombinedOutput()
		if err != nil {
			t.Fatal(err, string(output))
		}
		text := string(output)
		if strings.Count(text, "Release plan:") != 7 {
			t.Fatal(text)
		}
		for _, arch := range []string{"arm64", "x86_64"} {
			for _, want := range []string{"linux-" + arch + " | archive | actraiser-recomp-linux-" + arch + ".tar.xz"} {
				if !strings.Contains(text, want) {
					t.Fatal("missing generic Linux archive", text)
				}
			}
			for _, pair := range [][2]string{{"macos-" + arch, ".app.zip"}, {"windows-" + arch, ".exe"}, {"steam-deck", ".AppImage"}} {
				if legacy {
					if !strings.Contains(text, pair[0]+" | archive |") {
						t.Fatal("legacy opt-in ignored", text)
					}
				} else if !strings.Contains(text, pair[0]+" | desktop | ActRaiserRecompBuilder-"+pair[0]+pair[1]) {
					t.Fatal("missing desktop replacement", text)
				}
			}
		}
		if !legacy && strings.Count(text, " | archive | ") != 2 {
			t.Fatal("redundant archives in default release", text)
		}
	}
	for _, platforms := range []string{"", "unknown", "../steam-deck", "steam-deck;steam-deck"} {
		if output, err := exec.Command(cmake, "-DBUILDER_PRINT_PLAN=ON", "-DBUILDER_PLATFORMS="+platforms, "-P", "release.cmake").CombinedOutput(); err == nil {
			t.Fatal("invalid selection accepted", platforms, string(output))
		}
	}
}

func TestReleasePruningOnlyRemovesSupersededFilesAfterPublication(t *testing.T) {
	cmake := releaseCMake(t)
	policy, err := filepath.Abs("release_policy.cmake")
	if err != nil {
		t.Fatal(err)
	}
	for _, mode := range []string{"normal", "missing-replacement", "retired-directory", "retired-symlink", "replacement-symlink"} {
		t.Run(mode, func(t *testing.T) {
			root := t.TempDir()
			keep := filepath.Join(root, "ActRaiserRecompBuilder-steam-deck.AppImage")
			old := filepath.Join(root, "actraiser-recomp-steam-deck-x86_64.tar.xz")
			unrelated := filepath.Join(root, "player-save.srm")
			for _, p := range []string{keep, keep + ".sha256", old, old + ".sha256", unrelated} {
				if err := os.WriteFile(p, []byte("fixture"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			switch mode {
			case "missing-replacement":
				os.Remove(keep)
			case "retired-directory":
				os.Remove(old)
				if err := os.Mkdir(old, 0700); err != nil {
					t.Fatal(err)
				}
			case "retired-symlink", "replacement-symlink":
				link := old
				if mode == "replacement-symlink" {
					link = keep
				}
				os.Remove(link)
				if err := os.Symlink(unrelated, link); err != nil {
					t.Skip(err)
				}
			}
			script := filepath.Join(t.TempDir(), "prune.cmake")
			body := fmt.Sprintf("cmake_minimum_required(VERSION 3.25)\ninclude([[%s]])\nbuilder_prune_replaced_release(steam-deck [[%s]])\n", filepath.ToSlash(policy), filepath.ToSlash(root))
			if err := os.WriteFile(script, []byte(body), 0600); err != nil {
				t.Fatal(err)
			}
			output, err := exec.Command(cmake, "-P", script).CombinedOutput()
			if (err == nil) != (mode == "normal") {
				t.Fatal("wrong pruning result", err, string(output))
			}
			if data, err := os.ReadFile(unrelated); err != nil || string(data) != "fixture" {
				t.Fatal("unrelated data changed", err)
			}
			_, statErr := os.Lstat(old + ".sha256")
			if mode == "normal" {
				if !os.IsNotExist(statErr) {
					t.Fatal("obsolete checksum retained", statErr)
				}
				if _, err := os.Lstat(old); !os.IsNotExist(err) {
					t.Fatal("obsolete archive retained", err)
				}
			} else if statErr != nil {
				t.Fatal("pruning partially ran after refusing unsafe state", statErr)
			}
		})
	}
}
