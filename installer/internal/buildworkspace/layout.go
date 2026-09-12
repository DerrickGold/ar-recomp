// Package buildworkspace separates immutable release inputs from disposable
// compilation products. Nothing in this package deletes a prior installation.
package buildworkspace

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const Marker = ".builder-workspace"
const markerContents = "ActRaiserRecompBuilder workspace v2\n"

func ValidID(id string) bool {
	b, err := hex.DecodeString(id)
	return err == nil && len(b) == sha256.Size && strings.ToLower(id) == id
}

// Physical resolves existing ancestors without creating the destination.
func Physical(path string) (string, error) {
	path, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	if _, err := os.Lstat(path); err == nil {
		return filepath.EvalSymlinks(path)
	} else if !os.IsNotExist(err) {
		return "", err
	}
	parent := filepath.Dir(path)
	if parent == path {
		return "", fmt.Errorf("cannot resolve %s", path)
	}
	resolved, err := Physical(parent)
	return filepath.Join(resolved, filepath.Base(path)), err
}

func contains(parent, child string) bool {
	rel, err := filepath.Rel(parent, child)
	return err == nil && (rel == "." || filepath.IsLocal(rel))
}

// Separate refuses aliases and nested inputs/output/workspace, including
// symlinked ancestors. A compiler must never write inside its input bundle.
func Separate(paths ...string) error {
	var physical []string
	for _, path := range paths {
		p, err := Physical(path)
		if err != nil {
			return err
		}
		for _, prior := range physical {
			if contains(prior, p) || contains(p, prior) {
				return errors.New("bundled inputs, build workspace and game output must be separate directories")
			}
		}
		physical = append(physical, p)
	}
	return nil
}

// Prepare creates only an ownership marker. Legacy full-copy workspaces are
// accepted without changing their stamp or any user-edited source/data. Their
// old files are never used as this release's bundled inputs or auto-deleted.
func Prepare(workspace string) error {
	if info, err := os.Lstat(workspace); err == nil {
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return errors.New("workspace must be a real directory")
		}
		if data, err := readMarker(workspace, Marker); err == nil && string(data) == markerContents {
			return nil
		}
		if data, err := readMarker(workspace, ".builder-payload"); err == nil && ValidID(strings.TrimSpace(string(data))) {
			return nil
		}
		return errors.New("workspace is not managed by this Builder; choose a new workspace (existing files were not changed)")
	} else if !os.IsNotExist(err) {
		return err
	}
	parent := filepath.Dir(workspace)
	if err := os.MkdirAll(parent, 0700); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(parent, ".actraiser-workspace-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage) // Only our unpublished marker directory.
	if err := os.WriteFile(filepath.Join(stage, Marker), []byte(markerContents), 0600); err != nil {
		return err
	}
	return os.Rename(stage, workspace)
}

func readMarker(root, name string) ([]byte, error) {
	info, err := os.Lstat(filepath.Join(root, name))
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() > 128 {
		return nil, errors.New("invalid workspace marker")
	}
	return os.ReadFile(filepath.Join(root, name))
}

// Scratch keys derived files on BOTH the verified payload contents and ROM.
// A release upgrade never reuses an older release's objects or generated code.
func Scratch(workspace, inputID, rom string) (string, error) {
	if !ValidID(inputID) {
		return "", errors.New("invalid bundled input identity")
	}
	if err := Prepare(workspace); err != nil {
		return "", err
	}
	f, err := os.Open(rom)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	path := workspace
	for _, leaf := range []string{"build", inputID, hex.EncodeToString(h.Sum(nil))} {
		path = filepath.Join(path, leaf)
		if err := os.Mkdir(path, 0700); err != nil && !os.IsExist(err) {
			return "", err
		}
		info, err := os.Lstat(path)
		if err != nil {
			return "", err
		}
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return "", errors.New("build scratch must not contain symlinks or non-directories")
		}
	}
	// These are the only top-level outputs passed to the native tools. Refuse
	// pre-existing redirects before regeneration (not just before compiling).
	for _, leaf := range []string{"generated", "include", "objects", "toolchain", "zig-global", "zig-local", "metadata.json", "rts.txt", "rts.previous.txt", "include/funcs.h"} {
		info, err := os.Lstat(filepath.Join(path, filepath.FromSlash(leaf)))
		if err != nil && !os.IsNotExist(err) {
			return "", err
		}
		if err == nil && info.Mode()&os.ModeSymlink != 0 {
			return "", fmt.Errorf("build scratch output must not be a symlink: %s", leaf)
		}
	}
	return path, nil
}
