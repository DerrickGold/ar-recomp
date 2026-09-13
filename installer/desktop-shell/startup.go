package main

import (
	"errors"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
)

var errAlreadyRunning = errors.New("Builder already running for this workspace")

// The Windows implementation uses only native controls: WebView2 is itself
// part of the payload whose preparation this window must precede.
type startupWindow interface {
	Update(winbundle.Progress)
	Handoff()
	Close()
}
