package main

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestGUIFailedBuildCleansOnlyItsOwnScratch(t *testing.T) {
	for _, bundled := range []bool{false, true} {
		name := "generic"
		if bundled {
			name = "bundled"
		}
		t.Run(name, func(t *testing.T) {
			root, output := t.TempDir(), t.TempDir()
			rom := filepath.Join(output, "user-rom.sfc")
			if err := os.WriteFile(rom, []byte("invalid synthetic ROM"), 0600); err != nil {
				t.Fatal(err)
			}
			values := guiFlags{standaloneOutput: true}
			if bundled {
				values.buildWorkspace = filepath.Join(t.TempDir(), "work")
				values.inputID = strings.Repeat("a", 64)
			}
			old := filepath.Join(root, "build", "keep.o")
			if err := os.MkdirAll(filepath.Dir(old), 0700); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(old, []byte("old checkout object"), 0600); err != nil {
				t.Fatal(err)
			}
			var log bytes.Buffer
			if _, err := buildGameFromGUI(context.Background(), values, root, output, rom, &log); err == nil {
				t.Fatal("invalid ROM accepted")
			}
			const prefix = "Clean build: using new scratch directory "
			_, tail, found := strings.Cut(log.String(), prefix)
			if !found {
				t.Fatal("no clean-build announcement", log.String())
			}
			scratch, _, _ := strings.Cut(tail, "\n")
			if _, err := os.Stat(scratch); !os.IsNotExist(err) {
				t.Fatal("scratch not removed after failure", scratch, err)
			}
			for path, want := range map[string]string{old: "old checkout object", rom: "invalid synthetic ROM"} {
				if data, err := os.ReadFile(path); err != nil || string(data) != want {
					t.Fatal("changed unrelated file", path, err)
				}
			}
		})
	}
}

func TestCleanBuildOverridesInheritedCompilerCaches(t *testing.T) {
	t.Setenv("ZIG_GLOBAL_CACHE_DIR", "old global cache")
	t.Setenv("ZIG_LOCAL_CACHE_DIR", "old local cache")
	t.Setenv("zig_global_cache_dir", "case insensitive old cache")
	scratch := t.TempDir()
	counts := map[string]int{}
	for _, entry := range cleanBuildEnvironment(scratch) {
		key, value, _ := strings.Cut(entry, "=")
		switch strings.ToUpper(key) {
		case "ZIG_GLOBAL_CACHE_DIR":
			counts["global"]++
			if value != filepath.Join(scratch, "zig-global") {
				t.Fatal(entry)
			}
		case "ZIG_LOCAL_CACHE_DIR":
			counts["local"]++
			if value != filepath.Join(scratch, "zig-local") {
				t.Fatal(entry)
			}
		}
	}
	if counts["global"] != 1 || counts["local"] != 1 {
		t.Fatal(counts)
	}
}
