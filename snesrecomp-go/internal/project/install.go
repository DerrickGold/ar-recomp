package project

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// InstallOptions describes how a hermetic build becomes a portable playable
// folder. ProjectRoot contains config/assets, while DestinationDir receives the
// game binary, its shared libraries, and the one-click launcher.
type InstallOptions struct {
	BinaryPath     string
	ROMPath        string
	ProjectRoot    string
	DestinationDir string
}

// InstalledGame reports the user-facing artifacts produced by InstallPlayable.
type InstalledGame struct {
	BinaryPath string
	Launcher   string
	Libraries  []string
}

// InstallPlayable copies a hermetic game and its adjacent native libraries to
// the requested output folder, then writes a relocatable run-game launcher.
func InstallPlayable(options InstallOptions) (InstalledGame, error) {
	binary, err := filepath.Abs(options.BinaryPath)
	if err != nil {
		return InstalledGame{}, err
	}
	projectRoot, err := filepath.Abs(options.ProjectRoot)
	if err != nil {
		return InstalledGame{}, err
	}
	rom, err := filepath.Abs(options.ROMPath)
	if err != nil {
		return InstalledGame{}, err
	}
	destination, err := filepath.Abs(options.DestinationDir)
	if err != nil {
		return InstalledGame{}, err
	}
	if err := os.MkdirAll(destination, 0o755); err != nil {
		return InstalledGame{}, fmt.Errorf("create playable output folder: %w", err)
	}
	if info, statErr := os.Stat(binary); statErr != nil || !info.Mode().IsRegular() {
		if statErr == nil {
			statErr = fmt.Errorf("not a regular file")
		}
		return InstalledGame{}, fmt.Errorf("built game %s is unavailable: %w", binary, statErr)
	}

	installedBinary := filepath.Join(destination, filepath.Base(binary))
	if err := copyRegularFile(binary, installedBinary, 0o755); err != nil {
		return InstalledGame{}, fmt.Errorf("install game binary: %w", err)
	}
	libraries, err := installAdjacentLibraries(filepath.Dir(binary), destination)
	if err != nil {
		return InstalledGame{}, err
	}

	projectRelative, err := containedRelative(destination, projectRoot)
	if err != nil {
		return InstalledGame{}, fmt.Errorf("project root: %w", err)
	}
	romRelative, err := containedRelative(destination, rom)
	if err != nil {
		return InstalledGame{}, fmt.Errorf("ROM: %w", err)
	}
	launcherName, launcherContents := launcher(
		filepath.Base(installedBinary), projectRelative, romRelative)
	launcherPath := filepath.Join(destination, launcherName)
	if err := os.WriteFile(launcherPath, []byte(launcherContents), 0o755); err != nil {
		return InstalledGame{}, fmt.Errorf("write game launcher: %w", err)
	}
	return InstalledGame{
		BinaryPath: installedBinary,
		Launcher:   launcherPath,
		Libraries:  libraries,
	}, nil
}

func containedRelative(base, target string) (string, error) {
	relative, err := filepath.Rel(base, target)
	if err != nil {
		return "", err
	}
	if relative == ".." || strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
		return "", fmt.Errorf("%s must be inside %s", target, base)
	}
	if strings.ContainsAny(relative, "\"\r\n") {
		return "", fmt.Errorf("unsupported launcher path %q", relative)
	}
	return relative, nil
}

func installAdjacentLibraries(sourceDir, destinationDir string) ([]string, error) {
	var patterns []string
	switch runtime.GOOS {
	case "darwin":
		patterns = []string{"libSDL3*.dylib"}
	case "windows":
		patterns = []string{"SDL3*.dll"}
	default:
		patterns = []string{"libSDL3*.so*"}
	}
	seen := make(map[string]struct{})
	var installed []string
	for _, pattern := range patterns {
		matches, _ := filepath.Glob(filepath.Join(sourceDir, pattern))
		for _, source := range matches {
			name := filepath.Base(source)
			if _, found := seen[name]; found {
				continue
			}
			seen[name] = struct{}{}
			target := filepath.Join(destinationDir, name)
			if err := copyRegularFile(source, target, 0o755); err != nil {
				return nil, fmt.Errorf("install %s: %w", name, err)
			}
			installed = append(installed, target)
		}
	}
	return installed, nil
}

func copyRegularFile(source, destination string, mode os.FileMode) (resultErr error) {
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	output, err := os.OpenFile(destination, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, mode)
	if err != nil {
		return err
	}
	defer func() {
		if closeErr := output.Close(); resultErr == nil {
			resultErr = closeErr
		}
	}()
	if _, err := io.Copy(output, input); err != nil {
		return err
	}
	return output.Chmod(mode)
}

func launcher(binaryName, projectRelative, romRelative string) (string, string) {
	if runtime.GOOS == "windows" {
		projectPath := strings.ReplaceAll(projectRelative, "/", `\`)
		romPath := strings.ReplaceAll(romRelative, "/", `\`)
		return "run-game.bat", fmt.Sprintf(
			"@echo off\r\nrem Runs the locally built game.\r\n"+
				"cd /d \"%%~dp0%s\"\r\n\"%%~dp0%s\" \"%%~dp0%s\" --config config.ini\r\n",
			projectPath, binaryName, romPath)
	}
	projectPath := filepath.ToSlash(projectRelative)
	romPath := filepath.ToSlash(romRelative)
	name := "run-game.sh"
	if runtime.GOOS == "darwin" {
		name = "run-game.command"
	}
	return name, fmt.Sprintf(
		"#!/bin/sh\n# Runs the locally built game.\n"+
			"ROOT=\"$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\"\n"+
			"cd \"$ROOT/%s\" || exit 1\n"+
			"exec \"$ROOT/%s\" \"$ROOT/%s\" --config config.ini\n",
		projectPath, binaryName, romPath)
}
