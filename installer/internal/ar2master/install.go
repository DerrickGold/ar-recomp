package ar2master

import (
	"bytes"
	"crypto/rand"
	"errors"
	"fmt"
	"io"
	"os"
)

func openDirectory(gameRoot string, create bool) (*os.Root, error) {
	root, err := os.OpenRoot(gameRoot)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	for _, path := range []string{"game-assets", "game-assets/imports"} {
		info, err := root.Lstat(path)
		if create && errors.Is(err, os.ErrNotExist) {
			if err = root.Mkdir(path, 0700); err != nil && !errors.Is(err, os.ErrExist) {
				return nil, err
			}
			info, err = root.Lstat(path)
		}
		if err != nil {
			return nil, err
		}
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return nil, fmt.Errorf("character asset path is not a regular directory")
		}
	}
	return root.OpenRoot("game-assets/imports")
}

func readInstalled(dir *os.Root) ([]byte, error) {
	info, err := dir.Lstat(Filename)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() > MaximumBytes {
		return nil, fmt.Errorf("character pack is not a bounded regular file")
	}
	file, err := dir.Open(Filename)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	return io.ReadAll(io.LimitReader(file, MaximumBytes+1))
}

// Status also validates the saved data, so a partial or damaged pack never
// appears ready. Merely viewing this panel does not create any files.
func Status(gameRoot string) string {
	dir, err := openDirectory(gameRoot, false)
	if errors.Is(err, os.ErrNotExist) {
		return "missing"
	}
	if err != nil {
		return "invalid"
	}
	defer dir.Close()
	data, err := readInstalled(dir)
	if errors.Is(err, os.ErrNotExist) {
		return "missing"
	}
	if err != nil || Validate(data) != nil {
		return "invalid"
	}
	return "installed"
}

func Install(gameRoot string, data []byte) (bool, error) {
	if err := Validate(data); err != nil {
		return false, err
	}
	dir, err := openDirectory(gameRoot, true)
	if err != nil {
		return false, err
	}
	defer dir.Close()
	if info, err := dir.Lstat(Filename); err == nil && !info.Mode().IsRegular() {
		return false, fmt.Errorf("character pack destination is not a regular file")
	} else if err != nil && !errors.Is(err, os.ErrNotExist) {
		return false, err
	}
	if prior, err := readInstalled(dir); err == nil && bytes.Equal(prior, data) {
		return false, nil
	}
	temporary := ".character-" + rand.Text()
	file, err := dir.OpenFile(temporary, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return false, err
	}
	defer dir.Remove(temporary)
	if _, err = file.Write(data); err != nil {
		file.Close()
		return false, err
	}
	if err = file.Sync(); err != nil {
		file.Close()
		return false, err
	}
	if err = file.Close(); err != nil {
		return false, err
	}
	err = dir.Rename(temporary, Filename)
	return err == nil, err
}
