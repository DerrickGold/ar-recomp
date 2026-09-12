package desktop

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
)

// InstallGameFolder is the direct-executable deliverable used on Windows (and
// for an explicitly requested folder build). Stage every file first, then keep
// rollback copies until all binaries/dependencies have been published. Settings,
// saves, translations and custom assets are never part of this transaction.
func InstallGameFolder(binary, helper, rom, destination string) (string, error) {
	name := filepath.Base(binary)
	helperName := "actraiser-builder"
	if filepath.Ext(helper) == ".exe" {
		helperName += ".exe"
	}
	helperLeaf := filepath.Join("tools", helperName)
	files := map[string]string{name: binary, helperLeaf: helper, "user-rom.sfc": rom}
	order := []string{"user-rom.sfc", helperLeaf}
	patterns := map[string][]string{"darwin": {"libSDL3*.dylib"}, "windows": {"SDL3*.dll"}, "linux": {"libSDL3*.so*"}}[runtime.GOOS]
	for _, pattern := range patterns {
		libraries, err := filepath.Glob(filepath.Join(filepath.Dir(binary), pattern))
		if err != nil {
			return "", err
		}
		for _, library := range libraries {
			leaf := filepath.Base(library)
			files[leaf] = library
			order = append(order, leaf)
		}
	}
	order = append(order, name)
	for _, leaf := range order {
		if err := safeDestination(destination, leaf); err != nil {
			return "", err
		}
	}
	if err := os.MkdirAll(destination, 0755); err != nil {
		return "", err
	}
	stage, err := os.MkdirTemp(destination, ".actraiser-game-update-")
	if err != nil {
		return "", err
	}
	keep := false
	defer func() {
		if !keep {
			_ = os.RemoveAll(stage)
		}
	}() // Only our exact staging directory.
	for _, leaf := range order {
		mode := os.FileMode(0755)
		if leaf == "user-rom.sfc" {
			mode = 0600
		}
		if err := copyFileAtomic(files[leaf], filepath.Join(stage, "new", leaf), mode); err != nil {
			return "", err
		}
	}
	keep, err = publishGameFiles(stage, destination, order, os.Rename)
	if err != nil {
		return "", err
	}
	return filepath.Join(destination, name), nil
}

func publishGameFiles(stage, destination string, order []string, rename func(string, string) error) (keepBackup bool, err error) {
	existed := make(map[string]os.FileMode)
	for _, leaf := range order {
		if err := safeDestination(destination, leaf); err != nil {
			return false, err
		}
		path := filepath.Join(destination, leaf)
		if info, err := os.Stat(path); err == nil {
			existed[leaf] = info.Mode().Perm()
			if err := copyFileAtomic(path, filepath.Join(stage, "previous", leaf), info.Mode().Perm()); err != nil {
				return false, err
			}
		} else if !errors.Is(err, os.ErrNotExist) {
			return false, err
		}
	}
	var published []string
	rollback := func(cause error) (bool, error) {
		var failures []error
		for i := len(published) - 1; i >= 0; i-- {
			leaf := published[i]
			if mode, ok := existed[leaf]; ok {
				if err := copyFileAtomic(filepath.Join(stage, "previous", leaf), filepath.Join(destination, leaf), mode); err != nil {
					failures = append(failures, err)
				}
			} else if err := os.Remove(filepath.Join(destination, leaf)); err != nil {
				failures = append(failures, err)
			}
		}
		if len(failures) > 0 {
			return true, fmt.Errorf("update failed: %w; rollback: %v; recovery files retained at %s", cause, errors.Join(failures...), stage)
		}
		return false, fmt.Errorf("update failed; previous game files restored: %w", cause)
	}
	for _, leaf := range order {
		if err := safeDestination(destination, leaf); err != nil {
			return rollback(err)
		}
		target := filepath.Join(destination, leaf)
		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return rollback(err)
		}
		if err := rename(filepath.Join(stage, "new", leaf), target); err != nil {
			return rollback(err)
		}
		published = append(published, leaf)
	}
	return false, nil
}
