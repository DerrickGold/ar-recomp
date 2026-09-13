//go:build !windows

package host

import "os/exec"

func startBackendCommand(command *exec.Cmd) (func(), error) {
	return func() {}, command.Start()
}
