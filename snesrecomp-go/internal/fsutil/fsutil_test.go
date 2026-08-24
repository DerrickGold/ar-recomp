package fsutil

import (
	"os"
	"path/filepath"
	"testing"
)

func TestDirectoryExists(t *testing.T) {
	directory := t.TempDir()
	if !DirectoryExists(directory) {
		t.Fatalf("DirectoryExists(%q) = false for a directory", directory)
	}

	file := filepath.Join(directory, "file")
	if err := os.WriteFile(file, []byte("test"), 0o600); err != nil {
		t.Fatal(err)
	}
	if DirectoryExists(file) {
		t.Fatalf("DirectoryExists(%q) = true for a regular file", file)
	}
	if DirectoryExists(filepath.Join(directory, "missing")) {
		t.Fatal("DirectoryExists returned true for a missing path")
	}
}
