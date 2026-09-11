// Package appimage assembles type-2 images with native host tools. The Linux
// runtime is copied as data, never run during packaging (including on macOS).
package appimage

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/linuxsdk"
)

func Create(appdir, runtimeFile, output, arch string) error {
	actual, err := linuxsdk.ELFArchitecture(runtimeFile)
	if err != nil {
		return err
	}
	if actual != arch {
		return fmt.Errorf("AppImage runtime is %s, expected %s", actual, arch)
	}
	runtime, err := os.Open(runtimeFile)
	if err != nil {
		return err
	}
	defer runtime.Close()
	var header [11]byte
	if _, err = runtime.ReadAt(header[:], 0); err != nil || string(header[8:]) != "AI\x02" {
		return fmt.Errorf("not a type-2 AppImage runtime")
	}
	if _, err = os.Lstat(output); !os.IsNotExist(err) {
		return fmt.Errorf("AppImage output already exists: %s", output)
	}
	if err = os.MkdirAll(filepath.Dir(output), 0755); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(filepath.Dir(output), ".appimage-create-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	squash := filepath.Join(stage, "filesystem.squashfs")
	cmd := exec.Command("mksquashfs", appdir, squash, "-noappend", "-comp", "zstd", "-b", "131072", "-all-root", "-no-xattrs", "-no-progress", "-processors", "2")
	// Normalize archive mtimes, ownership and xattrs without touching inputs.
	for _, v := range os.Environ() {
		if !strings.HasPrefix(v, "SOURCE_DATE_EPOCH=") {
			cmd.Env = append(cmd.Env, v)
		}
	}
	cmd.Env = append(cmd.Env, "SOURCE_DATE_EPOCH=0")
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	if err = cmd.Run(); err != nil {
		return fmt.Errorf("native host SquashFS creation: %w", err)
	}
	fs, err := os.Open(squash)
	if err != nil {
		return err
	}
	defer fs.Close()
	var magic [4]byte
	if _, err = fs.ReadAt(magic[:], 0); err != nil || string(magic[:]) != "hsqs" {
		return fmt.Errorf("invalid SquashFS output")
	}
	file := filepath.Join(stage, "Builder.AppImage")
	out, err := os.OpenFile(file, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0755)
	if err != nil {
		return err
	}
	defer out.Close()
	if _, err = io.Copy(out, runtime); err != nil {
		return err
	}
	if _, err = io.Copy(out, fs); err != nil {
		return err
	}
	if err = out.Sync(); err != nil {
		return err
	}
	if err = out.Close(); err != nil {
		return err
	}
	return os.Rename(file, output)
}
