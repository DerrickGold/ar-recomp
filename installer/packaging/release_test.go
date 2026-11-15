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
	for _, version := range []string{"v0900", "v1.0"} {
		for _, legacy := range []bool{false, true} {
			t.Run(fmt.Sprintf("%s/legacy=%t", version, legacy), func(t *testing.T) {
				output, err := exec.Command(cmake, "-DBUILDER_PRINT_PLAN=ON", "-DSNESBUILD_VERSION="+version,
					fmt.Sprintf("-DBUILDER_LEGACY_ARCHIVES=%t", legacy), "-P", "release.cmake").CombinedOutput()
				if err != nil {
					t.Fatal(err, string(output))
				}
				text := string(output)
				if strings.Count(text, "Release plan:") != 7 {
					t.Fatal(text)
				}
				for _, arch := range []string{"arm64", "x86_64"} {
					want := "linux-" + arch + " | archive | ActRaiserRecompBuilder-" + version + "-linux-" + arch + ".tar.xz"
					if !strings.Contains(text, want) {
						t.Fatal("missing generic Linux archive", text)
					}
					for _, pair := range [][4]string{
						{"macos-" + arch, ".app.zip", ".zip", ".tar.xz"},
						{"windows-" + arch, ".exe", ".zip", ".zip"},
					} {
						publicPlatform := strings.ReplaceAll(pair[0], "windows-x86_64", "windows-x64")
						if legacy {
							want := pair[0] + " | archive | actraiser-recomp-" + version + "-" + publicPlatform + pair[3]
							if !strings.Contains(text, want) {
								t.Fatal("wrong legacy archive name", text)
							}
						} else {
							base := "ActRaiserRecompBuilder-" + version + "-" + publicPlatform
							want := pair[0] + " | desktop | " + base + pair[1] + " | portable | " + base + "-portable" + pair[2]
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
					if !strings.Contains(text, "steam-deck | archive | actraiser-recomp-"+version+"-steam-deck-x86_64.tar.xz") {
						t.Fatal("wrong legacy Steam Deck archive", text)
					}
				} else {
					base := "ActRaiserRecompBuilder-" + version + "-steam-deck"
					want := "steam-deck | desktop | " + base + ".AppImage | portable | " + base + "-portable.tar.xz"
					if !strings.Contains(text, want) {
						t.Fatal("missing Steam Deck release pair", text)
					}
					if strings.Count(text, " | portable | ") != 5 {
						t.Fatal("wrong portable desktop bundle count", text)
					}
					if strings.Count(text, " | archive | ") != 2 {
						t.Fatal("redundant archives in default release", text)
					}
				}
			})
		}
	}
	for _, platforms := range []string{"", "unknown", "../steam-deck", "steam-deck;steam-deck"} {
		if output, err := exec.Command(cmake, "-DBUILDER_PRINT_PLAN=ON", "-DSNESBUILD_VERSION=v0900", "-DBUILDER_PLATFORMS="+platforms, "-P", "release.cmake").CombinedOutput(); err == nil {
			t.Fatal("invalid selection accepted", platforms, string(output))
		}
	}
}

func TestReleaseVersionRejectsUnsafeFilenameComponents(t *testing.T) {
	cmake := releaseCMake(t)
	for _, version := range []string{"../v0900", "v0900/next", "v0900\\next", "v0900;next", "v0900 next", "v0900\nnext", "-v0900", "v0900:next"} {
		t.Run(version, func(t *testing.T) {
			output, err := exec.Command(cmake, "-DBUILDER_PRINT_PLAN=ON", "-DBUILDER_PLATFORMS=windows-x86_64",
				"-DSNESBUILD_VERSION="+version, "-P", "release.cmake").CombinedOutput()
			if err == nil {
				t.Fatal("unsafe version accepted", string(output))
			}
			if strings.Contains(string(output), "Release plan:") {
				t.Fatal("unsafe version reached release plan", string(output))
			}
		})
	}
}

func TestReleaseVersionTracksTagsAndDevelopmentBuilds(t *testing.T) {
	cmake := releaseCMake(t)
	git, err := exec.LookPath("git")
	if err != nil {
		t.Skip("Git required for version discovery tests")
	}
	helper, err := os.ReadFile("release_version.cmake")
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	packaging := filepath.Join(root, "installer", "packaging")
	if err := os.MkdirAll(packaging, 0700); err != nil {
		t.Fatal(err)
	}
	helperPath := filepath.Join(packaging, "release_version.cmake")
	if err := os.WriteFile(helperPath, helper, 0600); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(t.TempDir(), "resolve.cmake")
	body := fmt.Sprintf("cmake_minimum_required(VERSION 3.25)\ninclude([[%s]])\nbuilder_resolve_release_version(version)\nmessage(STATUS \"VERSION=${version}\")\n", filepath.ToSlash(helperPath))
	if err := os.WriteFile(script, []byte(body), 0600); err != nil {
		t.Fatal(err)
	}
	resolve := func(explicit string) string {
		t.Helper()
		output, err := exec.Command(cmake, "-DSNESBUILD_VERSION="+explicit, "-P", script).CombinedOutput()
		if err != nil {
			t.Fatal(err, string(output))
		}
		for _, line := range strings.Split(string(output), "\n") {
			if strings.HasPrefix(line, "-- VERSION=") {
				return strings.TrimPrefix(line, "-- VERSION=")
			}
		}
		t.Fatal("missing version output", string(output))
		return ""
	}
	if got := resolve(""); got != "0.0.0-dev" {
		t.Fatal("source snapshot fallback", got)
	}
	for _, version := range []string{"v0900", "v1.0", "v1.0.1", "v1.0-rc.1+build.7"} {
		if got := resolve(version); got != version {
			t.Fatalf("explicit version %q became %q", version, got)
		}
	}
	runGit := func(args ...string) {
		t.Helper()
		cmd := exec.Command(git, args...)
		cmd.Dir = root
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatal(err, string(output))
		}
	}
	runGit("init", "-q")
	runGit("config", "user.name", "Release Test")
	runGit("config", "user.email", "release-test@example.invalid")
	runGit("config", "tag.gpgsign", "false")
	tracked := filepath.Join(root, "tracked.txt")
	if err := os.WriteFile(tracked, []byte("first\n"), 0600); err != nil {
		t.Fatal(err)
	}
	runGit("add", "tracked.txt")
	runGit("-c", "commit.gpgsign=false", "commit", "-qm", "Initial release")
	runGit("tag", "v0900")
	if got := resolve(""); got != "v0900" {
		t.Fatal("compact tag was not preserved", got)
	}
	if err := os.WriteFile(tracked, []byte("second\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if got := resolve(""); got != "v0900-dirty" {
		t.Fatal("dirty build lost its tag or suffix", got)
	}
	runGit("-c", "commit.gpgsign=false", "commit", "-qam", "Next release")
	if got := resolve(""); !strings.HasPrefix(got, "v0900-1-g") || strings.HasSuffix(got, "-dirty") {
		t.Fatal("development build lost its source revision", got)
	}
	runGit("tag", "v1.0")
	if got := resolve(""); got != "v1.0" {
		t.Fatal("dotted tag was not preserved", got)
	}
	if got := resolve("v1000"); got != "v1000" {
		t.Fatal("explicit compact version did not override discovered tag", got)
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
			keep := filepath.Join(root, "ActRaiserRecompBuilder-v0900-steam-deck.AppImage")
			portable := filepath.Join(root, "ActRaiserRecompBuilder-v0900-steam-deck-portable.tar.xz")
			old := filepath.Join(root, "actraiser-recomp-v0900-steam-deck-x86_64.tar.xz")
			unrelated := filepath.Join(root, "player-save.srm")
			previous := filepath.Join(root, "actraiser-recomp-v0805-steam-deck-x86_64.tar.xz")
			for _, p := range []string{keep, keep + ".sha256", portable, portable + ".sha256", old, old + ".sha256", unrelated, previous, previous + ".sha256"} {
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
			body := fmt.Sprintf("cmake_minimum_required(VERSION 3.25)\nset(SNESBUILD_VERSION v0900)\ninclude([[%s]])\nbuilder_prune_replaced_release(steam-deck [[%s]])\n", filepath.ToSlash(policy), filepath.ToSlash(root))
			if err := os.WriteFile(script, []byte(body), 0600); err != nil {
				t.Fatal(err)
			}
			output, err := exec.Command(cmake, "-P", script).CombinedOutput()
			if (err == nil) != (mode == "normal") {
				t.Fatal("wrong pruning result", err, string(output))
			}
			for _, path := range []string{unrelated, previous, previous + ".sha256"} {
				if data, err := os.ReadFile(path); err != nil || string(data) != "fixture" {
					t.Fatal("unrelated data or previous release changed", path, err)
				}
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

func TestDesktopReleaseUsesFinalPayloadVersion(t *testing.T) {
	cmake := releaseCMake(t)
	root := t.TempDir()
	for _, relative := range []string{
		"packaging/release_policy.cmake", "packaging/release_version.cmake",
		"desktop-shell/release.cmake", "desktop-shell/portable-release.cmake",
		"desktop-shell/PORTABLE-README.txt",
	} {
		data, err := os.ReadFile(filepath.Join("..", relative))
		if err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(root, "installer", relative)
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	build := filepath.Join(root, "installer", "packaging", "build", "windows-x86_64")
	if err := os.MkdirAll(build, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(build, "CMakeCache.txt"), []byte("SNESBUILD_VERSION_STAMP:INTERNAL=v0900\n"), 0600); err != nil {
		t.Fatal(err)
	}
	// A build may regenerate CMake's cache after the source tag has changed.
	// The publisher must use the payload's final stamp, not its initial cache.
	packager := `cmake_minimum_required(VERSION 3.25)
file(WRITE "${BUILDER_DIST_BUILD}/CMakeCache.txt" "SNESBUILD_VERSION_STAMP:INTERNAL=v1.0\n")
file(WRITE "${BUILDER_OUTPUT}" "built payload for v1.0\n")
`
	shell := filepath.Join(root, "installer", "desktop-shell")
	if err := os.WriteFile(filepath.Join(shell, "package-windows.cmake"), []byte(packager), 0600); err != nil {
		t.Fatal(err)
	}
	output, err := exec.Command(cmake, "-DBUILDER_PLATFORM=windows-x86_64", "-P", filepath.Join(shell, "release.cmake")).CombinedOutput()
	if err != nil {
		t.Fatal(err, string(output))
	}
	release := filepath.Join(root, "release")
	entries, err := os.ReadDir(release)
	if err != nil || len(entries) != 4 {
		t.Fatalf("expected only the final direct/portable files and checksums: %v, %v", entries, err)
	}
	for _, name := range []string{
		"ActRaiserRecompBuilder-v1.0-windows-x64.exe",
		"ActRaiserRecompBuilder-v1.0-windows-x64-portable.zip",
	} {
		data, err := os.ReadFile(filepath.Join(release, name))
		if err != nil {
			t.Fatal("missing final-version artifact", err)
		}
		checksum, err := os.ReadFile(filepath.Join(release, name+".sha256"))
		want := fmt.Sprintf("%x  %s\n", sha256.Sum256(data), name)
		if err != nil || string(checksum) != want {
			t.Fatalf("checksum must identify the final version: %q, %v", checksum, err)
		}
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
		{"windows", "windows-x86_64", "ActRaiserRecompBuilder.exe", "ActRaiserRecompBuilder-v0900-windows-x64-portable.zip"},
		{"linux", "steam-deck", "ActRaiserRecompBuilder.AppImage", "ActRaiserRecompBuilder-v0900-steam-deck-portable.tar.xz"},
	}
	if _, err := exec.LookPath("ditto"); err == nil {
		tests = append(tests, struct {
			kind, platform, artifact, archive string
		}{"macos", "macos-arm64", "ActRaiserRecompBuilder.app", "ActRaiserRecompBuilder-v0900-macos-arm64-portable.zip"})
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
			body := fmt.Sprintf("cmake_minimum_required(VERSION 3.25)\nset(SNESBUILD_VERSION v0900)\ninclude([[%s]])\nbuilder_create_portable_release(%s %s [[%s]] [[%s]] archive filename)\n",
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
			bundleName := strings.TrimSuffix(strings.TrimSuffix(test.archive, ".zip"), ".tar.xz")
			bundle := filepath.Join(extracted, bundleName)
			readme, err := os.ReadFile(filepath.Join(bundle, "README.txt"))
			guidance := strings.Join(strings.Fields(string(readme)), " ")
			if err != nil || !strings.Contains(guidance, "Smart App Control") || !strings.Contains(guidance, "Open Anyway") {
				t.Fatalf("portable download lacks startup security guidance: %v", err)
			}
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
