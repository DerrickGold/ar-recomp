// Package desktop owns locally generated ActRaiser applications and their
// startup contract. The reusable recompiler does not know this layout.
package desktop

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const Name = "ActRaiserRecomp"
const markerName = "actraiser-app.json"
const ROMName = "user-rom.sfc"

type Manifest struct {
	Format  int    `json:"format"`
	Product string `json:"product"`
	Version string `json:"version"`
}

type Layout struct {
	Artifact  string
	Resources string
	Binary    string
	Libraries string
}

// Discover uses the launcher's actual location, never the inherited working
// directory. APPIMAGE is used only for a positively identified AppDir.
func Discover(executable, appImage string) (Layout, error) {
	executable, err := filepath.Abs(executable)
	if err != nil {
		return Layout{}, err
	}
	executable, err = filepath.EvalSymlinks(executable)
	if err != nil {
		return Layout{}, err
	}
	bin := filepath.Dir(executable)
	var layout Layout
	if filepath.Base(bin) == "MacOS" && filepath.Base(filepath.Dir(bin)) == "Contents" {
		contents := filepath.Dir(bin)
		layout = Layout{Artifact: filepath.Dir(contents), Resources: filepath.Join(contents, "Resources"),
			Binary: filepath.Join(bin, Name), Libraries: filepath.Join(contents, "Frameworks")}
	} else if filepath.Base(bin) == "bin" && filepath.Base(filepath.Dir(bin)) == "usr" {
		usr := filepath.Dir(bin)
		layout = Layout{Artifact: filepath.Dir(usr), Resources: filepath.Join(usr, "share", Name),
			Binary: filepath.Join(bin, Name), Libraries: filepath.Join(usr, "lib")}
		if appImage != "" {
			if !filepath.IsAbs(appImage) {
				return Layout{}, errors.New("APPIMAGE must be absolute")
			}
			layout.Artifact = appImage
		}
	} else {
		return Layout{}, errors.New("launcher is not inside an ActRaiser application")
	}
	if err := validateManifest(layout.Resources); err != nil {
		return Layout{}, err
	}
	if info, err := os.Stat(layout.Binary); err != nil {
		return Layout{}, fmt.Errorf("application game executable: %w", err)
	} else if !info.Mode().IsRegular() || info.Mode().Perm()&0111 == 0 {
		return Layout{}, errors.New("application game executable is missing execute permission or is not a regular file")
	}
	return layout, nil
}

func validateManifest(resources string) error {
	data, err := os.ReadFile(filepath.Join(resources, markerName))
	if err != nil {
		return fmt.Errorf("read application identity: %w", err)
	}
	var manifest Manifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return err
	}
	if manifest.Format != 1 || manifest.Product != Name {
		return errors.New("unsupported application identity")
	}
	return nil
}

type StorageOptions struct {
	DataDir  string
	Portable bool
	Global   bool
}

// ResolveDataDirectory is read-only and independent of the host OS for tests.
func ResolveDataDirectory(layout Layout, options StorageOptions, cwd, goos string, env func(string) string) (string, error) {
	choices := 0
	if options.DataDir != "" {
		choices++
	}
	if options.Portable {
		choices++
	}
	if options.Global {
		choices++
	}
	if choices > 1 {
		return "", errors.New("choose only one of --data-dir, --portable, or --global")
	}
	if !filepath.IsAbs(cwd) {
		return "", errors.New("launch directory must be absolute")
	}
	absolute := func(path string) string {
		if !filepath.IsAbs(path) {
			path = filepath.Join(cwd, path)
		}
		return filepath.Clean(path)
	}
	if options.DataDir != "" {
		return absolute(options.DataDir), nil
	}
	if options.Portable {
		return filepath.Clean(cwd), nil
	}
	if !options.Global {
		if path := env("AR_USER_DATA_DIR"); path != "" {
			return absolute(path), nil
		}
		marker := layout.Artifact + ".portable"
		data, err := os.ReadFile(marker)
		if err == nil {
			path := strings.TrimSpace(string(data))
			if path == "" {
				path = "."
			}
			if !localPath(path, true) {
				return "", fmt.Errorf("%s must contain a relative directory inside its parent", marker)
			}
			return filepath.Join(filepath.Dir(layout.Artifact), path), nil
		}
		if !errors.Is(err, os.ErrNotExist) {
			return "", fmt.Errorf("read portable marker: %w", err)
		}
	}
	if goos == "linux" {
		if path := env("XDG_DATA_HOME"); filepath.IsAbs(path) {
			return filepath.Join(path, Name), nil
		}
	}
	home := env("HOME")
	if !filepath.IsAbs(home) {
		return "", errors.New("cannot resolve application data: HOME must name an absolute directory")
	}
	switch goos {
	case "darwin":
		return filepath.Join(home, "Library", "Application Support", Name), nil
	case "linux":
		return filepath.Join(home, ".local", "share", Name), nil
	default:
		return "", fmt.Errorf("desktop applications are not supported on %s", goos)
	}
}

func localPath(path string, allowDot bool) bool {
	if strings.ContainsAny(path, "\\\r\n\x00") || filepath.IsAbs(path) {
		return false
	}
	clean := filepath.Clean(path)
	return (allowDot || clean != ".") && clean != ".." && !strings.HasPrefix(clean, ".."+string(filepath.Separator))
}

// WritePortableMarker binds an app to existing folder-bundle data without
// storing a build-machine absolute path inside the movable application.
func WritePortableMarker(artifact, root string) error {
	relative, err := filepath.Rel(filepath.Dir(artifact), root)
	if err != nil {
		return err
	}
	if !localPath(relative, true) {
		return errors.New("portable data must be inside the application's parent folder")
	}
	return atomicWrite(artifact+".portable", []byte(filepath.ToSlash(relative)+"\n"), 0644)
}
