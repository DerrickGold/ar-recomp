package host

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
)

// WriteManifest already hashes this test fixture; include its own bytes as the
// Windows bundle's outer inventory does after packaging the inner manifest.
func verifiedInventory(t *testing.T, payload string) []File {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(payload, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	var manifest Manifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		t.Fatal(err)
	}
	return append(manifest.Files, File{Path: manifestName, Size: int64(len(data)), SHA256: fmt.Sprintf("%x", sha256.Sum256(data))})
}

func TestVerifiedPayloadPreparesSameSessionIdentityWithoutCopy(t *testing.T) {
	ctx := context.Background()
	payload := fixture(t)
	verified, err := PayloadFromVerifiedFiles(ctx, payload, verifiedInventory(t, payload))
	if err != nil {
		t.Fatal(err)
	}
	work := filepath.Join(t.TempDir(), "workspace")
	id, err := verified.PrepareSession(ctx, payload, work)
	if err != nil {
		t.Fatal(err)
	}
	want, err := PrepareSession(ctx, payload, filepath.Join(t.TempDir(), "fully-scanned"), nil)
	if err != nil || id != want {
		t.Fatalf("session identity changed: %q != %q (%v)", id, want, err)
	}
	entries, err := os.ReadDir(work)
	if err != nil || len(entries) != 1 || entries[0].Name() != buildworkspace.Marker {
		t.Fatalf("workspace should contain only its marker: %v (%v)", entries, err)
	}
}

func TestVerifiedPayloadRejectsInconsistentInventories(t *testing.T) {
	for _, mode := range []string{"missing manifest", "changed manifest", "missing file", "extra file", "duplicate file", "wrong digest", "wrong size", "wrong mode", "inconsistent inner manifest", "unsafe inner manifest"} {
		t.Run(mode, func(t *testing.T) {
			payload := fixture(t)
			files := verifiedInventory(t, payload)
			switch mode {
			case "missing manifest":
				files = files[:len(files)-1]
			case "changed manifest":
				files[len(files)-1].SHA256 = strings.Repeat("0", 64)
			case "missing file":
				files = files[1:]
			case "extra file":
				files = append(files, File{Path: "extra.dll"})
			case "duplicate file":
				files = append(files, files[0])
			case "wrong digest":
				files[0].SHA256 = strings.Repeat("0", 64)
			case "wrong size":
				files[0].Size++
			case "wrong mode":
				files[0].Mode ^= 0100
			default:
				data, err := os.ReadFile(filepath.Join(payload, manifestName))
				if err != nil {
					t.Fatal(err)
				}
				var manifest Manifest
				if err := json.Unmarshal(data, &manifest); err != nil {
					t.Fatal(err)
				}
				if mode == "unsafe inner manifest" {
					manifest.Files[0].Path = "../escape"
				} else {
					manifest.Files[0].SHA256 = strings.Repeat("0", 64)
				}
				data, err = json.Marshal(manifest)
				if err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(payload, manifestName), data, 0600); err != nil {
					t.Fatal(err)
				}
				// The outer inventory verifies the manifest's bytes, but its
				// payload file hashes still disagree with the inner claims.
				files[len(files)-1].Size = int64(len(data))
				files[len(files)-1].SHA256 = fmt.Sprintf("%x", sha256.Sum256(data))
			}
			verified, err := PayloadFromVerifiedFiles(context.Background(), payload, files)
			if err == nil || verified != nil {
				t.Fatal("accepted inconsistent verification", verified, err)
			}
		})
	}
}

func TestVerifiedSessionRetainsIdentityPlatformAndWorkspaceChecks(t *testing.T) {
	for _, mode := range []string{"different directory", "changed manifest", "wrong platform", "overlap", "unmanaged", "cancelled", "empty verification"} {
		t.Run(mode, func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			payload := fixture(t)
			if mode == "wrong platform" {
				arch := "amd64"
				if arch == runtime.GOARCH {
					arch = "arm64"
				}
				if err := WriteManifest(payload, runtime.GOOS, arch); err != nil {
					t.Fatal(err)
				}
			}
			verified, err := PayloadFromVerifiedFiles(ctx, payload, verifiedInventory(t, payload))
			if err != nil {
				t.Fatal(err)
			}
			work := filepath.Join(t.TempDir(), "workspace")
			switch mode {
			case "different directory":
				payload = fixture(t)
			case "changed manifest":
				data, err := os.ReadFile(filepath.Join(payload, manifestName))
				if err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(payload, manifestName), append(data, '\n'), 0600); err != nil {
					t.Fatal(err)
				}
			case "overlap":
				work = filepath.Join(payload, "workspace")
			case "unmanaged":
				if err := os.Mkdir(work, 0700); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(work, "keep"), []byte("user data"), 0600); err != nil {
					t.Fatal(err)
				}
			case "cancelled":
				cancel()
			case "empty verification":
				verified = &VerifiedPayload{}
			}
			if _, err := verified.PrepareSession(ctx, payload, work); err == nil {
				t.Fatal("invalid session accepted")
			}
			if _, err := os.Stat(filepath.Join(work, buildworkspace.Marker)); !os.IsNotExist(err) {
				t.Fatal("failed session published workspace", err)
			}
			if mode == "unmanaged" {
				data, err := os.ReadFile(filepath.Join(work, "keep"))
				if err != nil || string(data) != "user data" {
					t.Fatal("unmanaged workspace changed", err)
				}
			}
		})
	}
}
