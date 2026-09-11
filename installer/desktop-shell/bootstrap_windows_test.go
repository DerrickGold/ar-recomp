package main

import (
	"os"
	"path/filepath"
	"testing"
)

// These need a native Windows host and its actual ACL-capable filesystem.
// Cross-compiling the test executable is not an execution result.
func TestRuntimeCacheOwnershipAndACLPreparation(t *testing.T) {
	root := t.TempDir()
	unmanaged := filepath.Join(root, "existing")
	if err := os.Mkdir(unmanaged, 0700); err != nil {
		t.Fatal(err)
	}
	if err := ensureRuntimeCache(unmanaged); err == nil {
		t.Fatal("adopted unmanaged directory")
	}
	cache := filepath.Join(root, "managed cache")
	if err := ensureRuntimeCache(cache); err != nil {
		t.Fatal(err)
	}
	if err := ensureRuntimeCache(cache); err != nil {
		t.Fatal("warm cache:", err)
	}
	webview := filepath.Join(cache, "test runtime")
	if err := os.Mkdir(webview, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(webview, "fixture.txt"), []byte("test"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := grantWebviewReadAccess(webview); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cache, ".builder-runtime-cache"), []byte("different owner"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := ensureRuntimeCache(cache); err == nil {
		t.Fatal("adopted conflicting cache marker")
	}
}
