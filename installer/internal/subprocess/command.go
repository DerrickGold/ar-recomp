// Package subprocess keeps background build tools from allocating consoles on
// Windows, without changing launches from an existing CLI/Windows Terminal.
package subprocess

import "os/exec"

func Command(name string, args ...string) *exec.Cmd {
	cmd := exec.Command(name, args...)
	Configure(cmd)
	return cmd
}
