//go:build !smoketest

package main

import "github.com/wailsapp/wails/v2/pkg/options"

func enableSmokeTest(*options.App) {}
