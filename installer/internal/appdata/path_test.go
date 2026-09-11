package appdata

import (
	"path/filepath"
	"testing"
)

func TestSharedNamespace(t *testing.T) {
	base := t.TempDir()
	for _, goos := range []string{"darwin", "linux", "windows"} {
		t.Run(goos, func(t *testing.T) {
			env := map[string]string{"HOME": base, "LOCALAPPDATA": filepath.Join(base, "local"), "XDG_DATA_HOME": filepath.Join(base, "data")}
			getenv := func(key string) string { return env[key] }
			parent := filepath.Join(base, "Library", "Application Support", Name)
			if goos == "windows" {
				parent = filepath.Join(base, "local", Name)
			} else if goos == "linux" {
				parent = filepath.Join(base, "data", Name)
			}
			for _, component := range []string{"installer", "game"} {
				got, err := Directory(goos, component, getenv)
				if err != nil || got != filepath.Join(parent, component) {
					t.Fatalf("%s: %s, %v", component, got, err)
				}
			}
		})
	}
	for _, goos := range []string{"darwin", "linux", "windows", "unknown"} {
		if _, err := Directory(goos, "game", func(string) string { return "relative" }); err == nil {
			t.Fatalf("accepted relative application-data base on %s", goos)
		}
	}
}
