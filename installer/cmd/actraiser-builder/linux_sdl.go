package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// The SDK fixes SDL's version, not the operating system's graphics/audio ABI.
// Check its loader dependencies before a potentially long regeneration/build.
// A source checkout without a bundled SDK retains normal driver discovery.
func checkBundledLinuxSDL(ctx context.Context, snesbuild string) error {
	base := filepath.Join(filepath.Dir(snesbuild), "sdl3")
	if _, err := os.Stat(base); os.IsNotExist(err) {
		return nil
	} else if err != nil {
		return err
	}
	for _, name := range []string{"include/SDL3/SDL.h", "include/SDL3_ttf/SDL_ttf.h", "lib/libSDL3.so", "lib/libSDL3.so.0", "lib/libSDL3_ttf.so", "lib/libSDL3_ttf.so.0"} {
		info, err := os.Stat(filepath.Join(base, name))
		if err != nil || !info.Mode().IsRegular() || info.Size() == 0 {
			return fmt.Errorf("bundled SDL SDK is incomplete (%s); re-extract the installer", name)
		}
	}
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	for _, name := range []string{"libSDL3.so.0", "libSDL3_ttf.so.0"} {
		command := exec.CommandContext(ctx, "ldd", filepath.Join(base, "lib", name))
		// The ttf probe must resolve the selected SDL, not an older system copy.
		for _, entry := range os.Environ() {
			if !strings.HasPrefix(entry, "LD_LIBRARY_PATH=") {
				command.Env = append(command.Env, entry)
			}
		}
		command.Env = append(command.Env, "LD_LIBRARY_PATH="+filepath.Join(base, "lib"))
		data, err := command.CombinedOutput()
		if err != nil || strings.Contains(string(data), "not found") {
			var missing []string
			for _, line := range strings.Split(string(data), "\n") {
				if strings.Contains(line, "not found") {
					missing = append(missing, strings.TrimSpace(line))
				}
			}
			detail := strings.Join(missing, "; ")
			if detail == "" {
				detail = fmt.Sprintf("%v: %s", err, strings.TrimSpace(string(data)))
			}
			return fmt.Errorf("bundled %s cannot load on this Linux installation: %s. Install the missing OS runtime libraries (not SDL development packages), or use a compatible distribution; see README.txt", name, detail)
		}
	}
	return nil
}
