// Package subprocess keeps background build tools from allocating consoles on
// Windows, without changing launches from an existing CLI/Windows Terminal.
package subprocess

import (
	"context"
	"os/exec"
)

func Command(name string, args ...string) *exec.Cmd {
	cmd := exec.Command(name, args...)
	Configure(cmd)
	return cmd
}

// CommandContext is Command whose process is killed once ctx is done.
func CommandContext(ctx context.Context, name string, args ...string) *exec.Cmd {
	cmd := exec.CommandContext(ctx, name, args...)
	Configure(cmd)
	return cmd
}
