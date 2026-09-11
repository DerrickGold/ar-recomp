package main

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/appimage"
	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/linuxbundle"
	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/linuxsdk"
)

func packageLinux(source, shell, output, sdkRoot, glibcMax string) error {
	if runtime.GOOS != "linux" && sdkRoot == "" {
		return fmt.Errorf("Linux packaging requires a native Linux maintainer host")
	}
	arch := runtime.GOARCH
	var sdk *linuxsdk.SDK
	if sdkRoot != "" {
		var err error
		sdk, err = linuxsdk.Load(sdkRoot)
		if err != nil {
			return err
		}
		arch = sdk.Lock.Arch
	}
	if filepath.Ext(output) != ".AppImage" && filepath.Ext(output) != ".AppDir" {
		return fmt.Errorf("Linux output must end in .AppImage or .AppDir")
	}
	if _, err := os.Lstat(output); !os.IsNotExist(err) {
		return fmt.Errorf("output exists or cannot be checked: %s", output)
	}
	if err := host.ValidateWorkspace(output, source); err != nil {
		return fmt.Errorf("output must be outside the source payload: %w", err)
	}
	for _, file := range []string{shell, filepath.Join(source, "utils/tools/actraiser-builder"), filepath.Join(source, "utils/tools/snesbuild")} {
		actual, err := linuxsdk.ELFArchitecture(file)
		if err != nil {
			return err
		}
		if actual != arch {
			return fmt.Errorf("mixed Linux payload: %s is %s, expected %s", file, actual, arch)
		}
	}
	if err := host.WriteManifest(source, "linux", arch); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(output), 0755); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(filepath.Dir(output), ".builder-package-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(stage)
	app := filepath.Join(stage, host.Name+".AppDir")
	bin := filepath.Join(app, "usr/bin", host.Name)
	if err = os.MkdirAll(filepath.Dir(bin), 0755); err != nil {
		return err
	}
	if err = copyFile(shell, bin, 0755); err != nil {
		return err
	}
	if sdk != nil {
		err = linuxbundle.StageSDK(app, shell, sdk)
	} else {
		err = linuxbundle.Stage(app, shell)
	}
	if err != nil {
		return err
	}
	if err = linuxbundle.CopyPayload(source, filepath.Join(app, "usr/share", host.Name, "payload")); err != nil {
		return err
	}
	audit, err := linuxsdk.AuditABI(app, arch, glibcMax)
	if err != nil {
		return err
	}
	raw, err := json.MarshalIndent(audit, "", "  ")
	if err != nil {
		return err
	}
	if err = os.WriteFile(filepath.Join(app, "usr/share/doc", host.Name, "linux-abi.json"), append(raw, '\n'), 0644); err != nil {
		return err
	}
	fmt.Printf("Linux ABI audit: %d ELF programs/libraries; maximum required GLIBC_%s\n", len(audit.Files), audit.MaximumGLIBC)
	result := app
	if filepath.Ext(output) == ".AppImage" {
		tools := filepath.Join(source, "utils/tools")
		result = filepath.Join(stage, host.Name+".AppImage")
		if sdk != nil {
			if err = appimage.Create(app, filepath.Join(tools, "appimage-runtime"), result, arch); err != nil {
				return err
			}
		} else {
			cmd := exec.Command(filepath.Join(tools, "appimagetool"), "--runtime-file", filepath.Join(tools, "appimage-runtime"), "--no-appstream", app, result)
			cmd.Env = append(os.Environ(), "APPIMAGE_EXTRACT_AND_RUN=1", "ARCH="+map[string]string{"arm64": "aarch64", "amd64": "x86_64"}[runtime.GOARCH])
			cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
			if err = cmd.Run(); err != nil {
				return fmt.Errorf("create Builder AppImage: %w", err)
			}
			if err = os.Chmod(result, 0755); err != nil {
				return err
			}
			f, err := os.Open(result)
			if err != nil {
				return err
			}
			header := make([]byte, 11)
			_, err = f.ReadAt(header, 0)
			f.Close()
			if err != nil || string(header[:4]) != "\x7fELF" || string(header[8:11]) != "AI\x02" {
				return fmt.Errorf("invalid type-2 AppImage output")
			}
		}
	}
	if err = os.Rename(result, output); err != nil {
		return err
	}
	fmt.Println(output)
	return nil
}
