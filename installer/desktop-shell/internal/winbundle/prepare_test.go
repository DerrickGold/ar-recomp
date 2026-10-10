package winbundle

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestPrepareRuntimeScansFilesOnceOnColdAndWarmLaunch(t *testing.T) {
	o := fixture(t, "arm64")
	if err := Create(o); err != nil {
		t.Fatal(err)
	}
	directory := filepath.Join(t.TempDir(), "runtime")
	for _, warm := range []bool{false, true} {
		a, err := Open(o.Output)
		if err != nil {
			t.Fatal(err)
		}
		var stages []string
		verified, err := a.PrepareDirectoryContext(context.Background(), directory, func(p Progress) {
			if p.Completed == 0 && p.Total > 0 {
				stages = append(stages, p.Stage)
			}
		})
		a.Close()
		if err != nil || verified == nil {
			t.Fatal("runtime preparation failed", err)
		}
		want := "Extracting bundled tools"
		if warm {
			want = "Verifying prepared files"
		}
		if len(stages) != 1 || stages[0] != want {
			t.Fatalf("warm=%t: redundant/missing pass: %v", warm, stages)
		}
	}
}

func TestPrepareRuntimeRechecksCacheAndPreservesInvalidFiles(t *testing.T) {
	for _, mode := range []string{"same-size modification", "extra file", "missing file", "symlink", "wrong identity"} {
		t.Run(mode, func(t *testing.T) {
			o := fixture(t, "amd64")
			if err := Create(o); err != nil {
				t.Fatal(err)
			}
			a, err := Open(o.Output)
			if err != nil {
				t.Fatal(err)
			}
			defer a.Close()
			directory := filepath.Join(t.TempDir(), "runtime")
			if _, err := a.PrepareDirectoryContext(context.Background(), directory, nil); err != nil {
				t.Fatal(err)
			}
			name, contents := "payload/utils/snesbuild.ini", "changed"
			switch mode {
			case "extra file":
				name = "payload/injected.dll"
			case "missing file", "symlink":
				if err := os.Remove(filepath.Join(directory, name)); err != nil {
					t.Fatal(err)
				}
				if mode == "symlink" {
					if err := os.Symlink(filepath.Join(o.Payload, "utils/snesbuild.ini"), filepath.Join(directory, name)); err != nil {
						t.Skipf("symlink unavailable: %v", err)
					}
				}
			case "wrong identity":
				name = ".bundle-id"
			}
			if mode != "missing file" && mode != "symlink" {
				put(t, directory, name, []byte(contents))
			}
			verified, err := a.PrepareDirectoryContext(context.Background(), directory, nil)
			if err == nil || verified != nil {
				t.Fatal("reused old verification for changed cache")
			}
			if mode == "missing file" {
				if _, err := os.Lstat(filepath.Join(directory, name)); !os.IsNotExist(err) {
					t.Fatal("silently repaired missing file")
				}
			} else if mode != "symlink" {
				data, err := os.ReadFile(filepath.Join(directory, name))
				if err != nil || string(data) != contents {
					t.Fatal("changed cache was overwritten", err)
				}
			}
		})
	}
}

func TestPrepareRuntimeFailureNeverReturnsVerification(t *testing.T) {
	a, err := Open(customArchive(t, nil, true))
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	directory := filepath.Join(t.TempDir(), "runtime")
	if verified, err := a.PrepareDirectoryContext(context.Background(), directory, nil); err == nil || verified != nil {
		t.Fatal("corrupt extraction returned verification")
	}
	if _, err := os.Lstat(directory); !os.IsNotExist(err) {
		t.Fatal("published failed extraction")
	}
	o := fixture(t, "amd64")
	if err := Create(o); err != nil {
		t.Fatal(err)
	}
	a, err = Open(o.Output)
	if err != nil {
		t.Fatal(err)
	}
	defer a.Close()
	for _, warm := range []bool{false, true} {
		if warm {
			if _, err := a.PrepareDirectoryContext(context.Background(), directory, nil); err != nil {
				t.Fatal(err)
			}
		}
		ctx, cancel := context.WithCancel(context.Background())
		verified, err := a.PrepareDirectoryContext(ctx, directory, func(Progress) { cancel() })
		cancel()
		if !errors.Is(err, context.Canceled) || verified != nil {
			t.Fatal("cancelled startup returned verification", err)
		}
		if warm {
			if err := a.VerifyDirectory(directory); err != nil {
				t.Fatal("cancelled verification damaged cache", err)
			}
		} else if _, err := os.Lstat(directory); !os.IsNotExist(err) {
			t.Fatal("published cancelled extraction")
		}
	}
}
