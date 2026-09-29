package compilerlaunch

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func deepDirectory(t *testing.T, base string) string {
	t.Helper()
	for len(base) < 360 {
		base = filepath.Join(base, "portable compiler 日本語 folder")
	}
	if err := os.MkdirAll(base, 0700); err != nil {
		t.Fatal(err)
	}
	return base
}

func fakeCompiler(t *testing.T, dir string) string {
	t.Helper()
	if err := os.MkdirAll(filepath.Join(dir, "lib"), 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "zig.exe")
	if err := os.WriteFile(path, []byte("compiler fixture"), 0700); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLongCompilerUsesPrivateCopyAndOriginalSDK(t *testing.T) {
	source := fakeCompiler(t, deepDirectory(t, t.TempDir()))
	env := []string{"SNESBUILD_ZIG=old", "snesbuild_zig=other", "ZIG_GLOBAL_CACHE_DIR=fresh-cache", "PATH=keep"}
	original := append([]string(nil), env...)
	s, err := newSession(t.TempDir(), env, true, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	staged, err := s.UseToolchain(context.Background(), source)
	if err != nil || !staged {
		t.Fatalf("stage = %v, %v", staged, err)
	}
	path := environmentValue(s.Environment, "SNESBUILD_ZIG")
	if data, err := os.ReadFile(path); err != nil || string(data) != "compiler fixture" {
		t.Fatalf("staged compiler: %q, %v", data, err)
	}
	physical, _ := physicalPath(source)
	if got := environmentValue(s.Environment, "ZIG_LIB_DIR"); got != filepath.Join(filepath.Dir(physical), "lib") {
		t.Fatal("lost original SDK", got)
	}
	if !reflect.DeepEqual(env, original) || environmentValue(s.Environment, "ZIG_GLOBAL_CACHE_DIR") != "fresh-cache" {
		t.Fatal("mutated caller environment or fresh compiler cache")
	}
	count := 0
	for _, entry := range s.Environment {
		key, _, _ := strings.Cut(entry, "=")
		if strings.EqualFold(key, "SNESBUILD_ZIG") {
			count++
		}
	}
	if count != 1 {
		t.Fatal("duplicate toolchain override", s.Environment)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("temporary compiler survived cleanup", err)
	}
	if _, err := os.Stat(source); err != nil {
		t.Fatal("removed original compiler", err)
	}
	if _, err := os.Stat(s.Directory); err != nil {
		t.Fatal("removed caller's scratch directory", err)
	}
}

func TestDeepWorkspaceUsesIndependentShortLaunchDirectory(t *testing.T) {
	base := t.TempDir()
	scratch := deepDirectory(t, base)
	lib := filepath.Join(scratch, "custom-lib")
	if err := os.Mkdir(lib, 0700); err != nil {
		t.Fatal(err)
	}
	physicalLib, err := physicalPath(lib)
	if err != nil {
		t.Fatal(err)
	}
	sessions := make([]*Session, 2)
	for i := range sessions {
		s, err := newSession(scratch, []string{"zig_lib_dir=custom-lib"}, true, []string{scratch, base})
		if err != nil {
			t.Fatal(err)
		}
		sessions[i] = s
		t.Cleanup(func() { _ = s.Close() })
		if !shortDirectory(s.Directory) || s.Directory == scratch {
			t.Fatal("compiler inherited deep working directory", s.Directory)
		}
		if got := environmentValue(s.Environment, "ZIG_LIB_DIR"); got != physicalLib {
			t.Fatal("changed relative library override", got)
		}
	}
	if sessions[0].Directory == sessions[1].Directory {
		t.Fatal("concurrent builds share a launch directory")
	}
	dir := sessions[0].Directory
	if err := sessions[0].Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatal("temporary directory survived cleanup", err)
	}
	if _, err := os.Stat(lib); err != nil {
		t.Fatal("changed portable workspace", err)
	}
}

func TestShortCompilerAndOtherPlatformsDoNotStage(t *testing.T) {
	for _, windows := range []bool{false, true} {
		env := []string{"PATH=keep"}
		s, err := newSession(t.TempDir(), env, windows, nil)
		if err != nil {
			t.Fatal(err)
		}
		source := fakeCompiler(t, t.TempDir())
		if staged, err := s.UseToolchain(context.Background(), source); err != nil || staged {
			t.Fatalf("short compiler = %v, %v", staged, err)
		}
		if !reflect.DeepEqual(env, s.Environment) || s.temporary != "" || s.compiler != "" {
			t.Fatal("unnecessary staging")
		}
	}
}

func TestStagingCancellationAndCollisionPreserveFiles(t *testing.T) {
	source := fakeCompiler(t, deepDirectory(t, t.TempDir()))
	scratch := t.TempDir()
	destination := fakeCompiler(t, scratch)
	s, err := newSession(scratch, nil, true, nil)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := s.UseToolchain(ctx, source); !errors.Is(err, context.Canceled) {
		t.Fatal("cancellation ignored", err)
	}
	if _, err := s.UseToolchain(context.Background(), source); !errors.Is(err, os.ErrExist) {
		t.Fatal("existing file overwritten", err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if data, err := os.ReadFile(destination); err != nil || string(data) != "compiler fixture" {
		t.Fatal("changed unrelated file", err)
	}
	partial := filepath.Join(scratch, "partial.exe")
	if err := copyCompiler(ctx, source, partial); !errors.Is(err, context.Canceled) {
		t.Fatal(err)
	}
	if _, err := os.Stat(partial); !os.IsNotExist(err) {
		t.Fatal("cancelled copy left partial compiler", err)
	}
}

func TestPathBudgetCountsWindowsUTF16Units(t *testing.T) {
	if !shortDirectory(strings.Repeat("x", 239)) || shortDirectory(strings.Repeat("x", 240)) {
		t.Fatal("incorrect short-directory boundary")
	}
	if shortDirectory(strings.Repeat("😀", 120)) {
		t.Fatal("non-BMP characters counted as one Windows path unit")
	}
}
