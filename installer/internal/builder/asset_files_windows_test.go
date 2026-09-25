package builder

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
	"testing"
)

func TestWriteAtomicFileLockedDestinationPreservesOriginal(t *testing.T) {
	path := filepath.Join(t.TempDir(), "manifest.ini")
	if err := os.WriteFile(path, []byte("previous"), 0600); err != nil {
		t.Fatal(err)
	}
	wide, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		t.Fatal(err)
	}
	handle, err := syscall.CreateFile(wide, syscall.GENERIC_READ, syscall.FILE_SHARE_READ, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.CloseHandle(handle)
	err = writeAtomicFile(path, []byte("replacement"))
	var renameError *os.LinkError
	if !errors.As(err, &renameError) || renameError.Old == path {
		t.Fatalf("expected the staging rename failure, got %v", err)
	}
	data, err := os.ReadFile(path)
	if err != nil || string(data) != "previous" {
		t.Fatalf("failed replacement lost original: %q, %v", data, err)
	}
	entries, err := os.ReadDir(filepath.Dir(path))
	if err != nil || len(entries) != 1 {
		t.Fatalf("staging or backup files remain: %v, %v", entries, err)
	}
}
