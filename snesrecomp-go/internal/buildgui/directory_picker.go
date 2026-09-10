package buildgui

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"slices"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/uicatalog"
)

var errDirectoryChooserUnavailable = errors.New("no native folder chooser available")

// Constant programs only: translated captions are data arguments/environment,
// never interpolated into a shell/AppleScript/PowerShell program. Cancellation
// returns an empty path. Importing uses the existing confined Go directory reader.
func choosePackDirectory(ctx context.Context, language string) (string, error) {
	prompt, err := directoryPickerPrompt(language)
	if err != nil {
		return "", err
	}
	name, args := directoryPickerCommand(runtime.GOOS, prompt, func(name string) bool { _, err := exec.LookPath(name); return err == nil })
	if name == "" {
		return "", errDirectoryChooserUnavailable
	}
	command := exec.CommandContext(ctx, name, args...)
	command.Env = append(os.Environ(), "AR_BUILDER_FOLDER_PROMPT="+prompt)
	output, err := command.Output()
	if ctx.Err() != nil {
		return "", ctx.Err()
	}
	if err != nil {
		if exit, ok := err.(*exec.ExitError); ok && (name == "zenity" || name == "kdialog") && exit.ExitCode() == 1 {
			return "", nil
		}
		return "", fmt.Errorf("native folder chooser: %w", err)
	}
	// Some Windows PowerShell hosts prefix redirected UTF-8 with a BOM.
	return strings.TrimRight(strings.TrimPrefix(string(output), "\ufeff"), "\r\n"), nil
}

func directoryPickerPrompt(language string) (string, error) {
	entries, err := uicatalog.Entries()
	if err != nil {
		return "", err
	}
	index := slices.Index(uicatalog.Locales(), uicatalog.NormalizeLocale(language))
	for _, entry := range entries {
		if entry.Key == "builder.language.folder_prompt" {
			return entry.Text[index], nil
		}
	}
	return "", fmt.Errorf("missing folder chooser caption")
}

func directoryPickerCommand(platform, prompt string, available func(string) bool) (string, []string) {
	switch platform {
	case "darwin":
		return "/usr/bin/osascript", []string{"-e", "on run argv\ntry\nreturn POSIX path of (choose folder with prompt (item 1 of argv))\non error number -128\nreturn \"\"\nend try\nend run", "--", prompt}
	case "windows":
		return "powershell.exe", []string{"-NoProfile", "-NonInteractive", "-STA", "-Command", "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.Windows.Forms; $picker = New-Object System.Windows.Forms.FolderBrowserDialog; $picker.Description = $env:AR_BUILDER_FOLDER_PROMPT; $picker.ShowNewFolderButton = $false; try { if ($picker.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8; [Console]::Write($picker.SelectedPath) } } finally { $picker.Dispose() }"}
	default:
		if available("zenity") {
			return "zenity", []string{"--file-selection", "--directory", "--title=" + prompt}
		}
		if available("kdialog") {
			return "kdialog", []string{"--getexistingdirectory", ".", "--title", prompt}
		}
		return "", nil
	}
}
