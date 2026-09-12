package desktop

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
)

// PreparePortableData seeds a standalone playable folder from the build
// workspace. Workshop edits subsequently target this folder directly, so new
// assets/languages work without rebuilding and never depend on a symlink back
// into the installer's private tools. Existing saves and settings always win.
func PreparePortableData(source, destination string, embedded ...fs.FS) error {
	if !filepath.IsAbs(source) || !filepath.IsAbs(destination) {
		return errors.New("portable source and output must be absolute")
	}
	// Check real ancestors, including not-yet-created paths, before seeding.
	physicalSource, err := resolveDataPath(source)
	if err != nil {
		return err
	}
	physicalOutput, err := resolveDataPath(destination)
	if err != nil {
		return err
	}
	relative, err := filepath.Rel(physicalSource, physicalOutput)
	if err != nil {
		return err
	}
	if localPath(relative, true) {
		return errors.New("game output must be outside build inputs")
	}
	if err := os.MkdirAll(destination, 0755); err != nil {
		return fmt.Errorf("create game output %s (move the Builder to a writable folder or use --output-dir): %w", destination, err)
	}
	unlock, err := lockInitialization(destination)
	if err != nil {
		return err
	}
	defer unlock()
	_, stateErr := os.Lstat(filepath.Join(destination, seedStateName))
	if stateErr != nil && !errors.Is(stateErr, os.ErrNotExist) {
		return stateErr
	}
	fresh := errors.Is(stateErr, os.ErrNotExist)
	stage, err := os.MkdirTemp("", "actraiser-output-seed-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	if err := stageSeed(source, stage, embedded...); err != nil {
		return err
	}
	// Notices must also accompany Windows/folder outputs, which do not have
	// an application resource directory of their own.
	if err := stageNotices(source, stage); err != nil {
		return err
	}
	if _, err := os.Stat(filepath.Join(stage, "notices")); err == nil {
		if err := copyMissingTree(filepath.Join(stage, "notices"), filepath.Join(destination, "notices")); err != nil {
			return err
		}
	}
	if fresh {
		for _, leaf := range []string{"config.ini", "settings.ini", "diorama-layers.ini", "saves"} {
			if err := copyMissingTree(filepath.Join(source, leaf), filepath.Join(destination, leaf)); err != nil {
				return err
			}
		}
	}
	return initializeDataLocked(stage, destination)
}

// InstallArchiveHelper keeps folder outputs independently capable of reading
// .arlang packs. The C runtime only searches fixed paths beside its executable.
func InstallArchiveHelper(executable, destination string) error {
	leaf := "actraiser-builder"
	if filepath.Ext(executable) == ".exe" {
		leaf += ".exe"
	}
	if err := safeDestination(destination, filepath.Join("tools", leaf)); err != nil {
		return err
	}
	return copyFileAtomic(executable, filepath.Join(destination, "tools", leaf), 0755)
}

// Older global games used the shared application directory itself. Import only
// a recognized game's runtime data into the new game/ child; never move/delete
// the original, traverse installer/, or import an adjacent portable install.
// Copy the seed history last so an interrupted import remains retryable.
func importLegacyGlobalData(destination string) error {
	if _, err := os.Lstat(filepath.Join(destination, seedStateName)); err == nil {
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	legacy := filepath.Dir(destination)
	if _, err := os.Lstat(filepath.Join(legacy, seedStateName)); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return err
	}
	if err := os.MkdirAll(destination, 0755); err != nil {
		return err
	}
	unlock, err := lockInitialization(destination)
	if err != nil {
		return err
	}
	defer unlock()
	if _, err := os.Lstat(filepath.Join(destination, seedStateName)); err == nil {
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	for _, leaf := range []string{"config.ini", "settings.ini", "diorama-layers.ini", "saves", "game-assets", "defaults", seedStateName} {
		if err := copyMissingTree(filepath.Join(legacy, leaf), filepath.Join(destination, leaf)); err != nil {
			return fmt.Errorf("import previous global game data (originals are unchanged): %w", err)
		}
	}
	return nil
}

// copyMissingTree is a non-destructive import, not synchronization: existing
// files retain precedence and symlinks are refused on both sides.
func copyMissingTree(source, destination string) error {
	info, err := os.Lstat(source)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("refusing data import from symlink: %s", source)
	}
	target, targetErr := os.Lstat(destination)
	if targetErr != nil && !errors.Is(targetErr, os.ErrNotExist) {
		return targetErr
	}
	if targetErr == nil && (target.Mode()&os.ModeSymlink != 0 || target.IsDir() != info.IsDir()) {
		return fmt.Errorf("conflicting data destination: %s", destination)
	}
	if !info.IsDir() {
		if !info.Mode().IsRegular() {
			return fmt.Errorf("not a regular data file: %s", source)
		}
		if targetErr == nil {
			if !target.Mode().IsRegular() {
				return fmt.Errorf("not a regular data destination: %s", destination)
			}
			return nil
		}
		return copyFileAtomic(source, destination, info.Mode().Perm())
	}
	if err := os.MkdirAll(destination, 0755); err != nil {
		return err
	}
	entries, err := os.ReadDir(source)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if err := copyMissingTree(filepath.Join(source, entry.Name()), filepath.Join(destination, entry.Name())); err != nil {
			return err
		}
	}
	return nil
}
