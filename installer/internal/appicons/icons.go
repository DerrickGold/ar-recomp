// Package appicons supplies original, ROM-independent artwork for every desktop
// package. Generated exports are checked in: player builds need no image tools.
package appicons

import (
	"bytes"
	"embed"
	"fmt"
	"image/png"
	"os"
	"path/filepath"
	"strings"
)

//go:embed assets/builder/* assets/game/*
var assets embed.FS

type Icon string

const (
	Builder Icon = "builder"
	Game    Icon = "game"
)

func (icon Icon) Name() string {
	if icon == Builder {
		return "ActRaiserRecompBuilder"
	}
	return "ActRaiserRecomp"
}

func (icon Icon) data(file string) []byte {
	data, err := assets.ReadFile("assets/" + string(icon) + "/" + file)
	if err != nil {
		panic(err) // Checked-in, embedded application assets; never user input.
	}
	return data
}

func (icon Icon) PNG(size int) []byte { return icon.data(fmt.Sprintf("%d.png", size)) }
func (icon Icon) ICO() []byte         { return icon.data("icon.ico") }
func (icon Icon) ICNS() []byte        { return icon.data("icon.icns") }

// WriteAppDir deliberately uses a regular PNG for .DirIcon, not a symlink that
// could be lost while staging/archiving on a different operating system.
func (icon Icon) WriteAppDir(root string) error {
	files := map[string][]byte{
		".DirIcon":           icon.PNG(256),
		icon.Name() + ".png": icon.PNG(256),
	}
	for _, size := range []int{16, 32, 48, 64, 128, 256, 512} {
		name := fmt.Sprintf("usr/share/icons/hicolor/%dx%d/apps/%s.png", size, size, icon.Name())
		files[name] = icon.PNG(size)
	}
	for name, data := range files {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			return err
		}
		if err := os.WriteFile(path, data, 0644); err != nil {
			return err
		}
	}
	return icon.ValidateAppDir(root)
}

// ValidateAppDir runs before either native appimagetool or cross-host SquashFS
// packaging, so a broken icon fails here instead of at a player's first launch.
func (icon Icon) ValidateAppDir(root string) error {
	for _, name := range []string{".DirIcon", icon.Name() + ".png"} {
		path := filepath.Join(root, name)
		info, err := os.Lstat(path)
		if err != nil {
			return fmt.Errorf("AppImage icon %s: %w", name, err)
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("AppImage icon %s must be a regular PNG file", name)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		cfg, err := png.DecodeConfig(bytes.NewReader(data))
		if err != nil || cfg.Width != 256 || cfg.Height != 256 || !bytes.Equal(data, icon.PNG(256)) {
			return fmt.Errorf("AppImage icon %s is not the expected 256px PNG", name)
		}
	}
	desktop, err := os.ReadFile(filepath.Join(root, icon.Name()+".desktop"))
	if err != nil {
		return err
	}
	for _, line := range strings.Split(strings.ReplaceAll(string(desktop), "\r\n", "\n"), "\n") {
		if line == "Icon="+icon.Name() {
			return nil
		}
	}
	return fmt.Errorf("AppImage desktop entry must specify Icon=%s", icon.Name())
}
