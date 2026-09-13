//go:build !windows

package main

import (
	"context"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
)

type noStartupWindow struct{}

func beginStartup(_ string, _ context.CancelFunc) (startupWindow, error) {
	return noStartupWindow{}, nil
}
func (noStartupWindow) Update(winbundle.Progress) {}
func (noStartupWindow) Handoff()                  {}
func (noStartupWindow) Close()                    {}
