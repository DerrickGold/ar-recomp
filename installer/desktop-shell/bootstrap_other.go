//go:build !windows

package main

import (
	"context"
	"fmt"
	"os"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
)

func prepareEmbedded(_ context.Context, payload, workspace, browser *string, _ winbundle.ProgressFunc) error {
	return nil
}
func showStartupError(err error) { fmt.Fprintln(os.Stderr, err) }
