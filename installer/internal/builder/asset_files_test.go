package builder

import (
	"os"
	"path/filepath"
	"testing"
)

func TestWriteAtomicFileReplacesExisting(t *testing.T) {
	path := filepath.Join(t.TempDir(), "manifest.ini")
	if err := os.WriteFile(path, []byte("previous"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := writeAtomicFile(path, []byte("replacement")); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil || string(data) != "replacement" {
		t.Fatalf("replacement = %q, %v", data, err)
	}
	entries, err := os.ReadDir(filepath.Dir(path))
	if err != nil || len(entries) != 1 {
		t.Fatalf("staging files remain: %v, %v", entries, err)
	}
}
