package packaging_test

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/romset"
)

func TestLauncherROMRequirementsAndChecksums(t *testing.T) {
	profile, err := os.ReadFile(filepath.Join("..", "desktop-shell", "recomplauncher.json"))
	if err != nil {
		t.Fatal(err)
	}
	var manifest struct {
		Inputs []struct {
			ID, SHA256 string
			Optional   bool
			Dumps      []struct {
				Name, CRC32 string
				Size        int
			}
		}
	}
	if err := json.Unmarshal(profile, &manifest); err != nil {
		t.Fatal(err)
	}
	files := map[string]string{
		"rom": "ar.sfc", "rom-jp": "ar-jp.sfc", "rom-eu-en": "ar-eu.sfc",
		"rom-de": "ar-ger.sfc", "rom-fr": "ar-fra.sfc", "rom-ar2-us": "ar2.sfc",
	}
	directory := os.Getenv("AR_ROM_SET_DIR")
	for _, input := range manifest.Inputs {
		file, ok := files[input.ID]
		if !ok {
			continue
		}
		delete(files, input.ID)
		if input.Optional != (input.ID != "rom") {
			t.Errorf("%s: only the base ActRaiser (USA) ROM should be required in Easy Setup", input.ID)
		}
		if input.SHA256 != "" || len(input.Dumps) != 1 {
			t.Fatalf("%s should advertise its headerless dump and allow copier headers", input.ID)
		}
		if directory == "" {
			continue
		}
		raw, err := os.ReadFile(filepath.Join(directory, file))
		if err != nil {
			t.Fatal(err)
		}
		id, payload, err := romset.Identify(raw)
		if err != nil || (input.ID != "rom-"+id && !(input.ID == "rom" && id == "us")) {
			t.Fatalf("input %s: unexpected ROM identity %q (%v)", input.ID, id, err)
		}
		dump := input.Dumps[0]
		if dump.Name == "" || dump.Size != len(payload) || !strings.EqualFold(dump.CRC32, fmt.Sprintf("%08x", crc32.ChecksumIEEE(payload))) {
			t.Errorf("%s: dump metadata does not match %s: %+v", input.ID, file, dump)
		}
	}
	if len(files) > 0 {
		t.Fatalf("missing launcher inputs: %v", files)
	}
}

func releaseCMake(t *testing.T) string {
	t.Helper()
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("CMake required for release policy tests")
	}
	return cmake
}

func TestLinuxSDKMountCleanup(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX mount helper")
	}
	for _, mode := range []string{"mounted", "attach-failed", "partially-attached"} {
		t.Run(mode, func(t *testing.T) {
			root := t.TempDir()
			cache := filepath.Join(root, "external cache")
			if err := os.Mkdir(cache, 0700); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(cache, "linux-sdk.sparseimage"), nil, 0600); err != nil {
				t.Fatal(err)
			}
			stub := `#!/bin/sh
set -eu
case "$1" in
    info)
        if test -f "$SDK_TEST_DIR/attached"; then
            printf 'image-path      : %s\n' "$(cat "$SDK_TEST_DIR/attached")"
        fi
        ;;
    attach)
        test "$6" = -mountpoint
        printf '%s\n' "$7" > "$SDK_TEST_DIR/mountpoint"
        test "$SDK_TEST_MODE" != attach-failed || exit 41
        printf '%s\n' "$2" > "$SDK_TEST_DIR/attached"
        test "$SDK_TEST_MODE" != partially-attached || exit 42
        ;;
    detach)
        printf '%s\n' "$2" > "$SDK_TEST_DIR/detach"
        test "$SDK_TEST_MODE" != partially-attached || exit 42
        rm "$SDK_TEST_DIR/attached"
        ;;
    *) exit 1 ;;
esac
`
			if err := os.WriteFile(filepath.Join(root, "hdiutil"), []byte(stub), 0755); err != nil {
				t.Fatal(err)
			}
			cmd := exec.Command("/bin/sh", "../desktop-shell/with-linux-sdk-volume.sh", cache,
				"/bin/sh", "-c", `test "$BUILDER_LINUX_SDK_VOLUME" = "$(cat "$SDK_TEST_DIR/mountpoint")"`)
			cmd.Env = append(os.Environ(), "PATH="+root+":"+os.Getenv("PATH"), "TMPDIR="+cache,
				"SDK_TEST_DIR="+root, "SDK_TEST_MODE="+mode)
			output, runErr := cmd.CombinedOutput()
			var wantExit int
			switch mode {
			case "attach-failed":
				wantExit = 41
			case "partially-attached":
				wantExit = 1
			}
			if (wantExit == 0 && runErr != nil) || (wantExit != 0 && cmd.ProcessState.ExitCode() != wantExit) {
				t.Fatalf("wanted exit %d: %v\n%s", wantExit, runErr, output)
			}
			data, err := os.ReadFile(filepath.Join(root, "mountpoint"))
			if err != nil {
				t.Fatal(err)
			}
			mountpoint := strings.TrimSpace(string(data))
			temp, err := filepath.EvalSymlinks("/tmp")
			if err != nil || filepath.Dir(mountpoint) != temp || !strings.HasPrefix(filepath.Base(mountpoint), "actraiser-linux-sdk.") {
				t.Fatalf("SDK mountpoint must use the internal temporary directory: %s (%v)", mountpoint, err)
			}
			t.Cleanup(func() { _ = os.Remove(mountpoint) })
			_, detachErr := os.Stat(filepath.Join(root, "detach"))
			if os.IsNotExist(detachErr) != (mode == "attach-failed") {
				t.Fatalf("unexpected detach attempt: %v", detachErr)
			}
			for _, dir := range []string{mountpoint, filepath.Join(cache, "linux-sdk.lock")} {
				_, err := os.Stat(dir)
				if (mode == "partially-attached" && err != nil) || (mode != "partially-attached" && !os.IsNotExist(err)) {
					t.Fatalf("wrong cleanup result for %s: %v\n%s", dir, err, output)
				}
			}
		})
	}
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
				if strings.Count(text, "Release plan:") != 6 || strings.Contains(text, "steam-deck") {
					t.Fatal(text)
				}
				for _, arch := range []string{"arm64", "x86_64"} {
					platform := "linux-" + arch
					base := "ActRaiserRecompBuilder-" + version + "-" + strings.ReplaceAll(platform, "x86_64", "x64")
					want := platform + " | archive | " + base + ".tar.xz"
					if !legacy {
						want = platform + " | desktop | " + base + ".AppImage | archive | " + base + ".tar.xz"
					}
					if !strings.Contains(text, want) {
						t.Fatal("missing Linux release", text)
					}
					for _, pair := range [][2]string{
						{"macos-" + arch, ".tar.xz"},
						{"windows-" + arch, ".zip"},
					} {
						publicPlatform := strings.ReplaceAll(pair[0], "windows-x86_64", "windows-x64")
						if legacy {
							want := pair[0] + " | archive | actraiser-recomp-" + version + "-" + publicPlatform + pair[1]
							if !strings.Contains(text, want) {
								t.Fatal("wrong legacy archive name", text)
							}
						} else {
							base := "ActRaiserRecompBuilder-" + version + "-" + publicPlatform
							want := pair[0] + " | desktop | " + base + "-portable.zip"
							if !strings.Contains(text, want) {
								t.Fatal("missing portable ZIP", text)
							}
						}
					}
				}
				if legacy {
					if strings.Contains(text, " | portable | ") {
						t.Fatal("legacy archive unexpectedly gained desktop portable companion", text)
					}
				} else {
					if strings.Count(text, " | archive | ") != 2 || strings.Count(text, "-portable.zip") != 4 {
						t.Fatal("wrong desktop or Linux fallback count", text)
					}
					for _, retired := range []string{".exe", ".app.zip", "-portable.tar.xz"} {
						if strings.Contains(text, retired) {
							t.Fatal("redundant desktop download in default release", text)
						}
					}
				}
			})
		}
	}
	output, err := exec.Command(cmake, "-DBUILDER_PRINT_PLAN=ON", "-DSNESBUILD_VERSION=v0900", "-DBUILDER_PLATFORMS=steam-deck", "-P", "release.cmake").CombinedOutput()
	if err != nil || !strings.Contains(string(output), "steam-deck | desktop | ActRaiserRecompBuilder-v0900-linux-x64.AppImage") {
		t.Fatal("Steam Deck alias must produce the Linux x64 AppImage", err, string(output))
	}
	for _, platforms := range []string{"", "unknown", "../steam-deck", "steam-deck;steam-deck", "steam-deck;linux-x86_64"} {
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
	for _, mode := range []string{"normal", "missing-portable", "missing-checksum", "retired-directory", "retired-symlink", "portable-symlink"} {
		t.Run(mode, func(t *testing.T) {
			root := t.TempDir()
			keep := filepath.Join(root, "ActRaiserRecompBuilder-v0900-windows-x64.exe")
			portable := filepath.Join(root, "ActRaiserRecompBuilder-v0900-windows-x64-portable.zip")
			old := filepath.Join(root, "actraiser-recomp-v0900-windows-x64.zip")
			unrelated := filepath.Join(root, "player-save.srm")
			previous := filepath.Join(root, "actraiser-recomp-v0805-windows-x64.zip")
			for _, p := range []string{keep, keep + ".sha256", portable, portable + ".sha256", old, old + ".sha256", unrelated, previous, previous + ".sha256"} {
				if err := os.WriteFile(p, []byte("fixture"), 0600); err != nil {
					t.Fatal(err)
				}
			}
			switch mode {
			case "missing-portable":
				os.Remove(portable)
			case "missing-checksum":
				os.Remove(portable + ".sha256")
			case "retired-directory":
				os.Remove(old)
				if err := os.Mkdir(old, 0700); err != nil {
					t.Fatal(err)
				}
			case "retired-symlink", "portable-symlink":
				link := old
				if mode == "portable-symlink" {
					link = portable
				}
				os.Remove(link)
				if err := os.Symlink(unrelated, link); err != nil {
					t.Skip(err)
				}
			}
			script := filepath.Join(t.TempDir(), "prune.cmake")
			body := fmt.Sprintf("cmake_minimum_required(VERSION 3.25)\nset(SNESBUILD_VERSION v0900)\ninclude([[%s]])\nbuilder_prune_replaced_release(windows-x86_64 [[%s]])\n", filepath.ToSlash(policy), filepath.ToSlash(root))
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
				for _, name := range []string{keep, keep + ".sha256"} {
					if _, err := os.Lstat(name); !os.IsNotExist(err) {
						t.Fatal("obsolete Windows EXE retained", name, err)
					}
				}
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

func TestLinuxAppImageKeepsFallbackAndRetiresDuplicateAppImageArchive(t *testing.T) {
	cmake := releaseCMake(t)
	policy, err := filepath.Abs("release_policy.cmake")
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	base := filepath.Join(dir, "ActRaiserRecompBuilder-v0900-linux-x64")
	for _, suffix := range []string{".AppImage", "-portable.tar.xz", ".tar.xz"} {
		for _, path := range []string{base + suffix, base + suffix + ".sha256"} {
			if err := os.WriteFile(path, []byte("fixture"), 0600); err != nil {
				t.Fatal(err)
			}
		}
	}
	script := filepath.Join(t.TempDir(), "keep.cmake")
	body := fmt.Sprintf("cmake_minimum_required(VERSION 3.25)\nset(SNESBUILD_VERSION v0900)\ninclude([[%s]])\nbuilder_prune_replaced_release(linux-x86_64 [[%s]])\n", filepath.ToSlash(policy), filepath.ToSlash(dir))
	if err := os.WriteFile(script, []byte(body), 0600); err != nil {
		t.Fatal(err)
	}
	fallbackChecksum := base + ".tar.xz.sha256"
	if err := os.Remove(fallbackChecksum); err != nil {
		t.Fatal(err)
	}
	if output, err := exec.Command(cmake, "-P", script).CombinedOutput(); err == nil {
		t.Fatal("duplicate retired before Linux fallback was ready", string(output))
	}
	if _, err := os.Stat(base + "-portable.tar.xz"); err != nil {
		t.Fatal("old Linux download lost before fallback publication", err)
	}
	if err := os.WriteFile(fallbackChecksum, []byte("fixture"), 0600); err != nil {
		t.Fatal(err)
	}
	if output, err := exec.Command(cmake, "-P", script).CombinedOutput(); err != nil {
		t.Fatal(err, string(output))
	}
	for _, suffix := range []string{".tar.xz", ".tar.xz.sha256"} {
		if _, err := os.Stat(base + suffix); err != nil {
			t.Fatal("explicit archive or checksum removed", err)
		}
	}
	for _, suffix := range []string{"-portable.tar.xz", "-portable.tar.xz.sha256"} {
		if _, err := os.Lstat(base + suffix); !os.IsNotExist(err) {
			t.Fatal("duplicate AppImage archive retained", err)
		}
	}
}

func TestDesktopReleaseUsesFinalPayloadVersion(t *testing.T) {
	t.Run("windows-x86_64", func(t *testing.T) {
		testDesktopReleaseUsesFinalPayloadVersion(t, "windows-x86_64", "windows", "windows-x64-portable.zip")
	})
	t.Run("macos-arm64", func(t *testing.T) {
		if _, err := exec.LookPath("ditto"); err != nil {
			t.Skip("ditto required for macOS releases")
		}
		testDesktopReleaseUsesFinalPayloadVersion(t, "macos-arm64", "macos", "macos-arm64-portable.zip")
	})
	t.Run("linux-arm64", func(t *testing.T) {
		testDesktopReleaseUsesFinalPayloadVersion(t, "linux-arm64", "linux", "linux-arm64.AppImage")
	})
}

const macOSBuilderProgram = "ActRaiserRecompBuilder.app/Contents/MacOS/ActRaiserRecompBuilder"

func testDesktopReleaseUsesFinalPayloadVersion(t *testing.T, platform, kind, filename string) {
	t.Helper()
	cmake := releaseCMake(t)
	root := t.TempDir()
	for _, relative := range []string{
		"packaging/release_policy.cmake", "packaging/release_version.cmake",
		"desktop-shell/release.cmake", "desktop-shell/portable-release.cmake",
		"desktop-shell/PORTABLE-README.txt", "desktop-shell/recomplauncher.json",
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
	build := filepath.Join(root, "installer", "packaging", "build", platform)
	if err := os.MkdirAll(build, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(build, "CMakeCache.txt"), []byte("SNESBUILD_VERSION_STAMP:INTERNAL=v0900\n"), 0600); err != nil {
		t.Fatal(err)
	}
	// A build may regenerate CMake's cache after the source tag has changed.
	// The publisher must use the payload's final stamp, not its initial cache.
	target := "${BUILDER_OUTPUT}"
	if kind == "macos" {
		target += "/Contents/MacOS/ActRaiserRecompBuilder"
	}
	packager := fmt.Sprintf(`cmake_minimum_required(VERSION 3.25)
file(WRITE "${BUILDER_DIST_BUILD}/CMakeCache.txt" "SNESBUILD_VERSION_STAMP:INTERNAL=v1.0\n")
file(WRITE "%s" "built payload for v1.0\n")
`, target)
	if kind == "windows" {
		packager += `file(WRITE "${BUILDER_DIST_BUILD}/bin/actraiser-builder.exe" "console helper")
`
	}
	shell := filepath.Join(root, "installer", "desktop-shell")
	if err := os.WriteFile(filepath.Join(shell, "package-"+kind+".cmake"), []byte(packager), 0600); err != nil {
		t.Fatal(err)
	}
	output, err := exec.Command(cmake, "-DBUILDER_PLATFORM="+platform, "-P", filepath.Join(shell, "release.cmake")).CombinedOutput()
	if err != nil {
		t.Fatal(err, string(output))
	}
	release := filepath.Join(root, "release")
	entries, err := os.ReadDir(release)
	if err != nil || len(entries) != 2 {
		t.Fatalf("expected only the desktop download and its checksum: %v, %v", entries, err)
	}
	name := "ActRaiserRecompBuilder-v1.0-" + filename
	data, err := os.ReadFile(filepath.Join(release, name))
	if err != nil {
		t.Fatal("missing final-version artifact", err)
	}
	checksum, err := os.ReadFile(filepath.Join(release, name+".sha256"))
	want := fmt.Sprintf("%x  %s\n", sha256.Sum256(data), name)
	if err != nil || string(checksum) != want {
		t.Fatalf("checksum must identify the final version: %q, %v", checksum, err)
	}
	if kind == "macos" || kind == "windows" {
		extracted := t.TempDir()
		cmd := exec.Command(cmake, "-E", "tar", "xf", filepath.Join(release, name))
		cmd.Dir = extracted
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatal(err, string(output))
		}
		bundle := filepath.Join(extracted, strings.TrimSuffix(name, ".zip"))
		if kind == "macos" {
			checkReleaseLauncherProfile(t, bundle, macOSBuilderProgram)
		} else {
			checkReleaseLauncherProfile(t, bundle, "ActRaiserRecompBuilder.exe")
			helper, err := os.ReadFile(filepath.Join(bundle, "actraiser-builder.exe"))
			if err != nil || string(helper) != "console helper" {
				t.Fatalf("missing mod CLI in portable release: %v", err)
			}
		}
	}
}

func checkReleaseLauncherProfile(t *testing.T, directory, program string) {
	t.Helper()
	readProfile := func(path string) map[string]any {
		t.Helper()
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		var profile map[string]any
		if err := json.Unmarshal(data, &profile); err != nil {
			t.Fatalf("%s: %v", path, err)
		}
		return profile
	}
	want := readProfile(filepath.Join("..", "desktop-shell", "recomplauncher.json"))
	want["start"].(map[string]any)["program"] = []any{program}
	got := readProfile(filepath.Join(directory, "recomplauncher.json"))
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("expected the shared launcher profile with start.program %q, got %v", program, got)
	}
	if info, err := os.Stat(filepath.Join(directory, filepath.FromSlash(program))); err != nil || !info.Mode().IsRegular() {
		t.Fatalf("launcher program %s is missing from the archive: %v", program, err)
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
		{"windows", "windows-arm64", "ActRaiserRecompBuilder.exe", "ActRaiserRecompBuilder-v0900-windows-arm64-portable.zip"},
		{"linux", "linux-x86_64", "ActRaiserRecompBuilder.AppImage", "ActRaiserRecompBuilder-v0900-linux-x64-portable.tar.xz"},
		{"linux", "linux-arm64", "ActRaiserRecompBuilder.AppImage", "ActRaiserRecompBuilder-v0900-linux-arm64-portable.tar.xz"},
		{"macos", "macos-arm64", "ActRaiserRecompBuilder.app", "ActRaiserRecompBuilder-v0900-macos-arm64-portable.zip"},
		{"macos", "macos-x86_64", "ActRaiserRecompBuilder.app", "ActRaiserRecompBuilder-v0900-macos-x86_64-portable.zip"},
	}
	for _, test := range tests {
		t.Run(test.platform, func(t *testing.T) {
			if test.kind == "macos" {
				if _, err := exec.LookPath("ditto"); err != nil {
					t.Skip("ditto required for macOS releases")
				}
			}
			root := t.TempDir()
			stage := filepath.Join(root, "stage")
			if err := os.Mkdir(stage, 0700); err != nil {
				t.Fatal(err)
			}
			artifact := filepath.Join(root, test.artifact)
			payload := artifact
			if test.kind == "macos" {
				payload = filepath.Join(root, filepath.FromSlash(macOSBuilderProgram))
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
			if test.kind == "windows" {
				checkReleaseLauncherProfile(t, bundle, test.artifact)
			} else if test.kind == "macos" {
				checkReleaseLauncherProfile(t, bundle, macOSBuilderProgram)
				bundledPayload = filepath.Join(bundle, filepath.FromSlash(macOSBuilderProgram))
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
