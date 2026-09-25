package regionalmedia

import (
	"os"
	"path/filepath"
	"syscall"
	"testing"
)

func TestDonorWindowsPaths(t *testing.T) {
	for _, tc := range []struct{ path, want string }{
		{`C:\Media\jp.armedia`, `\\?\C:\Media\jp.armedia`},
		{`\\server\share\jp.armedia`, `\\?\UNC\server\share\jp.armedia`},
		{`\\?\C:\Media\jp.armedia`, `\\?\C:\Media\jp.armedia`},
		{`\\?\UNC\server\share\jp.armedia`, `\\?\UNC\server\share\jp.armedia`},
	} {
		wide, err := donorWindowsPath(tc.path)
		if err != nil {
			t.Fatal(err)
		}
		if got := syscall.UTF16ToString(wide); got != tc.want {
			t.Fatalf("path %q = %q, want %q", tc.path, got, tc.want)
		}
	}
}

func TestWindowsDonorPublicationLongPath(t *testing.T) {
	root := t.TempDir()
	for len(root) < 300 {
		root = filepath.Join(root, "regional-media-long-path")
	}
	testDonorPublication(t, filepath.Join(root, "日本 🎮"))
}

func TestWindowsDonorPublicationOnSelectedDrive(t *testing.T) {
	parent := os.Getenv("AR_WINDOWS_TEST_MEDIA_ROOT")
	if parent == "" {
		t.Skip("set AR_WINDOWS_TEST_MEDIA_ROOT to a writable folder on exFAT or FAT32")
	}
	root, err := os.MkdirTemp(parent, "actraiser-media-test-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(root) })
	testDonorPublication(t, filepath.Join(root, "Regional media 日本 🎮"))
}
