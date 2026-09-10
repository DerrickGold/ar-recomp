package desktop

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

const seedStateName = ".actraiser-seed.json"

// InitializeData updates untouched seeded content, preserves edits/deletions,
// and publishes the baseline only after every file has been handled. It does
// not seed saves or settings. Call before the game or Workshop accesses data.
func InitializeData(resources, root string) error {
	root, err := filepath.Abs(root)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(root, 0755); err != nil {
		return err
	}
	// Resolve the chosen root once; reject symlinks beneath it while seeding.
	root, err = filepath.EvalSymlinks(root)
	if err != nil {
		return err
	}
	unlock, err := lockInitialization(root)
	if err != nil {
		return err
	}
	defer unlock()
	previous := map[string]string{}
	statePath := filepath.Join(root, seedStateName)
	if err := safeDestination(root, seedStateName); err != nil {
		return err
	}
	if raw, err := os.ReadFile(statePath); err == nil {
		if err := json.Unmarshal(raw, &previous); err != nil {
			return fmt.Errorf("read seed history: %w", err)
		}
		if previous == nil {
			previous = map[string]string{}
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	next := make(map[string]string, len(previous))
	for key, value := range previous {
		next[key] = value
	}
	seed := filepath.Join(resources, "seed")
	err = filepath.WalkDir(seed, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			return nil
		}
		if !entry.Type().IsRegular() {
			return fmt.Errorf("seed contains a non-regular file: %s", path)
		}
		leaf, err := filepath.Rel(seed, path)
		if err != nil {
			return err
		}
		key := filepath.ToSlash(leaf)
		if !strings.HasPrefix(key, "defaults/") && !strings.HasPrefix(key, "game-assets/") {
			return fmt.Errorf("unsupported seed file: %s", key)
		}
		if err := safeDestination(root, leaf); err != nil {
			return err
		}
		incoming, err := fileHash(path)
		if err != nil {
			return err
		}
		destination := filepath.Join(root, leaf)
		current, readErr := fileHash(destination)
		if readErr != nil && !errors.Is(readErr, os.ErrNotExist) {
			return readErr
		}
		old, seen := previous[key]
		ownedDefault := strings.HasPrefix(key, "defaults/")
		if current != incoming && (ownedDefault || (!seen && errors.Is(readErr, os.ErrNotExist)) || (seen && readErr == nil && current == old)) {
			if err := copyFileAtomic(path, destination, 0644); err != nil {
				return err
			}
		}
		next[key] = incoming
		return nil
	})
	if err != nil {
		return fmt.Errorf("initialize application data: %w", err)
	}
	for _, leaf := range []string{"saves", "game-assets/languages/packs", "game-assets/audio", "game-assets/hd"} {
		if err := safeDestination(root, filepath.FromSlash(leaf)+"/placeholder"); err != nil {
			return err
		}
		if err := os.MkdirAll(filepath.Join(root, leaf), 0755); err != nil {
			return err
		}
	}
	raw, err := json.Marshal(next)
	if err != nil {
		return err
	}
	return atomicWrite(statePath, raw, 0600)
}

func safeDestination(root, relative string) error {
	if !localPath(relative, false) {
		return fmt.Errorf("invalid data path: %s", relative)
	}
	path := root
	parts := strings.Split(filepath.Clean(relative), string(filepath.Separator))
	for i, part := range parts {
		path = filepath.Join(path, part)
		info, err := os.Lstat(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			return err
		}
		if info.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("refusing to seed through a symlink: %s", path)
		}
		if i < len(parts)-1 && !info.IsDir() {
			return fmt.Errorf("data parent is not a directory: %s", path)
		}
		if i == len(parts)-1 && !info.Mode().IsRegular() {
			return fmt.Errorf("data file is not a regular file: %s", path)
		}
	}
	return nil
}

func fileHash(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return "", err
	}
	if !info.Mode().IsRegular() {
		return "", fmt.Errorf("not a regular file: %s", path)
	}
	hash := sha256.New()
	if _, err := io.Copy(hash, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func atomicWrite(path string, data []byte, mode fs.FileMode) error {
	return writeAtomic(path, strings.NewReader(string(data)), mode)
}

func copyFileAtomic(source, destination string, mode fs.FileMode) error {
	f, err := os.Open(source)
	if err != nil {
		return err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("not a regular file: %s", source)
	}
	return writeAtomic(destination, f, mode)
}

func writeAtomic(path string, input io.Reader, mode fs.FileMode) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".actraiser-write-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	defer f.Close()
	if err := f.Chmod(mode); err != nil {
		return err
	}
	if _, err := io.Copy(f, input); err != nil {
		return err
	}
	if err := f.Sync(); err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(f.Name(), path)
}
