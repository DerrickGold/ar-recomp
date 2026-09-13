package desktop

import (
	"encoding/json"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestLocalDataPathsAcceptNativeSeparatorsAndRejectEscapes(t *testing.T) {
	for _, path := range []string{
		"defaults/config.ini",
		filepath.Join("defaults", "config.ini"),
		filepath.Join("game-assets", "languages", "packs", "French pack.arlang"),
		filepath.Join("tools", "actraiser-builder.exe"),
	} {
		if !localPath(path, false) {
			t.Errorf("rejected local native path %q", path)
		}
	}
	for _, path := range []string{"", "..", "../outside", "nested/../../outside", filepath.Join("..", "outside"), t.TempDir(), "bad\nname", "bad\rname", "bad\x00name"} {
		if localPath(path, true) || localPath(path, false) {
			t.Errorf("accepted unsafe path %q", path)
		}
	}
	if !localPath(".", true) || localPath(".", false) {
		t.Fatal("dot must be allowed only for directory containment/sidecars")
	}
	// These literal Windows paths exercise the reported failure on Windows.
	// On Unix, keep rejecting backslashes instead of interpreting them as '/'.
	for _, path := range []string{`defaults\config.ini`, `game-assets\languages/packs\fr.arlang`} {
		if got, want := localPath(path, false), runtime.GOOS == "windows"; got != want {
			t.Errorf("native separator handling for %q: got %v, want %v", path, got, want)
		}
	}
	for _, path := range []string{`..\outside`, `defaults\..\..\outside`, `C:\outside`, `\outside`, `\\server\share\file`, `\\?\C:\outside`} {
		if localPath(path, true) {
			t.Errorf("accepted Windows escape %q", path)
		}
	}
	if runtime.GOOS == "windows" {
		for _, path := range []string{`C:relative`, `defaults\config.ini:stream`, `NUL`, `defaults\CON.txt`, `game-assets\LPT1`} {
			if localPath(path, true) {
				t.Errorf("accepted Windows drive/device/stream path %q", path)
			}
		}
	}
}

func TestNativeDataPathsSeedDefaultsAssetsAndArchiveHelper(t *testing.T) {
	resources, output := t.TempDir(), t.TempDir()
	files := []string{"defaults/config.ini", "game-assets/manifest.ini", "game-assets/languages/packs/example.arlang"}
	for _, leaf := range files {
		put(t, filepath.Join(resources, "seed", filepath.FromSlash(leaf)), "synthetic "+leaf)
	}
	if err := InitializeData(resources, output); err != nil {
		t.Fatal(err)
	}
	for _, leaf := range files {
		if got := read(t, filepath.Join(output, filepath.FromSlash(leaf))); got != "synthetic "+leaf {
			t.Fatalf("seeded %s: %q", leaf, got)
		}
	}
	for _, leaf := range []string{"saves", "game-assets/languages/packs", "game-assets/audio", "game-assets/hd"} {
		if info, err := os.Stat(filepath.Join(output, filepath.FromSlash(leaf))); err != nil || !info.IsDir() {
			t.Fatalf("missing initialized directory %s: %v", leaf, err)
		}
	}
	var history map[string]string
	if err := json.Unmarshal([]byte(read(t, filepath.Join(output, seedStateName))), &history); err != nil {
		t.Fatal(err)
	}
	for _, leaf := range files {
		if history[leaf] == "" {
			t.Errorf("missing portable history key %s", leaf)
		}
	}
	for key := range history {
		if strings.Contains(key, `\`) {
			t.Errorf("native separator leaked into portable seed history: %s", key)
		}
	}
	helper := filepath.Join(t.TempDir(), "actraiser-builder.exe")
	put(t, helper, "synthetic helper")
	if err := InstallArchiveHelper(helper, output); err != nil {
		t.Fatal(err)
	}
	if read(t, filepath.Join(output, "tools", "actraiser-builder.exe")) != "synthetic helper" {
		t.Fatal("runtime archive helper missing")
	}
}

func TestNestedPortableMarkerUsesNativeContainmentAndPortableContents(t *testing.T) {
	base := t.TempDir()
	layout := Layout{Artifact: filepath.Join(base, Name+".app")}
	data := filepath.Join(base, "profiles", "Player One")
	if err := WritePortableMarker(layout.Artifact, data); err != nil {
		t.Fatal(err)
	}
	if got := read(t, layout.Artifact+".portable"); got != "profiles/Player One\n" {
		t.Fatalf("sidecar must stay relocatable across systems: %q", got)
	}
	got, err := ResolveDataDirectory(layout, StorageOptions{}, t.TempDir(), runtime.GOOS, func(string) string { return "" })
	if err != nil || got != data {
		t.Fatalf("nested portable data: %q %v", got, err)
	}
}
