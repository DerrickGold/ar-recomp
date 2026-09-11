//go:build !windows

package subprocess

import "os/exec"

func Configure(*exec.Cmd) {}
