package host

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
)

func TestSessionReadsBundleWithoutCopyAndAcceptsUpgrade(t *testing.T) {
	payload := fixture(t)
	work := filepath.Join(t.TempDir(), "workspace")
	first, err := PrepareSession(context.Background(), payload, work, nil)
	if err != nil {
		t.Fatal(err)
	}
	entries, err := os.ReadDir(work)
	if err != nil || len(entries) != 1 || entries[0].Name() != buildworkspace.Marker {
		t.Fatalf("copied payload: %v %v", entries, err)
	}
	if !buildworkspace.ValidID(first) {
		t.Fatalf("invalid identity %q", first)
	}
	if err := os.WriteFile(filepath.Join(work, "keep"), []byte("existing build"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(payload, "utils", "snesbuild.ini"), []byte("upgraded"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := WriteManifest(payload, runtime.GOOS, runtime.GOARCH); err != nil {
		t.Fatal(err)
	}
	second, err := PrepareSession(context.Background(), payload, work, nil)
	if err != nil || first == second {
		t.Fatalf("upgrade identity: %s %v", second, err)
	}
	data, err := os.ReadFile(filepath.Join(work, "keep"))
	if err != nil || string(data) != "existing build" {
		t.Fatalf("upgrade changed workspace: %q %v", data, err)
	}
	legacy := t.TempDir()
	stamp := strings.Repeat("c", 64)
	if err := os.WriteFile(filepath.Join(legacy, ".builder-payload"), []byte(stamp), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(filepath.Join(legacy, "utils"), 0700); err != nil {
		t.Fatal(err)
	}
	edited := filepath.Join(legacy, "utils", "snesbuild.ini")
	if err := os.WriteFile(edited, []byte("user-edited legacy source"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := PrepareSession(context.Background(), payload, legacy, nil); err != nil {
		t.Fatalf("legacy workspace rejected: %v", err)
	}
	for file, want := range map[string]string{edited: "user-edited legacy source", filepath.Join(legacy, ".builder-payload"): stamp} {
		if data, err := os.ReadFile(file); err != nil || string(data) != want {
			t.Fatalf("legacy file changed: %s: %q %v", file, data, err)
		}
	}
}

func TestSessionRejectsUnmanagedWorkspaceWithoutChanges(t *testing.T) {
	payload, work := fixture(t), t.TempDir()
	path := filepath.Join(work, "keep")
	if err := os.WriteFile(path, []byte("unrelated data"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := PrepareSession(context.Background(), payload, work, nil); err == nil {
		t.Fatal("accepted unmanaged workspace")
	}
	if data, err := os.ReadFile(path); err != nil || string(data) != "unrelated data" {
		t.Fatalf("unmanaged files changed: %q %v", data, err)
	}
	if entries, err := os.ReadDir(work); err != nil || len(entries) != 1 {
		t.Fatalf("unmanaged workspace adopted or seeded: %v %v", entries, err)
	}
}

func TestSessionRejectsCorruptionAndCancellationWithoutPublishing(t *testing.T) {
	for _, mode := range []string{"corrupt", "wrong-size", "cancel", "symlink", "wrong-platform"} {
		t.Run(mode, func(t *testing.T) {
			payload := fixture(t)
			work := filepath.Join(t.TempDir(), "workspace")
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			file := filepath.Join(payload, "utils", "snesbuild.ini")
			switch mode {
			case "corrupt":
				// Same size as the fixture: exercise the checksum, not just size.
				if err := os.WriteFile(file, []byte("corrupt"), 0600); err != nil {
					t.Fatal(err)
				}
			case "wrong-size":
				if err := os.WriteFile(file, []byte("tampered"), 0600); err != nil {
					t.Fatal(err)
				}
			case "symlink":
				// Replace only a fixture owned by this test.
				if err := os.Remove(file); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(filepath.Join(payload, "utils", "CMakeLists.txt"), file); err != nil {
					t.Skipf("symlinks unavailable: %v", err)
				}
			case "wrong-platform":
				arch := "amd64"
				if runtime.GOARCH == arch {
					arch = "arm64"
				}
				if err := WriteManifest(payload, runtime.GOOS, arch); err != nil {
					t.Fatal(err)
				}
			}
			_, err := PrepareSession(ctx, payload, work, func(string) {
				if mode == "cancel" {
					cancel()
				}
			})
			if err == nil {
				t.Fatal("invalid session succeeded")
			}
			if mode == "cancel" && !errors.Is(err, context.Canceled) {
				t.Fatalf("cancellation: %v", err)
			}
			if _, err := os.Stat(work); !os.IsNotExist(err) {
				t.Fatalf("published failed workspace: %v", err)
			}
		})
	}
}

func TestBundledBackendRunsFromInputsWithWritableCWD(t *testing.T) {
	payload := backendProcessFixture(t, "ready")
	work := t.TempDir()
	id := strings.Repeat("e", 64)
	b, err := StartBundledBackend(context.Background(), work, payload, id, t.TempDir(), 1)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(b.Stop)
	if b.command.Dir != work || !strings.HasPrefix(b.command.Path, payload+string(filepath.Separator)) {
		t.Fatalf("wrong layout: %+v", b.command)
	}
	args := strings.Join(b.command.Args, "\x00")
	for _, want := range []string{"--root\x00" + filepath.Join(payload, "utils"), "--build-workspace\x00" + work, "--input-id\x00" + id} {
		if !strings.Contains(args, want) {
			t.Fatalf("missing %q in %q", want, args)
		}
	}
	if _, err := os.Stat(filepath.Join(work, "utils")); !os.IsNotExist(err) {
		t.Fatalf("copied tools: %v", err)
	}
	b.Stop()
	if !b.command.ProcessState.Success() {
		t.Fatalf("backend shutdown: %v", b.command.ProcessState)
	}
}
