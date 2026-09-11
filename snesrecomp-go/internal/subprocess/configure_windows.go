package subprocess

import (
	"os/exec"
	"syscall"
)

var getConsoleWindow = syscall.NewLazyDLL("kernel32.dll").NewProc("GetConsoleWindow")

func Configure(cmd *exec.Cmd) {
	window, _, _ := getConsoleWindow.Call()
	configure(cmd, window != 0)
}

func configure(cmd *exec.Cmd, hasConsole bool) {
	if hasConsole {
		return
	}
	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	// CREATE_NO_WINDOW is per child, not an inherited setting. Each Go
	// launcher in the helper -> snesbuild -> compiler chain must apply it.
	// Unlike HideWindow, it does not hide a tool's own GUI (folder picker).
	cmd.SysProcAttr.CreationFlags |= 0x08000000
}
