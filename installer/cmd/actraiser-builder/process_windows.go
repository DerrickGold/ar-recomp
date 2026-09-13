//go:build windows

package main

import (
	"context"
	"os/exec"
	"strconv"
	"syscall"
	"time"

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
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	kill := exec.CommandContext(ctx, "taskkill", "/T", "/F", "/PID", strconv.Itoa(command.Process.Pid))
	subprocess.Configure(kill)
	if err := kill.Run(); err != nil {
		// The desktop lifetime job is the final descendant cleanup backstop.
		_ = command.Process.Kill()
	}
}
