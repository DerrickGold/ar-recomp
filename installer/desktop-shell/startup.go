package main

import (
	"errors"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
)

var errAlreadyRunning = errors.New("Builder already running for this workspace")

// errWebview2Missing has already been explained to the user in its own dialog.
var errWebview2Missing = errors.New("WebView2 runtime is not installed")

// The Windows implementation uses only native controls: it is shown while the
// payload is prepared, before Wails starts WebView2.
type startupWindow interface {
	Update(winbundle.Progress)
	Handoff()
	Close()
}
