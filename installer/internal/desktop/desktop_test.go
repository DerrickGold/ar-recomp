package desktop

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func put(t *testing.T, path, value string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(value), 0644); err != nil {
		t.Fatal(err)
	}
}

func read(t *testing.T, path string) string {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func TestStorageSelectionPreservesPortableDataAndIgnoresWritability(t *testing.T) {
	base := t.TempDir()
	layout := Layout{Artifact: filepath.Join(base, Name+".app")}
	cwd := filepath.Join(base, "caller")
	env := map[string]string{"HOME": filepath.Join(base, "home"), "XDG_DATA_HOME": "relative-is-invalid"}
	getenv := func(key string) string { return env[key] }
	for _, tc := range []struct {
		name, goos, want string
		options          StorageOptions
	}{
		{"writable app still global", "darwin", filepath.Join(env["HOME"], "Library", "Application Support", Name), StorageOptions{}},
		{"invalid XDG falls back", "linux", filepath.Join(env["HOME"], ".local", "share", Name), StorageOptions{}},
		{"portable means caller cwd", "darwin", cwd, StorageOptions{Portable: true}},
		{"explicit relative path", "linux", filepath.Join(cwd, "profile"), StorageOptions{DataDir: "profile"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := ResolveDataDirectory(layout, tc.options, cwd, tc.goos, getenv)
			if err != nil || got != tc.want {
				t.Fatalf("got %q, %v; want %q", got, err, tc.want)
			}
		})
	}
	root := filepath.Join(base, "utils")
	put(t, filepath.Join(root, "saves", "save.srm"), "existing-save")
	if err := WritePortableMarker(layout.Artifact, root); err != nil {
		t.Fatal(err)
	}
	got, err := ResolveDataDirectory(layout, StorageOptions{}, cwd, "darwin", getenv)
	if err != nil || got != root {
		t.Fatalf("existing data moved: %q %v", got, err)
	}
	if read(t, filepath.Join(got, "saves", "save.srm")) != "existing-save" {
		t.Fatal("save changed")
	}
	got, err = ResolveDataDirectory(layout, StorageOptions{Global: true}, cwd, "darwin", getenv)
	if err != nil || got == root {
		t.Fatal("--global did not override the sidecar")
	}
	env["AR_USER_DATA_DIR"] = "env-profile"
	got, err = ResolveDataDirectory(layout, StorageOptions{}, cwd, "darwin", getenv)
	if err != nil || got != filepath.Join(cwd, "env-profile") {
		t.Fatal("environment override did not win")
	}
	if _, err := ResolveDataDirectory(layout, StorageOptions{Portable: true, Global: true}, cwd, "darwin", getenv); err == nil {
		t.Fatal("conflicting modes accepted")
	}
	delete(env, "AR_USER_DATA_DIR")
	put(t, layout.Artifact+".portable", "../elsewhere")
	if _, err := ResolveDataDirectory(layout, StorageOptions{}, cwd, "darwin", getenv); err == nil {
		t.Fatal("escaping portable sidecar accepted")
	}
	delete(env, "HOME")
	if _, err := ResolveDataDirectory(layout, StorageOptions{Global: true}, cwd, "darwin", getenv); err == nil {
		t.Fatal("missing home fell back to cwd")
	}
}

func TestSeedUpgradePreservesEditedDeletedAndExistingUserFiles(t *testing.T) {
	resources, root := t.TempDir(), t.TempDir()
	for _, leaf := range []string{"game-assets/fonts/font.ttf", "game-assets/edited.ogg", "game-assets/deleted.ogg", "game-assets/existing.ogg", "defaults/config.ini"} {
		put(t, filepath.Join(resources, "seed", leaf), "v1")
	}
	put(t, filepath.Join(root, "game-assets/existing.ogg"), "player-original")
	put(t, filepath.Join(root, "settings.ini"), "player-settings")
	put(t, filepath.Join(root, "saves/save.srm"), "player-save")
	if err := InitializeData(resources, root); err != nil {
		t.Fatal(err)
	}
	put(t, filepath.Join(root, "game-assets/edited.ogg"), "player-edit")
	if err := os.Remove(filepath.Join(root, "game-assets/deleted.ogg")); err != nil {
		t.Fatal(err)
	}
	for _, leaf := range []string{"game-assets/fonts/font.ttf", "game-assets/edited.ogg", "game-assets/deleted.ogg", "game-assets/existing.ogg", "defaults/config.ini"} {
		put(t, filepath.Join(resources, "seed", leaf), "v2")
	}
	if err := InitializeData(resources, root); err != nil {
		t.Fatal(err)
	}
	for leaf, want := range map[string]string{"game-assets/fonts/font.ttf": "v2", "game-assets/edited.ogg": "player-edit", "game-assets/existing.ogg": "player-original", "defaults/config.ini": "v2", "settings.ini": "player-settings", "saves/save.srm": "player-save"} {
		if got := read(t, filepath.Join(root, leaf)); got != want {
			t.Fatalf("%s = %q; want %q", leaf, got, want)
		}
	}
	if _, err := os.Stat(filepath.Join(root, "game-assets/deleted.ogg")); !os.IsNotExist(err) {
		t.Fatal("deleted asset resurrected")
	}
	if err := InitializeData(resources, root); err != nil {
		t.Fatal(err)
	}
}

func TestSeedFailureDoesNotReplaceHistoryOrFollowSymlinks(t *testing.T) {
	resources, root, elsewhere := t.TempDir(), t.TempDir(), t.TempDir()
	put(t, filepath.Join(resources, "seed/game-assets/hd/art"), "new")
	put(t, filepath.Join(elsewhere, "art"), "keep")
	put(t, filepath.Join(root, seedStateName), "{}")
	if err := os.MkdirAll(filepath.Join(root, "game-assets"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(elsewhere, filepath.Join(root, "game-assets/hd")); err != nil {
		t.Skip(err)
	}
	if err := InitializeData(resources, root); err == nil {
		t.Fatal("followed symlink")
	}
	if read(t, filepath.Join(elsewhere, "art")) != "keep" {
		t.Fatal("external data replaced")
	}
	if read(t, filepath.Join(root, seedStateName)) != "{}" {
		t.Fatal("failed initialization advanced baseline")
	}
}

func TestBundleIdentityAndPhysicalStorageBoundary(t *testing.T) {
	parent := t.TempDir()
	app := filepath.Join(parent, "Renamed Game.app")
	resources := filepath.Join(app, "Contents/Resources")
	binary := filepath.Join(app, "Contents/MacOS/actraiser-builder")
	put(t, binary, "helper")
	put(t, filepath.Join(filepath.Dir(binary), Name), "game")
	if err := os.Chmod(filepath.Join(filepath.Dir(binary), Name), 0755); err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(Manifest{Format: 1, Product: Name})
	put(t, filepath.Join(resources, markerName), string(raw))
	layout, err := Discover(binary, "")
	canonical, _ := filepath.EvalSymlinks(app)
	if err != nil || layout.Artifact != canonical {
		t.Fatalf("discover: %+v %v", layout, err)
	}
	if _, err := rejectPackageData(layout, filepath.Join(resources, "data")); err == nil {
		t.Fatal("writes inside app accepted")
	}
	link := filepath.Join(parent, "alias")
	if err := os.Symlink(resources, link); err != nil {
		t.Skip(err)
	}
	if _, err := rejectPackageData(layout, filepath.Join(link, "new-data")); err == nil {
		t.Fatal("symlink into app accepted")
	}
	if _, err := rejectPackageData(layout, filepath.Join(parent, "user-data")); err != nil {
		t.Fatal(err)
	}
	put(t, filepath.Join(resources, markerName), `{"format":999,"product":"Other"}`)
	if _, err := Discover(binary, ""); err == nil {
		t.Fatal("wrong identity accepted")
	}
}

func TestPublishFailureAndReplacementRetainPreviousApplication(t *testing.T) {
	root := t.TempDir()
	final := filepath.Join(root, Name+".app")
	marker, _ := json.Marshal(Manifest{Format: 1, Product: Name})
	put(t, filepath.Join(final, "Contents/Resources", markerName), string(marker))
	put(t, filepath.Join(final, "old"), "previous")
	if _, err := publishArtifact(filepath.Join(root, "missing-stage"), final, true); err == nil {
		t.Fatal("missing stage succeeded")
	}
	if read(t, filepath.Join(final, "old")) != "previous" {
		t.Fatal("failed build lost prior output")
	}
	stage := filepath.Join(root, "stage.app")
	put(t, filepath.Join(stage, "new"), "replacement")
	result, err := publishArtifact(stage, final, true)
	if err != nil {
		t.Fatal(err)
	}
	if read(t, filepath.Join(result.Backup, "old")) != "previous" || read(t, filepath.Join(final, "new")) != "replacement" {
		t.Fatal("replacement/backup incorrect")
	}
}

func TestLaunchArgumentsRetainCallerRelativePaths(t *testing.T) {
	options, err := ParseLaunchOptions([]string{"--data-dir=profile", "--config", "custom.ini", "rom.sfc"})
	if err != nil {
		t.Fatal(err)
	}
	args, err := gameArguments(options.GameArgs, "/caller", "/profile", "/app/Resources")
	if err != nil || strings.Join(args, "|") != "--config|/caller/custom.ini|/caller/rom.sfc" {
		t.Fatalf("args %q %v", args, err)
	}
	args, err = gameArguments(nil, "/caller", "/profile", "/app/Resources")
	if err != nil || strings.Join(args, "|") != "/app/Resources/user-rom.sfc|--config|/profile/config.ini" {
		t.Fatalf("args %q %v", args, err)
	}
}

func TestLinuxDependenciesIncludeTransitiveLibrariesAndRejectMissingOnes(t *testing.T) {
	deps, err := parseLdd("linux-vdso.so.1 (0xffff)\nlibSDL3.so.0 => /sdk/lib/libSDL3.so.0 (0x123)\nlibfreetype.so.6 => /usr/lib/libfreetype.so.6 (0x456)\nlibc.so.6 => /lib/libc.so.6 (0x789)\n/lib64/ld-linux-x86-64.so.2 (0x111)\n")
	if err != nil || len(deps) != 2 || deps["libfreetype.so.6"] == "" {
		t.Fatalf("deps %v %v", deps, err)
	}
	if _, err := parseLdd("libSDL3_ttf.so.0 => not found\n"); err == nil {
		t.Fatal("missing library accepted")
	}
}
