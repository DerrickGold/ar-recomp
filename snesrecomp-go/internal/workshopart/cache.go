package workshopart

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"image/png"
	"io"
	"os"
	"path/filepath"
)

const cacheFile = "us-scene-v3.json"
const maxCacheBytes = 1 << 20

// One atomic JSON file contains metadata and a base64 PNG. These small assets
// are loaded once per builder session, never per frame. No browser ROM decoder,
// arbitrary asset paths, partially published pairs, or player dependencies.
func ReadCache(root *os.Root) (*Assets, error) {
	info, err := root.Lstat(cacheFile)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() > maxCacheBytes {
		return nil, fmt.Errorf("invalid scenery cache file")
	}
	f, err := root.Open(cacheFile)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err = f.Stat()
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() > maxCacheBytes {
		return nil, fmt.Errorf("invalid scenery cache file")
	}
	var assets Assets
	decoder := json.NewDecoder(io.LimitReader(f, maxCacheBytes+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&assets); err != nil {
		return nil, err
	}
	if decoder.Decode(new(any)) != io.EOF {
		return nil, fmt.Errorf("extra scenery cache data")
	}
	if err := validate(&assets); err != nil {
		return nil, err
	}
	return &assets, nil
}

func validate(a *Assets) error {
	if a == nil {
		return fmt.Errorf("missing scenery assets")
	}
	if a.Catalog.Version != Version || a.Catalog.ROM != SourceSHA256 || len(a.PNG) > maxCacheBytes/2 || fmt.Sprintf("%x", sha256.Sum256(a.PNG)) != a.Catalog.ImageSHA256 {
		return fmt.Errorf("stale or invalid scenery cache")
	}
	config, err := png.DecodeConfig(bytes.NewReader(a.PNG))
	if err != nil {
		return err
	}
	if config.Width != AtlasWidth || config.Height != AtlasHeight {
		return fmt.Errorf("invalid scenery atlas dimensions")
	}
	if _, err := png.Decode(bytes.NewReader(a.PNG)); err != nil {
		return err
	}
	frames := expectedFrames()
	if len(frames) != len(a.Catalog.Frames) {
		return fmt.Errorf("invalid scenery frame count")
	}
	for i, frame := range frames {
		if a.Catalog.Frames[i] != frame {
			return fmt.Errorf("invalid scenery frame metadata")
		}
	}
	if len(a.Catalog.Animations) < 2*len(masterClips) {
		return fmt.Errorf("invalid scene animation count")
	}
	if err := validateAnimations(a.Catalog.Animations[:2*len(masterClips)]); err != nil {
		return err
	}
	return validateEncounterAnimations(a.Catalog.Animations[2*len(masterClips):])
}

// OpenCacheRoot confines cache I/O to the caller's explicit game-assets folder,
// rejecting symlink components of the cache path.
func OpenCacheRoot(projectRoot string) (*os.Root, error) {
	root, err := os.OpenRoot(projectRoot)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	for _, path := range []string{"game-assets", "game-assets/workshop"} {
		info, err := root.Lstat(path)
		if os.IsNotExist(err) {
			if err = root.Mkdir(path, 0755); err != nil && !os.IsExist(err) {
				return nil, err
			}
			info, err = root.Lstat(path)
		}
		if err != nil {
			return nil, err
		}
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return nil, fmt.Errorf("scenery cache path is not a regular directory")
		}
	}
	return root.OpenRoot("game-assets/workshop")
}

func WriteCache(root *os.Root, a *Assets) error {
	if err := validate(a); err != nil {
		return err
	}
	// Create and write through the opened directory handle.
	name := ".scene-" + rand.Text()
	f, err := root.OpenFile(name, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
	if err != nil {
		return err
	}
	defer root.Remove(name)
	if err := json.NewEncoder(f).Encode(a); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	// os.Root.Rename requires Go 1.25; the builder supports Go 1.24. As with
	// the author-store publisher, use an atomic same-directory path rename.
	// Refuse publication if the opened directory was replaced meanwhile.
	opened, err := root.Stat(".")
	if err != nil {
		return err
	}
	current, err := os.Lstat(root.Name())
	if err != nil {
		return err
	}
	if !current.IsDir() || !os.SameFile(opened, current) {
		return fmt.Errorf("scenery cache directory changed during publication")
	}
	return os.Rename(filepath.Join(root.Name(), name), filepath.Join(root.Name(), cacheFile))
}
