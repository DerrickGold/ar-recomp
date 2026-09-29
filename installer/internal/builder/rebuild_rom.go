package builder

import (
	"errors"
	"os"
	"path/filepath"
)

// A browser file picker supplies bytes, not the original absolute path. The
// Builder already keeps those bytes as user-rom.sfc in its game data folder.
// Recognize the CLI's game.sfc too, plus the parent of legacy utils/ bundles.
// Never search unrelated directories or accept a path from an HTTP request.
func reusableBuildROM(root string) (string, error) {
	candidates := []string{filepath.Join(root, "user-rom.sfc"), filepath.Join(root, "game.sfc")}
	if filepath.Base(root) == "utils" {
		parent := filepath.Dir(root)
		candidates = append(candidates, filepath.Join(parent, "user-rom.sfc"), filepath.Join(parent, "game.sfc"))
	}
	for _, path := range candidates {
		info, err := os.Lstat(path)
		if err != nil || !info.Mode().IsRegular() || info.Size() == 0 || info.Size() > maxROMBytes {
			continue
		}
		f, err := os.Open(path)
		if err != nil {
			continue
		}
		var probe [1]byte
		_, readErr := f.Read(probe[:])
		f.Close()
		if readErr == nil {
			return path, nil
		}
	}
	return "", errors.New("the saved ROM is missing or unreadable; choose your .sfc or .smc ROM again to rebuild (your game, saves and settings have not been changed)")
}
