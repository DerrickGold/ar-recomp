//go:build !windows

package main

import (
	"os/exec"
	"syscall"
)

// detachFromBuilder puts the launched game in its OWN process group.
//
// WHY: without this the game inherits the builder's group, so every signal aimed
// at the builder reaches the game too. All three of these killed a running game,
// measured with a stub that logged what it received:
//
//   - Ctrl-C in the run-build window   -> group SIGINT
//   - closing that window             -> group SIGHUP
//   - clicking "Close builder"        -> the pty's group leader exits
//
// The run-build script keeps that terminal open for the whole session, so this is
// not an exotic path -- it is what a user does when they think they are finished
// with the builder. Losing the game takes any unsaved battery state with it.
//
// This was an accidental regression: launching via `open` (the path before the
// GUI ran the binary directly) hands off to launchd, which reparents the child
// into a fresh group. Removing that hop removed the detachment with it, so the
// detachment has to be asked for explicitly now.
//
// Setpgid alone is right here -- NOT Setsid. The game is not a daemon and should
// keep the controlling terminal for its stderr; it only needs to be out of the
// group that receives the builder's signals.
func detachFromBuilder(command *exec.Cmd) {
	command.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
}
