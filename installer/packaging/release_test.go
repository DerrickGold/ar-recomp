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
			for _, pair := range [][3]string{
				{"macos-" + arch, ".app.zip", ".zip"},
				{"windows-" + arch, ".exe", ".zip"},
			} {
				if legacy {
					if !strings.Contains(text, pair[0]+" | archive |") {
						t.Fatal("legacy opt-in ignored", text)
					}
				} else {
					want := pair[0] + " | desktop | ActRaiserRecompBuilder-" + pair[0] + pair[1] +
						" | portable | ActRaiserRecompBuilder-" + pair[0] + "-portable" + pair[2]
					if !strings.Contains(text, want) {
						t.Fatal("missing desktop release pair", text)
					}
				}
			}
		}
		if legacy {
			if strings.Contains(text, " | portable | ") {
				t.Fatal("legacy archive unexpectedly gained desktop portable companion", text)
			}
		} else {
			want := "steam-deck | desktop | ActRaiserRecompBuilder-steam-deck.AppImage" +
				" | portable | ActRaiserRecompBuilder-steam-deck-portable.tar.xz"
			if !strings.Contains(text, want) {
				t.Fatal("missing Steam Deck release pair", text)
			}
			if strings.Count(text, " | portable | ") != 5 {
				t.Fatal("wrong portable desktop bundle count", text)
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
	for _, mode := range []string{"normal", "missing-replacement", "missing-portable", "retired-directory", "retired-symlink", "replacement-symlink", "portable-symlink"} {
		t.Run(mode, func(t *testing.T) {
			root := t.TempDir()
			keep := filepath.Join(root, "ActRaiserRecompBuilder-steam-deck.AppImage")
			portable := filepath.Join(root, "ActRaiserRecompBuilder-steam-deck-portable.tar.xz")
			old := filepath.Join(root, "actraiser-recomp-steam-deck-x86_64.tar.xz")
			unrelated := filepath.Join(root, "player-save.srm")
			for _, p := range []string{keep, keep + ".sha256", portable, portable + ".sha256", old, old + ".sha256", unrelated} {
				if err := os.WriteFile(p, []byte("fixture"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			switch mode {
			case "missing-replacement":
				os.Remove(keep)
			case "missing-portable":
				os.Remove(portable)
			case "retired-directory":
				os.Remove(old)
				if err := os.Mkdir(old, 0700); err != nil {
					t.Fatal(err)
				}
			case "retired-symlink", "replacement-symlink", "portable-symlink":
				link := old
				if mode == "replacement-symlink" {
					link = keep
				} else if mode == "portable-symlink" {
					link = portable
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

func TestPortableReleaseBundlesWrapExistingArtifacts(t *testing.T) {
	cmake := releaseCMake(t)
	helper, err := filepath.Abs(filepath.Join("..", "desktop-shell", "portable-release.cmake"))
	if err != nil {
		t.Fatal(err)
	}
	tests := []struct {
		kind, platform, artifact, archive string
	}{
		{"windows", "windows-x86_64", "ActRaiserRecompBuilder.exe", "ActRaiserRecompBuilder-windows-x86_64-portable.zip"},
		{"linux", "steam-deck", "ActRaiserRecompBuilder.AppImage", "ActRaiserRecompBuilder-steam-deck-portable.tar.xz"},
	}
	if _, err := exec.LookPath("ditto"); err == nil {
		tests = append(tests, struct {
			kind, platform, artifact, archive string
		}{"macos", "macos-arm64", "ActRaiserRecompBuilder.app", "ActRaiserRecompBuilder-macos-arm64-portable.zip"})
	}
	for _, test := range tests {
		t.Run(test.kind, func(t *testing.T) {
			root := t.TempDir()
			stage := filepath.Join(root, "stage")
			if err := os.Mkdir(stage, 0700); err != nil {
				t.Fatal(err)
			}
			artifact := filepath.Join(root, test.artifact)
			payload := artifact
			if test.kind == "macos" {
				payload = filepath.Join(artifact, "Contents", "MacOS", "ActRaiserRecompBuilder")
				if err := os.MkdirAll(filepath.Dir(payload), 0700); err != nil {
					t.Fatal(err)
				}
			}
			if err := os.WriteFile(payload, []byte("already built\n"), 0755); err != nil {
				t.Fatal(err)
			}
			script := filepath.Join(root, "bundle.cmake")
			body := fmt.Sprintf("cmake_minimum_required(VERSION 3.25)\ninclude([[%s]])\nbuilder_create_portable_release(%s %s [[%s]] [[%s]] archive filename)\n",
				filepath.ToSlash(helper), test.kind, test.platform, filepath.ToSlash(artifact), filepath.ToSlash(stage))
			if err := os.WriteFile(script, []byte(body), 0600); err != nil {
				t.Fatal(err)
			}
			if output, err := exec.Command(cmake, "-P", script).CombinedOutput(); err != nil {
				t.Fatal(err, string(output))
			}

			archive := filepath.Join(stage, test.archive)
			extracted := filepath.Join(root, "extracted")
			if err := os.Mkdir(extracted, 0700); err != nil {
				t.Fatal(err)
			}
			cmd := exec.Command(cmake, "-E", "tar", "xf", archive)
			cmd.Dir = extracted
			if output, err := cmd.CombinedOutput(); err != nil {
				t.Fatal(err, string(output))
			}
			entries, err := os.ReadDir(extracted)
			if err != nil || len(entries) != 1 || !entries[0].IsDir() {
				t.Fatalf("portable archive should extract as one folder: %v, %v", entries, err)
			}
			bundle := filepath.Join(extracted, "ActRaiserRecompBuilder-"+test.platform+"-portable")
			bundledArtifact := filepath.Join(bundle, test.artifact)
			bundledPayload := bundledArtifact
			if test.kind == "macos" {
				bundledPayload = filepath.Join(bundledArtifact, "Contents", "MacOS", "ActRaiserRecompBuilder")
			}
			data, err := os.ReadFile(bundledPayload)
			if err != nil || string(data) != "already built\n" {
				t.Fatalf("built artifact was not reused intact: %q, %v", data, err)
			}
			marker, err := os.ReadFile(bundledArtifact + ".portable")
			if err != nil || string(marker) != "BuilderData\n" {
				t.Fatalf("wrong portable marker: %q, %v", marker, err)
			}
			if test.kind == "linux" {
				info, err := os.Stat(bundledArtifact)
				if err != nil || info.Mode().Perm()&0111 == 0 {
					t.Fatalf("AppImage lost its executable mode: %v, %v", info, err)
				}
			}
		})
	}
}
