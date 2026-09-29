package host

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func TestCachedSessionSkipsUnchangedContentsAndCanForceVerification(t *testing.T) {
	payload, work := fixture(t), filepath.Join(t.TempDir(), "workspace")
	var firstID string
	for _, mode := range []string{"first", "unchanged", "forced", "unchanged again"} {
		var messages []string
		id, err := PrepareCachedSession(context.Background(), payload, work, mode == "forced", func(s string) { messages = append(messages, s) })
		if err != nil {
			t.Fatal(mode, err)
		}
		if firstID == "" {
			firstID = id
		}
		if id != firstID {
			t.Fatal("cache changed payload identity")
		}
		hashed := strings.Contains(strings.Join(messages, "\n"), "Verifying bundled file contents")
		if hashed != (mode == "first" || mode == "forced") {
			t.Fatalf("%s: unexpected content hashing: %v", mode, messages)
		}
	}
	// AppImage mount paths change. Moving identical payload bytes and metadata
	// must not force another content scan.
	moved := filepath.Join(t.TempDir(), "new mount")
	if err := os.Rename(payload, moved); err != nil {
		t.Fatal(err)
	}
	_, err := PrepareCachedSession(context.Background(), moved, work, false, func(s string) {
		if strings.Contains(s, "Verifying bundled file contents") {
			t.Error("relocation invalidated unchanged file metadata")
		}
	})
	if err != nil {
		t.Fatal(err)
	}
}

func TestCachedSessionInvalidationAndFailures(t *testing.T) {
	for _, mode := range []string{"touch", "corrupt", "missing", "symlink", "new manifest", "bad receipt", "missing receipt", "forced same-metadata corruption", "cancelled"} {
		t.Run(mode, func(t *testing.T) {
			payload, work := fixture(t), filepath.Join(t.TempDir(), "workspace")
			if _, err := PrepareCachedSession(context.Background(), payload, work, false, nil); err != nil {
				t.Fatal(err)
			}
			name := filepath.Join(payload, "utils/snesbuild.ini")
			receipt := filepath.Join(work, ".builder-verification.json")
			prior, err := os.ReadFile(receipt)
			if err != nil {
				t.Fatal(err)
			}
			info, err := os.Stat(name)
			if err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			wantError, force := false, false
			switch mode {
			case "touch":
				err = os.Chtimes(name, info.ModTime(), info.ModTime().Add(time.Hour))
			case "corrupt", "forced same-metadata corruption":
				err = os.WriteFile(name, []byte("changed"), 0755)
				if err == nil {
					mtime := info.ModTime().Add(time.Hour)
					if mode == "forced same-metadata corruption" {
						mtime, force = info.ModTime(), true
					}
					err = os.Chtimes(name, mtime, mtime)
				}
				wantError = true
			case "missing", "symlink":
				err = os.Remove(name)
				if err == nil && mode == "symlink" {
					err = os.Symlink(filepath.Join(payload, "utils/defaults/config.ini"), name)
					if err != nil {
						t.Skipf("symlinks unavailable: %v", err)
					}
				}
				wantError = true
			case "new manifest":
				err = os.WriteFile(name, []byte("upgrade"), 0755)
				if err == nil {
					err = WriteManifest(payload, runtime.GOOS, runtime.GOARCH)
				}
			case "bad receipt":
				err = os.WriteFile(receipt, []byte("incomplete JSON"), 0600)
			case "missing receipt":
				err = os.Remove(receipt)
			case "cancelled":
				wantError = true
			}
			if err != nil {
				t.Fatal(err)
			}
			var hashed bool
			_, err = PrepareCachedSession(ctx, payload, work, force, func(s string) {
				hashed = hashed || strings.Contains(s, "Verifying bundled file contents")
				if mode == "cancelled" {
					cancel()
				}
			})
			if (err != nil) != wantError {
				t.Fatalf("unexpected result: %v", err)
			}
			if mode == "cancelled" && !errors.Is(err, context.Canceled) {
				t.Fatal("lost cancellation", err)
			}
			if wantError {
				current, readErr := os.ReadFile(receipt)
				if readErr != nil || string(current) != string(prior) {
					t.Fatal("failed check replaced successful receipt", readErr)
				}
			} else if !hashed {
				t.Fatal("change did not trigger content verification")
			}
		})
	}
}

func BenchmarkCachedSession(b *testing.B) {
	payload := fixture(b)
	if err := os.WriteFile(filepath.Join(payload, "utils", "large-sdk.dat"), make([]byte, 32<<20), 0600); err != nil {
		b.Fatal(err)
	}
	if err := WriteManifest(payload, runtime.GOOS, runtime.GOARCH); err != nil {
		b.Fatal(err)
	}
	for _, force := range []bool{true, false} {
		name := "unchanged"
		if force {
			name = "full"
		}
		b.Run(name, func(b *testing.B) {
			work := filepath.Join(b.TempDir(), "workspace")
			if _, err := PrepareCachedSession(context.Background(), payload, work, false, nil); err != nil {
				b.Fatal(err)
			}
			b.ResetTimer()
			for b.Loop() {
				if _, err := PrepareCachedSession(context.Background(), payload, work, force, nil); err != nil {
					b.Fatal(err)
				}
			}
		})
	}
}
