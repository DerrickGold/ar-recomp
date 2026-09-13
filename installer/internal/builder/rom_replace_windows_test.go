package builder

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

func TestStoreROMLockedDestinationPreservesPreviousCopy(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "user-rom.sfc")
	if err := os.WriteFile(path, []byte("old synthetic ROM"), 0600); err != nil {
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
	_, replaceErr := storeROM(root, strings.NewReader("new synthetic ROM"))
	syscall.CloseHandle(handle)
	if replaceErr == nil {
		t.Fatal("locked replacement should fail")
	}
	got, err := os.ReadFile(path)
	if err != nil || string(got) != "old synthetic ROM" {
		t.Fatalf("failed replacement lost ROM: %q %v", got, err)
	}
	paths, _ := filepath.Glob(filepath.Join(root, ".snesbuild-rom-*"))
	if len(paths) != 0 {
		t.Fatalf("staging files left behind: %v", paths)
	}
}
