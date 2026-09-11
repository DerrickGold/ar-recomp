//go:build darwin || linux

package main

import "github.com/wailsapp/wails/v2/pkg/options/windows"

func webviewOptions(string, string, string) (*windows.Options, error) { return nil, nil }
