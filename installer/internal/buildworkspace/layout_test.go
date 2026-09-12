package buildworkspace

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func put(t *testing.T, path, data string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(data), 0600); err != nil {
		t.Fatal(err)
	}
}

func TestPrepareCreatesOnlyMarkerAndPreservesLegacy(t *testing.T) {
	work := filepath.Join(t.TempDir(), "workspace")
	if err := Prepare(work); err != nil {
		t.Fatal(err)
	}
	entries, err := os.ReadDir(work)
	if err != nil || len(entries) != 1 || entries[0].Name() != Marker {
		t.Fatalf("initial workspace: %v, %v", entries, err)
	}
	if err := Prepare(work); err != nil {
		t.Fatal(err)
	}
	legacy := t.TempDir()
	stamp := strings.Repeat("a", 64) + "\n"
	put(t, filepath.Join(legacy, ".builder-payload"), stamp)
	put(t, filepath.Join(legacy, "utils", "config.ini"), "user settings")
	if err := Prepare(legacy); err != nil {
		t.Fatal(err)
	}
	for path, want := range map[string]string{".builder-payload": stamp, "utils/config.ini": "user settings"} {
		data, err := os.ReadFile(filepath.Join(legacy, filepath.FromSlash(path)))
		if err != nil || string(data) != want {
			t.Fatalf("legacy data changed: %s: %q %v", path, data, err)
		}
	}
	if err := Prepare(t.TempDir()); err == nil {
		t.Fatal("adopted unmanaged directory")
	}
}

func TestScratchKeysPayloadAndROMAndRejectsRedirects(t *testing.T) {
	work := filepath.Join(t.TempDir(), "workspace")
	rom := filepath.Join(t.TempDir(), "input.sfc")
	put(t, rom, "first ROM")
	a := strings.Repeat("a", 64)
	first, err := Scratch(work, a, rom)
	if err != nil {
		t.Fatal(err)
	}
	again, err := Scratch(work, a, rom)
	if err != nil || first != again {
		t.Fatalf("unstable scratch: %s, %v", again, err)
	}
	other, err := Scratch(work, strings.Repeat("b", 64), rom)
	if err != nil || first == other {
		t.Fatalf("payload cache collision: %s %v", other, err)
	}
	put(t, rom, "second ROM")
	otherROM, err := Scratch(work, a, rom)
	if err != nil || first == otherROM {
		t.Fatalf("ROM cache collision: %s %v", otherROM, err)
	}
	if err := os.Symlink(t.TempDir(), filepath.Join(otherROM, "generated")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	if _, err := Scratch(work, a, rom); err == nil {
		t.Fatal("followed generated-source redirect")
	}
}

func TestSeparateResolvesSymlinkedAncestors(t *testing.T) {
	root := t.TempDir()
	alias := filepath.Join(t.TempDir(), "alias")
	if err := os.Symlink(root, alias); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	for _, other := range []string{root, filepath.Join(root, "new"), filepath.Join(alias, "new", "work")} {
		if err := Separate(root, other); err == nil {
			t.Fatalf("accepted overlap %s", other)
		}
	}
	if err := Separate(root, t.TempDir(), filepath.Join(t.TempDir(), "output")); err != nil {
		t.Fatal(err)
	}
}
