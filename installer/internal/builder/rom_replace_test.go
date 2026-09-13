package builder

import (
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestStoreROMReplacesExistingCopy(t *testing.T) {
	root := t.TempDir()
	for _, contents := range []string{"original synthetic ROM", "replacement synthetic ROM"} {
		path, err := storeROM(root, strings.NewReader(contents))
		if err != nil {
			t.Fatal(err)
		}
		got, err := os.ReadFile(path)
		if err != nil || string(got) != contents {
			t.Fatalf("ROM = %q, %v", got, err)
		}
	}
}

type disappearingStagingROM struct {
	t    *testing.T
	root string
	sent bool
}

func (r *disappearingStagingROM) Read(p []byte) (int, error) {
	if !r.sent {
		r.sent = true
		return copy(p, []byte("new synthetic ROM")), nil
	}
	paths, err := filepath.Glob(filepath.Join(r.root, ".snesbuild-rom-*"))
	if err != nil || len(paths) != 1 {
		r.t.Fatalf("staging lookup: %v %v", paths, err)
	}
	if err := os.Remove(paths[0]); err != nil {
		r.t.Fatal(err)
	}
	return 0, io.EOF
}

func TestStoreROMPreservesPreviousCopyOnRenameFailure(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("open staging file cannot be unlinked on Windows; see locked destination test")
	}
	root := t.TempDir()
	prior := filepath.Join(root, "user-rom.sfc")
	if err := os.WriteFile(prior, []byte("old synthetic ROM"), 0600); err != nil {
		t.Fatal(err)
	}
	_, err := storeROM(root, &disappearingStagingROM{t: t, root: root})
	if err == nil {
		t.Fatal("missing staging file should fail")
	}
	got, err := os.ReadFile(prior)
	if err != nil || string(got) != "old synthetic ROM" {
		t.Fatalf("failed replacement lost ROM: %q %v", got, err)
	}
	paths, _ := filepath.Glob(filepath.Join(root, ".snesbuild-rom-*"))
	if len(paths) != 0 {
		t.Fatalf("staging files left behind: %v", paths)
	}
}
