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

func TestScratchStartsEmptyEveryAttemptAndPreservesOldCaches(t *testing.T) {
	work := filepath.Join(t.TempDir(), "workspace")
	first, err := Scratch(work)
	if err != nil {
		t.Fatal(err)
	}
	put(t, filepath.Join(first, "objects", "stale.o"), "old object")
	put(t, filepath.Join(first, "generated", "stale.c"), "old source")
	again, err := Scratch(work)
	if err != nil || first == again {
		t.Fatalf("reused scratch: %s, %v", again, err)
	}
	entries, err := os.ReadDir(again)
	if err != nil || len(entries) != 0 {
		t.Fatalf("new attempt is not empty: %v %v", entries, err)
	}
	if data, err := os.ReadFile(filepath.Join(first, "objects", "stale.o")); err != nil || string(data) != "old object" {
		t.Fatal("touched a different/possibly active attempt", err)
	}
	generic, err := Scratch("")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(generic) })
	if entries, err := os.ReadDir(generic); err != nil || len(entries) != 0 {
		t.Fatal("generic scratch not empty", entries, err)
	}
}

func TestScratchRejectsRedirectedBuildDirectory(t *testing.T) {
	work := filepath.Join(t.TempDir(), "workspace")
	if err := Prepare(work); err != nil {
		t.Fatal(err)
	}
	outside := t.TempDir()
	if err := os.Symlink(outside, filepath.Join(work, "build")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	if _, err := Scratch(work); err == nil {
		t.Fatal("accepted a redirected build directory")
	}
	if entries, err := os.ReadDir(outside); err != nil || len(entries) != 0 {
		t.Fatal("wrote through redirect", entries, err)
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
