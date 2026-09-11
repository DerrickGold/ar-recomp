package main

import (
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
	"github.com/wailsapp/wails/v2/pkg/options/windows"
)

// prepareEmbedded resolves a packaged Fixed Runtime before Wails starts.
// Explicit --payload/--webview-runtime paths remain useful for development.
func webviewOptions(override, payload, browser string) (*windows.Options, error) {
	executable, err := os.Executable()
	if err != nil {
		return nil, err
	}
	work, err := host.DefaultWorkspace(executable, override)
	if err != nil {
		return nil, err
	}
	// The webview starts before payload preparation. Keep its cache separate so
	// it cannot create an apparently unmanaged build workspace on first launch.
	profile := host.AuxiliaryDirectory(work, "webview")
	if payload != "" {
		if err = host.ValidateWorkspace(profile, payload); err != nil {
			return nil, err
		}
	}
	if browser != "" {
		browser, err = filepath.Abs(browser)
		if err != nil {
			return nil, err
		}
	}
	return &windows.Options{WebviewUserDataPath: profile, WebviewBrowserPath: browser}, nil
}
