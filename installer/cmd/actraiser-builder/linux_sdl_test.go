package main

import (
	"context"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestBundledLinuxSDLChecksBeforeBuilding(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX shell fixture")
	}
	for _, scenario := range []string{"valid", "missing library", "missing runtime", "old glibc", "source checkout"} {
		t.Run(scenario, func(t *testing.T) {
			dir := t.TempDir()
			base := filepath.Join(dir, "sdl3")
			if scenario != "source checkout" {
				for _, name := range []string{"include/SDL3/SDL.h", "include/SDL3_ttf/SDL_ttf.h", "lib/libSDL3.so", "lib/libSDL3.so.0", "lib/libSDL3_ttf.so", "lib/libSDL3_ttf.so.0"} {
					if scenario == "missing library" && name == "lib/libSDL3_ttf.so.0" {
						continue
					}
					path := filepath.Join(base, name)
					if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
						t.Fatal(err)
					}
					if err := os.WriteFile(path, []byte("fixture"), 0644); err != nil {
						t.Fatal(err)
					}
				}
			}
			script := "#!/bin/sh\n[ \"$LD_LIBRARY_PATH\" = \"$TEST_SDL_LIB\" ] || exit 9\n"
			if scenario == "missing runtime" {
				script += "echo 'libXtst.so.6 => not found'\n"
			}
			if scenario == "old glibc" {
				script += "echo 'GLIBC_2.38 not found'\nexit 1\n"
			}
			if err := os.WriteFile(filepath.Join(dir, "ldd"), []byte(script), 0755); err != nil {
				t.Fatal(err)
			}
			t.Setenv("PATH", dir)
			t.Setenv("TEST_SDL_LIB", filepath.Join(base, "lib"))
			t.Setenv("LD_LIBRARY_PATH", "/wrong/system/SDL")
			err := checkBundledLinuxSDL(context.Background(), filepath.Join(dir, "snesbuild"))
			wantError := scenario != "valid" && scenario != "source checkout"
			if (err != nil) != wantError {
				t.Fatalf("error = %v", err)
			}
			if err != nil && !strings.Contains(err.Error(), "bundled") {
				t.Fatal(err)
			}
		})
	}
}
