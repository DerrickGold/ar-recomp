// Package appdata defines the shared per-user application namespace. It does
// not create directories or move existing user data.
package appdata

import (
	"fmt"
	"path/filepath"
)

const Name = "ActRaiserRecomp"

func Directory(goos, component string, env func(string) string) (string, error) {
	if component != "installer" && component != "game" {
		return "", fmt.Errorf("unknown application-data component %q", component)
	}
	var base string
	switch goos {
	case "linux":
		base = env("XDG_DATA_HOME")
	case "windows":
		base = env("LOCALAPPDATA")
		if !filepath.IsAbs(base) {
			return "", fmt.Errorf("LOCALAPPDATA must name an absolute directory")
		}
	case "darwin":
	default:
		return "", fmt.Errorf("unsupported application-data platform %s", goos)
	}
	if !filepath.IsAbs(base) {
		home := env("HOME")
		if !filepath.IsAbs(home) {
			return "", fmt.Errorf("HOME must name an absolute directory")
		}
		if goos == "darwin" {
			base = filepath.Join(home, "Library", "Application Support")
		} else {
			base = filepath.Join(home, ".local", "share")
		}
	}
	return filepath.Join(base, Name, component), nil
}
