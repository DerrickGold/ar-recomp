package builder

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"

	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func nativeUSModPath(root string) string {
	return filepath.Join(root, "game-assets", "mods", ".bundled", "native-us.o2r")
}

// SaveNativeUSMod keeps the local baseline portable. The normal mod installer
// cannot replace this slot, and the unpacked manifest remains the runtime view.
func SaveNativeUSMod(root string, pack *lk.AuthorPack) error {
	var archive bytes.Buffer
	if err := lk.WriteNativeUSMod(&archive, pack); err != nil {
		return err
	}
	unlock, err := desktop.LockGameData(root)
	if err != nil {
		return err
	}
	defer unlock()
	path := nativeUSModPath(root)
	assets := filepath.Join(root, "game-assets")
	for _, dir := range []string{assets, filepath.Join(assets, "mods"), filepath.Dir(path)} {
		if err := ensureModDir(dir); err != nil {
			return err
		}
	}
	previous, _, err := readBoundedRegularFile(path, lk.MaxAuthorArchiveBytes)
	if err != nil || bytes.Equal(previous, archive.Bytes()) {
		return err
	}
	return writeAtomicFile(path, archive.Bytes())
}

func prepareNativeUSMod(root string) error {
	manifestRoot := filepath.Join(root, "game-assets", "languages", "native-us")
	path := nativeUSModPath(root)
	if regularFileExists(path) && regularFileExists(filepath.Join(manifestRoot, "pack.ini")) {
		return nil
	}
	pack, err := lk.OpenNativeUSSource(manifestRoot)
	if err != nil {
		return err
	}
	if pack != nil {
		if pack.Manifest().Version() < 2 {
			pack, err = lk.EnsureNativeUSSource(manifestRoot, nil)
			if err != nil {
				return err
			}
		}
		return SaveNativeUSMod(root, pack)
	}
	pack, err = lk.OpenNativeUSMod(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	_, err = lk.InstallNativeUSSource(manifestRoot, pack)
	return err
}

func regionalROMModPath(root, profile string) string {
	return filepath.Join(root, "game-assets", "mods", lk.LanguageModID("source."+profile)+".o2r")
}

func regionalROMModMarker(root, id string) string {
	return filepath.Join(root, "game-assets", "languages", "projects", "."+id+"-mod")
}

func installRegionalROMMod(root string, dirs []string, source *lk.AuthorProject) error {
	m := source.Pack().Manifest().Metadata()
	var font, notice []byte
	if m.Locale == "ja-JP" {
		dir := filepath.Join(root, "game-assets", "fonts", "noto")
		var missing bool
		var err error
		font, missing, err = readBoundedRegularFile(filepath.Join(dir, "NotoSansJP-Bold.otf"), lk.MaxPackFontBytes)
		if err != nil {
			return err
		}
		if missing {
			return fmt.Errorf("the bundled Japanese font is required")
		}
		notice, missing, err = readBoundedRegularFile(filepath.Join(dir, "NotoSansJP-OFL.txt"), 64<<10)
		if err != nil {
			return err
		}
		if missing {
			return fmt.Errorf("the bundled Japanese font notice is required")
		}
	}
	var archive bytes.Buffer
	if err := lk.WriteRegionalROMMod(&archive, source, font, notice); err != nil {
		return err
	}
	store, unlock, err := openModStore(root, dirs)
	if err != nil {
		return err
	}
	defer unlock()
	defer store.saveIndex()
	catalog, err := store.scan()
	if err != nil {
		return err
	}
	if !slices.ContainsFunc(catalog.languages, func(pack installedMod) bool { return pack.language.ID == m.ID }) {
		if err := store.install(&archive, &catalog, languageModType, nil); err != nil {
			return err
		}
	}
	return writeAtomicFile(regionalROMModMarker(root, m.ID), []byte("1\n"))
}

// Convert the old local references once. Keep the marker after uninstall so
// opening the Builder never silently reinstalls a pack the player removed.
func prepareExistingROMMods(root string, dirs []string) error {
	result := prepareNativeUSMod(root)
	projects := filepath.Join(root, "game-assets", "languages", "projects")
	for _, profile := range []string{"eu-en", "de", "fr", "jp"} {
		id := "source." + profile
		if regularFileExists(regionalROMModMarker(root, id)) || !regularFileExists(filepath.Join(projects, id+".arproject")) {
			continue
		}
		store, err := lk.NewAuthorStore(projects)
		if err == nil {
			var source *lk.AuthorProject
			source, err = store.Open(id)
			if err == nil {
				err = installRegionalROMMod(root, dirs, source)
			}
		}
		if err != nil {
			result = errors.Join(result, fmt.Errorf("%s: %w", id, err))
		}
	}
	return result
}
