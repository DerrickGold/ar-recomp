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

func TestWebviewACLHandlesLongPathsAndPreservesPrivateCache(t *testing.T) {
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
			webview := filepath.Join(cache, "stage", "webview")
			deep := webview
			for len(deep) < 350 {
				deep = filepath.Join(deep, strings.Repeat("runtime resource ", 3)+"folder")
			}
			if err := os.MkdirAll(deep, 0700); err != nil {
				t.Fatal(err)
			}
			existing := filepath.Join(deep, "FloatingComposerPageStyles.xbf")
			if err := os.WriteFile(existing, []byte("synthetic runtime"), 0600); err != nil {
				t.Fatal(err)
			}
			payload := filepath.Join(cache, "stage", "payload")
			if err := os.Mkdir(payload, 0700); err != nil {
				t.Fatal(err)
			}
			for i := 0; i < 2; i++ {
				if err := grantWebviewReadAccess(webview); err != nil {
					t.Fatalf("grant attempt %d: %v", i, err)
				}
			}
			future := filepath.Join(deep, "new-runtime-file.txt")
			if err := os.WriteFile(future, []byte("inherits permissions"), 0600); err != nil {
				t.Fatal(err)
			}
			user, err := windows.GetCurrentProcessToken().GetTokenUser()
			if err != nil {
				t.Fatal(err)
			}
			for _, path := range []string{cache, filepath.Dir(webview), payload, webview, deep, existing, future} {
				grants := readRuntimeGrants(t, path)
				ownerAccess := windows.ACCESS_MASK(windows.FILE_GENERIC_READ | windows.FILE_GENERIC_WRITE | windows.FILE_GENERIC_EXECUTE | windows.DELETE | windows.WRITE_DAC | windows.WRITE_OWNER)
				for _, sid := range []string{user.User.Sid.String(), "S-1-5-18"} {
					if grants[sid]&ownerAccess != ownerAccess {
						t.Errorf("lost owner/SYSTEM access at %s: %v", path, grants)
					}
				}
				want := windows.ACCESS_MASK(0)
				if path == webview || strings.HasPrefix(path, webview+string(filepath.Separator)) {
					want = windows.FILE_GENERIC_READ | windows.FILE_GENERIC_EXECUTE
				}
				for _, sid := range []string{"S-1-15-2-2", "S-1-15-2-1"} {
					if grants[sid] != want {
						t.Errorf("sandbox grants at %s = %#x; want %#x", path, grants[sid], want)
					}
				}
			}
			if content, err := os.ReadFile(existing); err != nil || string(content) != "synthetic runtime" {
				t.Fatalf("permission update modified runtime bytes: %q %v", content, err)
			}
		})
	}
}
