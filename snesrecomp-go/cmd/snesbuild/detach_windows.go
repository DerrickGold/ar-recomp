package main

import (
	"os/exec"
	"syscall"
)

// detachFromBuilder gives the launched game its own console process group, so a
// Ctrl-C or Ctrl-Break in the run-build window does not reach it. See the long
// note in detach_unix.go for what this is fixing and why the previous indirect
// launch got it for free.
//
// CREATE_NEW_PROCESS_GROUP is the Windows counterpart of Setpgid for this
// purpose: console control events are delivered per process group, so a new
// group is what excludes the game from the builder's. Spelled as the literal
// value because x/sys is not a dependency here and syscall does not export it.
func detachFromBuilder(command *exec.Cmd) {
	const createNewProcessGroup = 0x00000200
	command.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: createNewProcessGroup,
	}
}
