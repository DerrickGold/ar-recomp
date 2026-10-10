package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"unsafe"

	"golang.org/x/sys/windows"
)

// These need a native Windows host and its actual ACL-capable filesystem.
// Cross-compiling the test executable is not an execution result.
func TestRuntimeCacheOwnership(t *testing.T) {
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
	if err := os.WriteFile(filepath.Join(cache, ".builder-runtime-cache"), []byte("different owner"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := ensureRuntimeCache(cache); err == nil {
		t.Fatal("adopted conflicting cache marker")
	}
}

func TestLocalSecurityPath(t *testing.T) {
	for _, path := range []string{`C:\Builder Data\runtime`, `D:\` + strings.Repeat(`long directory\`, 30) + `webview`} {
		got, err := localSecurityPath(path)
		if err != nil || got != `\\?\`+path {
			t.Fatalf("extended security path: %q %v", got, err)
		}
	}
	for _, path := range []string{`relative\runtime`, `C:runtime`, `\\server\share\runtime`, `\\.\C:\runtime`} {
		if _, err := localSecurityPath(path); err == nil {
			t.Fatalf("accepted non-local/relative runtime path %q", path)
		}
	}
}

func readRuntimeGrants(t *testing.T, directory string) map[string]windows.ACCESS_MASK {
	t.Helper()
	path, err := localSecurityPath(directory)
	if err != nil {
		t.Fatal(err)
	}
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT, windows.DACL_SECURITY_INFORMATION)
	if err != nil || sd == nil {
		t.Fatalf("read security descriptor for %s: %v", directory, err)
	}
	acl, _, err := sd.DACL()
	if err != nil || acl == nil {
		t.Fatalf("read access list for %s: %v", directory, err)
	}
	grants := map[string]windows.ACCESS_MASK{}
	for i := uint32(0); i < uint32(acl.AceCount); i++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if err := windows.GetAce(acl, i, &ace); err != nil {
			t.Fatal(err)
		}
		if ace.Header.AceType == windows.ACCESS_ALLOWED_ACE_TYPE && ace.Header.AceFlags&windows.INHERIT_ONLY_ACE == 0 {
			sid := (*windows.SID)(unsafe.Pointer(&ace.SidStart))
			grants[sid.String()] |= ace.Mask
		}
	}
	return grants
}

// The cache holds only the Builder's own tools now; no AppContainer (WebView2
// sandbox) grant is needed or wanted anywhere in it.
func TestRuntimeCacheStaysPrivateOnLongPaths(t *testing.T) {
	for _, longRoot := range []bool{false, true} {
		name := "long child"
		if longRoot {
			name = "long runtime root"
		}
		t.Run(name, func(t *testing.T) {
			parent := t.TempDir()
			if longRoot {
				for len(parent) < 300 {
					parent = filepath.Join(parent, strings.Repeat("nested folder ", 4)+"directory")
				}
			}
			cache := filepath.Join(parent, "private cache")
			if err := ensureRuntimeCache(cache); err != nil {
				t.Fatal(err)
			}
			deep := filepath.Join(cache, "stage", "payload")
			for len(deep) < 350 {
				deep = filepath.Join(deep, strings.Repeat("bundled tool ", 3)+"folder")
			}
			if err := os.MkdirAll(deep, 0700); err != nil {
				t.Fatal(err)
			}
			file := filepath.Join(deep, "zig.exe")
			if err := os.WriteFile(file, []byte("synthetic tool"), 0600); err != nil {
				t.Fatal(err)
			}
			user, err := windows.GetCurrentProcessToken().GetTokenUser()
			if err != nil {
				t.Fatal(err)
			}
			for _, path := range []string{cache, filepath.Join(cache, "stage"), deep, file} {
				grants := readRuntimeGrants(t, path)
				ownerAccess := windows.ACCESS_MASK(windows.FILE_GENERIC_READ | windows.FILE_GENERIC_WRITE | windows.FILE_GENERIC_EXECUTE | windows.DELETE | windows.WRITE_DAC | windows.WRITE_OWNER)
				for _, sid := range []string{user.User.Sid.String(), "S-1-5-18"} {
					if grants[sid]&ownerAccess != ownerAccess {
						t.Errorf("lost owner/SYSTEM access at %s: %v", path, grants)
					}
				}
				for _, sid := range []string{"S-1-15-2-2", "S-1-15-2-1"} {
					if grants[sid] != 0 {
						t.Errorf("unexpected sandbox grant at %s: %#x", path, grants[sid])
					}
				}
			}
		})
	}
}
