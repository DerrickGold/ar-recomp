package buildgui

import (
	"context"
	"fmt"
	"os/exec"
	"runtime"
	"strings"
)

// Constant command arguments only: neither selected paths nor browser input
// are interpolated into a shell/AppleScript/PowerShell program. Cancellation
// returns an empty path. Importing uses the existing confined Go directory reader.
func choosePackDirectory(ctx context.Context) (string, error) {
	name, args := directoryPickerCommand(runtime.GOOS, func(name string) bool { _, err := exec.LookPath(name); return err == nil })
	if name == "" {
		return "", fmt.Errorf("no folder chooser is available; install Zenity or KDialog, or use the folder path option")
	}
	output, err := exec.CommandContext(ctx, name, args...).Output()
	if ctx.Err() != nil {
		return "", ctx.Err()
	}
	if err != nil {
		if exit, ok := err.(*exec.ExitError); ok && (name == "zenity" || name == "kdialog") && exit.ExitCode() == 1 {
			return "", nil
		}
		return "", fmt.Errorf("could not open the folder chooser; use the folder path option: %w", err)
	}
	// Some Windows PowerShell hosts prefix redirected UTF-8 with a BOM.
	return strings.TrimRight(strings.TrimPrefix(string(output), "\ufeff"), "\r\n"), nil
}

func directoryPickerCommand(platform string, available func(string) bool) (string, []string) {
	switch platform {
	case "darwin":
		return "/usr/bin/osascript", []string{"-e", "try\nreturn POSIX path of (choose folder with prompt \"Choose a language pack folder containing pack.ini\")\non error number -128\nreturn \"\"\nend try"}
	case "windows":
		return "powershell.exe", []string{"-NoProfile", "-NonInteractive", "-STA", "-Command", "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.Windows.Forms; $picker = New-Object System.Windows.Forms.FolderBrowserDialog; $picker.Description = 'Choose a language pack folder containing pack.ini'; $picker.ShowNewFolderButton = $false; try { if ($picker.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8; [Console]::Write($picker.SelectedPath) } } finally { $picker.Dispose() }"}
	default:
		if available("zenity") {
			return "zenity", []string{"--file-selection", "--directory", "--title=Choose a language pack folder containing pack.ini"}
		}
		if available("kdialog") {
			return "kdialog", []string{"--getexistingdirectory", ".", "--title", "Choose a language pack folder containing pack.ini"}
		}
		return "", nil
	}
}
