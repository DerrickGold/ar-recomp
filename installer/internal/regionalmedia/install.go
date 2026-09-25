package regionalmedia

import (
	"bytes"
	"crypto/rand"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/internal/gamerom"
)

var ErrReplaceRequired = errors.New("regional donor already exists and differs; replacement needs confirmation")

type InstalledDonor struct {
	Release   string   `json:"release"`
	Status    string   `json:"status"` // missing, installed or invalid
	Resources []uint32 `json:"resources"`
}

// ListInstalled is an explicit inventory operation, not a polling-time scan.
// Missing folders are ordinary and are never created by a read.
func ListInstalled(gameRoot string) ([]InstalledDonor, error) {
	dir, err := openInstallRoot(gameRoot, false)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, err
	}
	if dir != nil {
		defer dir.Close()
	}
	rows := make([]InstalledDonor, 0, len(gamerom.Releases()))
	for _, release := range gamerom.Releases() {
		row := InstalledDonor{Release: release.ID, Status: "missing", Resources: []uint32{}}
		if dir != nil {
			data, readErr := readInstalled(dir, release.ID+".armedia")
			if !errors.Is(readErr, os.ErrNotExist) {
				row.Status = "invalid"
				if readErr == nil {
					pack, parseErr := Unpack(data)
					if parseErr == nil && pack.Release == release {
						row.Status = "installed"
						for _, resource := range pack.Resources {
							row.Resources = append(row.Resources, resource.ID)
						}
					}
				}
			}
		}
		rows = append(rows, row)
	}
	return rows, nil
}

// Install takes a validated canonical package, never a path supplied by a
// browser. Reinstalling identical bytes is harmless. A different existing
// package is preserved unless the caller explicitly confirms replacement.
func Install(gameRoot string, data []byte, replace bool) (release string, changed bool, err error) {
	pack, err := Unpack(data)
	if err != nil {
		return "", false, err
	}
	release = pack.Release.ID
	dir, err := openInstallRoot(gameRoot, true)
	if err != nil {
		return release, false, err
	}
	defer dir.Close()
	name := release + ".armedia"
	prior, readErr := readInstalled(dir, name)
	if readErr == nil && bytes.Equal(prior, data) {
		return release, false, nil
	}
	if !errors.Is(readErr, os.ErrNotExist) {
		info, statErr := dir.Lstat(name)
		if statErr != nil || !info.Mode().IsRegular() {
			return release, false, fmt.Errorf("regional donor destination is not a regular file")
		}
		if !replace {
			return release, false, ErrReplaceRequired
		}
	}
	err = publishDonor(dir, name, data, replace)
	return release, err == nil, err
}

func openInstallRoot(gameRoot string, create bool) (*os.Root, error) {
	root, err := os.OpenRoot(gameRoot)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	for _, path := range []string{"game-assets", "game-assets/regions"} {
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
			return nil, fmt.Errorf("regional media path is not a regular directory")
		}
	}
	return root.OpenRoot("game-assets/regions")
}
func readInstalled(dir *os.Root, name string) ([]byte, error) {
	info, err := dir.Lstat(name)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() > MaximumBytes {
		return nil, fmt.Errorf("regional donor is not a bounded regular file")
	}
	file, err := dir.Open(name)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, MaximumBytes+1))
	if err == nil && len(data) > MaximumBytes {
		err = fmt.Errorf("regional donor exceeds size limit")
	}
	return data, err
}
func publishDonor(dir *os.Root, name string, data []byte, replace bool) error {
	temporary := ".media-" + rand.Text()
	file, err := dir.OpenFile(temporary, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer dir.Remove(temporary)
	if _, err = file.Write(data); err != nil {
		file.Close()
		return err
	}
	if err = file.Sync(); err != nil {
		file.Close()
		return err
	}
	if err = file.Close(); err != nil {
		return err
	}
	// Go 1.24 lacks Root.Rename/Link. Verify the pinned directory before the
	// same-directory atomic publication; never truncate a previous donor.
	opened, err := dir.Stat(".")
	if err != nil {
		return err
	}
	current, err := os.Lstat(dir.Name())
	if err != nil {
		return err
	}
	if !current.IsDir() || !os.SameFile(opened, current) {
		return fmt.Errorf("regional media directory changed during installation")
	}
	from, to := filepath.Join(dir.Name(), temporary), filepath.Join(dir.Name(), name)
	if !replace {
		// A concurrent install must not be overwritten after the initial check.
		if err = publishNewDonor(from, to); errors.Is(err, os.ErrExist) {
			return ErrReplaceRequired
		}
		return err
	}
	return os.Rename(from, to)
}
