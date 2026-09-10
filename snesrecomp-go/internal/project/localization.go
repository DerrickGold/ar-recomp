package project

import (
	"errors"
	"io"
	"os"
	"path/filepath"

	"github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

// PrepareBuildLocalization makes the native source available before expensive
// regeneration/toolchain work. Standalone compile paths also call the shared
// preparation step so they remain safe when invoked without the GUI or `all`.
func PrepareBuildLocalization(paths Paths, output io.Writer) error {
	resolved, err := paths.Resolve()
	if err != nil {
		return err
	}
	return prepareBuildLocalization(resolved, filepath.Join(resolved.Root, ManifestFileName), output)
}

func prepareBuildLocalization(paths Paths, manifestPath string, output io.Writer) error {
	manifest, err := LoadManifest(manifestPath)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	} // Legacy CMake-only projects.
	if err != nil {
		return err
	}
	if manifest.Localization == "" {
		return nil
	}
	if output == nil {
		output = io.Discard
	}
	step(output, "Preparing native US language source")
	dir := filepath.Join(paths.Root, "game-assets", "languages", "native-us")
	var data []byte
	if _, err := os.Lstat(filepath.Join(dir, "pack.ini")); errors.Is(err, os.ErrNotExist) {
		f, err := os.Open(paths.ROM)
		if err != nil {
			return err
		}
		data, err = io.ReadAll(io.LimitReader(f, (1<<20)+1))
		f.Close()
		if err != nil {
			return err
		}
	}
	_, err = localizationkit.EnsureNativeUSSource(dir, data)
	return err
}
