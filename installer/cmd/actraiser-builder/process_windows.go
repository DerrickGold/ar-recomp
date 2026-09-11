//go:build windows

package main

import (
	"os/exec"
	"strconv"
	"syscall"

	"github.com/DerrickGold/ar-recomp/installer/internal/subprocess"
)

func configureBuildProcess(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{CreationFlags: 0x00000200}
	subprocess.Configure(command)
}

func cancelBuildProcess(command *exec.Cmd) {
	if command.Process == nil {
		return
	}
	_ = subprocess.Command("taskkill", "/T", "/F", "/PID", strconv.Itoa(command.Process.Pid)).Run()
}
