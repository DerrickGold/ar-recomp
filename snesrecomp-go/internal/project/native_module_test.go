package project

import (
	"os"
	"path/filepath"
	"testing"
)

func TestPrivateRuntimeNeverFallsBackToExecutableArchive(t *testing.T) {
	root := t.TempDir()
	archive, err := VendedRuntimeArchivePath(root, "x86_64-linux-gnu")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(archive), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(root, "include"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(archive, []byte("normal archive"), 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := selectRuntimeInputsForModule(root, "x86_64-linux-gnu", true); err == nil {
		t.Fatal("normal non-isolated runtime used for a module")
	}
	private := moduleRuntimePath(archive)
	if err := os.WriteFile(private, []byte("private archive"), 0644); err != nil {
		t.Fatal(err)
	}
	module, err := selectRuntimeInputsForModule(root, "x86_64-linux-gnu", true)
	if err != nil || module.Archive != private {
		t.Fatal(module, err)
	}
	normal, err := selectRuntimeInputs(root, "x86_64-linux-gnu")
	if err != nil || normal.Archive != archive {
		t.Fatal(normal, err)
	}
}
