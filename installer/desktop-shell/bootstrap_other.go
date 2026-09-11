//go:build !windows

package main

import (
	"fmt"
	"os"
)

func prepareEmbedded(payload, workspace, browser *string) error { return nil }
func showStartupError(err error)                                { fmt.Fprintln(os.Stderr, err) }
