package desktop

import (
	"bytes"
	"context"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// Explicit acceptance must fail, not silently skip, when the Linux host or
// toolchain is unavailable. Ordinary unit tests do not download/run AppImages.
func TestAppImageAcceptance(t *testing.T) {
	if os.Getenv("AR_APPIMAGE_ACCEPTANCE") != "1" {
		t.Skip("run make check-appimage on Linux for finished-image acceptance")
	}
	if runtime.GOOS != "linux" || linuxArchitecture() == "" {
		t.Fatal("AppImage acceptance requires a native Linux amd64 or arm64 host")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Minute)
	defer cancel()
	repo, err := filepath.Abs("../../..")
	if err != nil {
		t.Fatal(err)
	}
	root, destination, caller := t.TempDir(), t.TempDir(), t.TempDir()
	source := filepath.Join(t.TempDir(), "Portable Builder")
	xdg := filepath.Join(t.TempDir(), "application data")
	for key, value := range map[string]string{
		"APPIMAGE": "", "APPDIR": "", "OWD": "", "APPIMAGE_EXTRACT_AND_RUN": "",
		"LD_LIBRARY_PATH": "", "LD_PRELOAD": "", "AR_USER_DATA_DIR": "",
		"AR_HEADLESS": "1", "XDG_DATA_HOME": xdg,
	} {
		t.Setenv(key, value)
		// AppImage's runtime treats OWD being present (even empty) differently
		// from it being absent. Register restoration, then genuinely unset it.
		if value == "" {
			if err := os.Unsetenv(key); err != nil {
				t.Fatal(err)
			}
		}
	}
	run := func(t *testing.T, dir, executable string, env []string, args ...string) string {
		t.Helper()
		command := exec.CommandContext(ctx, executable, args...)
		command.Dir = dir
		command.Env = appImageCheckEnvironment(env)
		command.WaitDelay = 2 * time.Second
		var stdout, stderr bytes.Buffer
		command.Stdout, command.Stderr = &stdout, &stderr
		if err := command.Run(); err != nil {
			// The launcher redirects the game's diagnostics to the chosen data
			// root. Include those logs in the failure, before TempDir cleanup.
			for _, base := range []string{destination, caller, xdg} {
				filepath.WalkDir(base, func(path string, entry fs.DirEntry, err error) error {
					if err == nil && !entry.IsDir() && strings.HasSuffix(path, ".log") {
						t.Logf("%s:\n%s", path, read(t, path))
					}
					return nil
				})
			}
			t.Fatalf("%s %q: %v\nstdout:\n%s\nstderr:\n%s", executable, args, err, &stdout, &stderr)
		}
		if stderr.Len() > 0 {
			t.Logf("%s: %s", filepath.Base(executable), &stderr)
		}
		return stdout.String()
	}

	// Match the portable installer's utils/tools layout and exercise default
	// tool discovery, not a separate packaging implementation or PATH lookup.
	toolsDir := filepath.Join(source, "utils/tools")
	helper := filepath.Join(toolsDir, "actraiser-builder")
	for key, leaf := range map[string]string{"AR_APPIMAGE_TOOL": "appimagetool", "AR_APPIMAGE_RUNTIME": "appimage-runtime"} {
		if os.Getenv(key) == "" {
			t.Fatalf("%s is required; use make check-appimage to stage the release-pinned tools", key)
		}
		if err := copyFileAtomic(os.Getenv(key), filepath.Join(toolsDir, leaf), 0755); err != nil {
			t.Fatal(err)
		}
	}
	run(t, repo, "go", []string{"CGO_ENABLED=0"}, "-C", "installer", "build", "-trimpath", "-o", helper, "./cmd/actraiser-builder")
	rom := nativePackageFixture(t, root)
	font := "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf"
	if err := copyFileAtomic(filepath.Join(repo, font), filepath.Join(root, font), 0644); err != nil {
		t.Fatal(err)
	}
	for _, leaf := range []string{"untouched.txt", "edited.txt", "deleted.txt", "existing.txt"} {
		put(t, filepath.Join(root, "game-assets/hd", leaf), "shipped v1")
	}
	put(t, filepath.Join(root, "config.ini"), "private build settings")
	put(t, filepath.Join(root, "settings.ini"), "private build settings")
	put(t, filepath.Join(source, "base.c"), "int base(void) { return 40; }\n")
	put(t, filepath.Join(source, "middle.c"), "extern int base(void); int middle(void) { return base()+2; }\n")
	run(t, source, "cc", nil, "-shared", "-fPIC", "base.c", "-o", "libbase.so", "-Wl,-soname,libbase.so")
	run(t, source, "cc", nil, "-shared", "-fPIC", "middle.c", "-L.", "-lbase", "-o", "libmiddle.so", "-Wl,-soname,libmiddle.so", "-Wl,-rpath,"+source)
	flags := strings.Fields(run(t, source, "pkg-config", nil, "--cflags", "--libs", "sdl3", "sdl3-ttf"))
	args := []string{filepath.Join(repo, "installer/internal/desktop/testdata/appimage-probe.c"), "-Wall", "-Wextra", "-Werror", "-L.", "-lmiddle", "-Wl,-rpath," + source, "-o", Name}
	run(t, source, "cc", nil, append(args, flags...)...)

	options := PackageOptions{Binary: filepath.Join(source, Name), Builder: helper,
		ROM: rom, Root: root, Destination: destination, Format: "appimage", Version: "acceptance-v1"}
	var packaging bytes.Buffer
	options.Output = &packaging
	artifact, err := Package(ctx, options)
	if err != nil {
		t.Fatalf("portable Builder packaging: %v\n%s", err, &packaging)
	}
	t.Log(packaging.String())
	// Also produce an upgrade with the production replacement/backup path.
	for _, leaf := range []string{"untouched.txt", "edited.txt", "deleted.txt", "existing.txt", "new.txt"} {
		put(t, filepath.Join(root, "game-assets/hd", leaf), "shipped v2")
	}
	options.Replace, options.Version = true, "acceptance-v2"
	packaging.Reset()
	updated, err := Package(ctx, options)
	if err != nil {
		t.Fatalf("replace AppImage: %v\n%s", err, &packaging)
	}
	if updated.Backup == "" {
		t.Fatal("replacement did not retain the old image")
	}
	// A moved/renamed install with spaces must work after all compiler inputs,
	// original shared objects, helper, and packaging tools have been removed.
	imagePath := filepath.Join(destination, "Moved Game.AppImage")
	if err := os.Rename(updated.Backup, imagePath); err != nil {
		t.Fatal(err)
	}
	if err := os.RemoveAll(source); err != nil { // only this test's generated inputs
		t.Fatal(err)
	}
	imageHash := func() string {
		t.Helper()
		hash, err := fileHash(imagePath)
		if err != nil {
			t.Fatal(err)
		}
		return hash
	}
	originalHash := imageHash()
	imageRun := func(t *testing.T, env []string, args ...string) string {
		t.Helper()
		return run(t, caller, imagePath, append([]string{"APPIMAGE_EXTRACT_AND_RUN=1"}, env...), args...)
	}
	absent := func(t *testing.T, path string) {
		t.Helper()
		if _, err := os.Lstat(path); !os.IsNotExist(err) {
			t.Fatalf("unexpected file/directory at %s: %v", path, err)
		}
	}
	portable := filepath.Join(destination, "utils")
	global := filepath.Join(xdg, Name)
	t.Run("global-without-sidecar-does-not-import-nearby-data", func(t *testing.T) {
		put(t, filepath.Join(portable, "saves/save.srm"), "player save")
		paths := imageRun(t, nil, "--print-paths")
		if !strings.Contains(paths, "Application: "+imagePath+"\n") || !strings.Contains(paths, "Data: "+global+"\n") {
			t.Fatalf("unexpected runtime paths: %s", paths)
		}
		absent(t, global)
		if got := strings.TrimSpace(imageRun(t, nil, "--prepare-only")); got != global {
			t.Fatalf("global data: %q; want %q", got, global)
		}
		absent(t, filepath.Join(global, "saves/save.srm"))
		absent(t, filepath.Join(global, "config.ini"))
		absent(t, filepath.Join(global, "settings.ini"))
	})
	if err := WritePortableMarker(imagePath, portable); err != nil {
		t.Fatal(err)
	}
	t.Run("portable-preserves-existing-and-modified-player-files", func(t *testing.T) {
		for leaf, content := range map[string]string{"config.ini": "player config", "settings.ini": "player settings", "game-assets/hd/existing.txt": "player art"} {
			put(t, filepath.Join(portable, leaf), content)
		}
		if got := strings.TrimSpace(imageRun(t, nil, "--prepare-only")); got != portable {
			t.Fatalf("sidecar data: %q; want %q", got, portable)
		}
		put(t, filepath.Join(portable, "game-assets/hd/edited.txt"), "player edit")
		if err := os.Remove(filepath.Join(portable, "game-assets/hd/deleted.txt")); err != nil {
			t.Fatal(err)
		}
		imageRun(t, nil, "--prepare-only")
		checkAppImagePlayerFiles(t, portable, "shipped v1")
	})
	t.Run("explicit-options-and-caller-directory", func(t *testing.T) {
		for _, tc := range []struct {
			args []string
			env  []string
			want string
		}{
			{[]string{"--global"}, []string{"AR_USER_DATA_DIR=environment profile"}, global},
			{[]string{"--portable"}, nil, caller},
			{[]string{"--data-dir", "relative profile"}, nil, filepath.Join(caller, "relative profile")},
			{nil, []string{"AR_USER_DATA_DIR=environment profile"}, filepath.Join(caller, "environment profile")},
		} {
			if got := strings.TrimSpace(imageRun(t, tc.env, append(tc.args, "--prepare-only")...)); got != tc.want {
				t.Fatalf("%q with %q: %q; want %q", tc.args, tc.env, got, tc.want)
			}
		}
	})
	t.Run("real-launcher-loads-private-transitive-SDL-and-font-libraries", func(t *testing.T) {
		// Exercise the runtime's CLI extraction switch as well as its env switch.
		imageRun(t, []string{"APPIMAGE_EXTRACT_AND_RUN="}, "--appimage-extract-and-run")
		checkAppImageProbe(t, portable)
	})
	t.Run("fuse-mounted-launch", func(t *testing.T) {
		if os.Getenv("AR_APPIMAGE_TEST_FUSE") != "1" {
			t.Skip("set AR_APPIMAGE_TEST_FUSE=1 on a host with working FUSE to test native mounting too")
		}
		if err := os.Remove(filepath.Join(portable, "appimage-probe.txt")); err != nil && !os.IsNotExist(err) {
			t.Fatal(err)
		}
		imageRun(t, []string{"APPIMAGE_EXTRACT_AND_RUN="})
		checkAppImageProbe(t, portable)
		if !strings.Contains(read(t, filepath.Join(portable, "appimage-probe.txt")), "filesystem=65735546\n") {
			t.Fatal("native launch did not read the ROM from a FUSE filesystem")
		}
	})
	t.Run("read-only-extracted-AppDir-and-private-input-exclusion", func(t *testing.T) {
		extraction := t.TempDir()
		run(t, extraction, imagePath, nil, "--appimage-extract")
		appDir := filepath.Join(extraction, "squashfs-root")
		resources := filepath.Join(appDir, "usr/share", Name)
		for _, leaf := range []string{"seed/saves/save.srm", "seed/config.ini", "seed/settings.ini", "seed/game-assets/languages/sources/private", "seed/game-assets/languages/packs/.arlang-cache/private"} {
			absent(t, filepath.Join(resources, leaf))
		}
		run(t, caller, "desktop-file-validate", nil, filepath.Join(appDir, Name+".desktop"))
		if _, err := os.Stat(filepath.Join(appDir, ".DirIcon")); err != nil {
			t.Fatalf("finished AppImage has no usable .DirIcon: %v", err)
		}
		before := appImageTreeHashes(t, appDir)
		// Restore only this extracted fixture's directory modes for cleanup.
		t.Cleanup(func() {
			filepath.WalkDir(appDir, func(path string, entry fs.DirEntry, err error) error {
				if err == nil && entry.IsDir() {
					return os.Chmod(path, 0755)
				}
				return err
			})
		})
		if err := filepath.WalkDir(appDir, func(path string, entry fs.DirEntry, err error) error {
			if err != nil || entry.Type()&os.ModeSymlink != 0 {
				return err
			}
			info, err := entry.Info()
			if err != nil {
				return err
			}
			return os.Chmod(path, info.Mode().Perm()&^0222)
		}); err != nil {
			t.Fatal(err)
		}
		profile := filepath.Join(caller, "extracted profile")
		run(t, caller, filepath.Join(appDir, "AppRun"), nil, "--data-dir", profile)
		checkAppImageProbe(t, profile)
		command := exec.CommandContext(ctx, filepath.Join(appDir, "AppRun"), "--data-dir", filepath.Join(appDir, "forbidden"), "--prepare-only")
		output, err := command.CombinedOutput()
		if err == nil || !strings.Contains(string(output), "outside the application package") {
			t.Fatalf("package-internal data was not rejected: %v: %s", err, output)
		}
		after := appImageTreeHashes(t, appDir)
		if before != after {
			t.Fatal("launch changed the extracted package")
		}
	})
	if originalHash != imageHash() {
		t.Fatal("launch modified the AppImage")
	}
	t.Run("upgraded-image-keeps-portable-data", func(t *testing.T) {
		// Only test-generated images are overwritten; the production replacement
		// operation above retained the original image in its backup directory.
		if err := copyFileAtomic(artifact.Path, imagePath, 0755); err != nil {
			t.Fatal(err)
		}
		imageRun(t, nil, "--prepare-only")
		checkAppImagePlayerFiles(t, portable, "shipped v2")
		if got := read(t, filepath.Join(portable, "game-assets/hd/new.txt")); got != "shipped v2" {
			t.Fatalf("upgrade did not add new assets: %q", got)
		}
	})
}

// Empty overrides mean unset, including when switching from the runtime's
// environment extraction mode to its command-line extraction mode.
func appImageCheckEnvironment(overrides []string) []string {
	values := map[string]string{}
	for _, entry := range append(os.Environ(), overrides...) {
		key, value, _ := strings.Cut(entry, "=")
		values[key] = value
	}
	var result []string
	for key, value := range values {
		if value != "" {
			result = append(result, key+"="+value)
		}
	}
	return result
}

func checkAppImagePlayerFiles(t *testing.T, root, untouched string) {
	t.Helper()
	for leaf, want := range map[string]string{"config.ini": "player config", "settings.ini": "player settings", "saves/save.srm": "player save", "game-assets/hd/existing.txt": "player art", "game-assets/hd/edited.txt": "player edit", "game-assets/hd/untouched.txt": untouched} {
		if got := read(t, filepath.Join(root, leaf)); got != want {
			t.Fatalf("%s = %q; want %q", leaf, got, want)
		}
	}
	if _, err := os.Lstat(filepath.Join(root, "game-assets/hd/deleted.txt")); !os.IsNotExist(err) {
		t.Fatal("player-deleted asset was recreated")
	}
}

func checkAppImageProbe(t *testing.T, root string) {
	t.Helper()
	report := read(t, filepath.Join(root, "appimage-probe.txt"))
	private := ""
	loaded := map[string]bool{}
	for _, line := range strings.Split(report, "\n") {
		if value, ok := strings.CutPrefix(line, "private="); ok {
			private = value
		}
		if path, ok := strings.CutPrefix(line, "library="); ok {
			name := filepath.Base(path)
			loaded[name] = true
			if glibcLibrary(name) || strings.HasPrefix(name, "linux-vdso.") {
				continue
			}
			if private == "" || !strings.HasPrefix(path, private+"/") {
				t.Fatalf("host library masked a packaging omission: %s\n%s", path, report)
			}
		}
	}
	for _, name := range []string{"libbase.so", "libmiddle.so", "libSDL3.so.0", "libSDL3_ttf.so.0"} {
		if !loaded[name] {
			t.Fatalf("probe did not load %s\n%s", name, report)
		}
	}
	if !strings.Contains(report, "cwd="+root+"\n") {
		t.Fatalf("game did not use the selected data directory: %s", report)
	}
}

func appImageTreeHashes(t *testing.T, root string) string {
	t.Helper()
	var result strings.Builder
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		result.WriteString(path + "\n")
		if entry.Type().IsRegular() {
			hash, err := fileHash(path)
			if err != nil {
				return err
			}
			result.WriteString(hash + "\n")
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return result.String()
}
