//go:build !windows

package main

import (
	"context"
	"fmt"
	"os"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
)

func prepareEmbedded(_ context.Context, payload, workspace *string, _ bool, _ winbundle.ProgressFunc) (*host.VerifiedPayload, error) {
	return nil, nil
}
func showStartupError(err error) { fmt.Fprintln(os.Stderr, err) }
