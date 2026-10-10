package main

import (
	"github.com/wailsapp/go-webview2/webviewloader"
	"golang.org/x/sys/windows"
)

// Wails refuses older runtimes (its wv2installer.MinimumRuntimeVersion).
const minimumWebview2 = "94.0.992.31"

const webview2DownloadPage = "https://developer.microsoft.com/microsoft-edge/webview2/"

// requireWebview2 runs before the payload is prepared, so a missing system
// runtime costs a dialog rather than a first-launch extraction. Wails' own
// wv2runtime.error check stays as the backstop. An explicit --webview-runtime
// folder is left to Wails, which validates that path itself.
func requireWebview2(browser string) error {
	if browser != "" {
		return nil
	}
	installed, err := webviewloader.GetAvailableCoreWebView2BrowserVersionString("")
	if err != nil {
		return nil // Unexpected registry state: let Wails report it.
	}
	if installed != "" {
		if order, err := webviewloader.CompareBrowserVersions(installed, minimumWebview2); err != nil || order >= 0 {
			return nil
		}
	}
	const idYes = 6
	message := splashText("ActRaiser Recomp Builder needs the Microsoft Edge WebView2 Runtime. " +
		"Windows 11 and up-to-date Windows 10 PCs already include it, but it is missing or out of date on this PC.\n\n" +
		"Open Microsoft's WebView2 download page now? Install the Evergreen runtime, then start the Builder again.")
	title := splashText("ActRaiser Recomp Builder needs WebView2")
	if answer, _ := windows.MessageBox(0, message, title, windows.MB_YESNO|windows.MB_ICONWARNING); answer == idYes {
		_ = windows.ShellExecute(0, splashText("open"), splashText(webview2DownloadPage), nil, nil, windows.SW_SHOWNORMAL)
	}
	return errWebview2Missing
}
