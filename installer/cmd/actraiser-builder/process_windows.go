//go:build windows

package main

import (
	"os/exec"
	"strconv"
	"syscall"
)

func configureBuildProcess(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{CreationFlags: 0x00000200}
}

func cancelBuildProcess(command *exec.Cmd) {
	if command.Process == nil {
		return
	}
	_ = exec.Command("taskkill", "/T", "/F", "/PID", strconv.Itoa(command.Process.Pid)).Run()
}
