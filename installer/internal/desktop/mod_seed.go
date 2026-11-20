package desktop

import (
	"errors"
	"os"
	"path/filepath"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/modconfig"
)

// Stage only the local archives and their choices. The game prepares its
// runtime overlay and cache from these archives at startup. External folders
// belong to the launcher or the machine that registered them.
func stageLocalMods(root, seed string) error {
	assets := filepath.Join(seed, "game-assets")
	const mods = "game-assets/mods"
	staged := 0
	for _, directory := range []string{mods, mods + "/.bundled"} {
		local := filepath.FromSlash(directory)
		if err := safeDestination(root, filepath.Join(local, "placeholder")); err != nil {
			return err
		}
		source := filepath.Join(root, local)
		entries, err := os.ReadDir(source)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return err
		}
		for _, entry := range entries {
			filename := entry.Name()
			archive := strings.EqualFold(filepath.Ext(filename), ".o2r")
			ownership := directory != mods && filename == "titles.json"
			if entry.Type().IsRegular() && (archive || ownership) {
				if err := copyFileAtomic(filepath.Join(source, filename), filepath.Join(seed, local, filename), 0644); err != nil {
					return err
				}
				if archive {
					staged++
				}
			}
		}
	}
	if staged == 0 {
		return nil
	}
	settings, err := modconfig.Read(filepath.Join(root, "game-assets", modconfig.OverlayName))
	if err != nil {
		return err
	}
	if !settings.HasChoices() {
		return nil
	}
	settings.Directories, settings.RelativeDirectories = nil, nil
	data, err := settings.Header()
	if err != nil {
		return err
	}
	return atomicWrite(filepath.Join(assets, modconfig.OverlayName), data, 0644)
}
