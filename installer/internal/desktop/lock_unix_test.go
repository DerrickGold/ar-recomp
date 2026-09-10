//go:build darwin || linux

package desktop

import (
	"os"
	"path/filepath"
	"testing"
)

func TestInitializationLockContentionAndReuse(t *testing.T) {
	root := t.TempDir()
	unlock, err := lockInitialization(root)
	if err != nil {
		t.Fatal(err)
	}
	if second, err := lockInitialization(root); err == nil {
		second()
		t.Error("concurrent initializer acquired the lock")
	}
	unlock()
	unlock, err = lockInitialization(root)
	if err != nil {
		t.Fatalf("released lock file prevents later launches: %v", err)
	}
	unlock()
}

func TestInitializationLockRejectsSymlink(t *testing.T) {
	root := t.TempDir()
	if err := os.Symlink(filepath.Join(t.TempDir(), "other"), filepath.Join(root, ".actraiser-initialize.lock")); err != nil {
		t.Fatal(err)
	}
	if unlock, err := lockInitialization(root); err == nil {
		unlock()
		t.Fatal("followed a symlink for the initialization lock")
	}
}
