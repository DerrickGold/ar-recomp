package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"sync"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/menu"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
	"github.com/wailsapp/wails/v2/pkg/options/linux"
	wruntime "github.com/wailsapp/wails/v2/pkg/runtime"
)

func main() {
	payload := flag.String("payload", "", "development-only: clean manifested installer tree")
	workspace := flag.String("workspace", "", "dedicated writable workspace; default: sidecar or per-user app data")
	outputDir := flag.String("output-dir", "", "playable game folder (default: ActRaiserRecomp beside this Builder)")
	webview := flag.String("webview-runtime", "", "Windows development: fixed WebView2 runtime directory")
	jobs := flag.Int("jobs", 0, "build workers (0 uses builder default; use 1 on low-memory machines)")
	flag.Parse()
	if *jobs < 0 {
		fmt.Fprintln(os.Stderr, "--jobs must not be negative")
		os.Exit(1)
	}
	if err := prepareEmbedded(payload, workspace, webview); err != nil {
		showStartupError(err)
		os.Exit(1)
	}
	winOptions, err := webviewOptions(*workspace, *payload, *webview)
	if err != nil {
		showStartupError(err)
		os.Exit(1)
	}
	bridge := &host.Bridge{}
	var mu sync.Mutex
	var backend *host.Backend
	var unlock func()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var startup sync.WaitGroup
	startup.Add(1)
	appMenu := menu.NewMenu()
	if runtime.GOOS == "darwin" {
		appMenu.Append(menu.AppMenu())
	}
	appMenu.Append(menu.EditMenu())
	appOptions := &options.App{
		Title: "ActRaiser Recomp Builder", Width: 1180, Height: 740, MinWidth: 800, MinHeight: 600,
		BackgroundColour: options.NewRGB(24, 27, 36),
		AssetServer:      &assetserver.Options{Handler: bridge}, Menu: appMenu,
		// WebKit uses GLib's program name for its default on-disk profile.
		// It must not create the global build workspace before Prepare owns it.
		Linux:   &linux.Options{ProgramName: host.Name + "WebView", WebviewGpuPolicy: linux.WebviewGpuPolicyOnDemand},
		Windows: winOptions,
		// No frontend Go bindings, remote content, telemetry, or updater.
		OnStartup: func(appctx context.Context) {
			go func() {
				defer startup.Done()
				bridge.Progress("Locating the Builder payload…")
				executable, err := os.Executable()
				if err != nil {
					bridge.Fail(err)
					return
				}
				artifact := executable
				var container string
				if *payload == "" {
					artifact, *payload, err = host.Discover(executable, os.Getenv("APPIMAGE"))
					if err == nil {
						container, _, err = host.Discover(executable, "")
					}
				} else {
					*payload, err = filepath.Abs(*payload)
				}
				if err != nil {
					bridge.Fail(err)
					return
				}
				work, err := host.DefaultWorkspace(artifact, *workspace)
				if err != nil {
					bridge.Fail(err)
					return
				}
				output, err := host.DefaultOutputDirectory(artifact, *outputDir)
				if err != nil {
					bridge.Fail(err)
					return
				}
				if err = host.ValidateWorkspace(output, *payload); err != nil {
					bridge.Fail(err)
					return
				}
				if err = host.ValidateWorkspace(output, work); err != nil {
					bridge.Fail(fmt.Errorf("game output must be separate from the Builder workspace: %w", err))
					return
				}
				if err = host.ValidateWorkspace(work, output); err != nil {
					bridge.Fail(fmt.Errorf("Builder workspace must be outside game output: %w", err))
					return
				}
				if container != "" {
					if err = host.ValidateWorkspace(work, container); err != nil {
						bridge.Fail(err)
						return
					}
					if err = host.ValidateWorkspace(output, container); err != nil {
						bridge.Fail(err)
						return
					}
				}
				if err = host.Prepare(*payload, work, bridge.Progress); err != nil {
					bridge.Fail(err)
					return
				}
				release, err := host.Lock(work)
				if err != nil {
					bridge.Fail(err)
					return
				}
				mu.Lock()
				unlock = release
				mu.Unlock()
				bridge.Progress("Starting the local Workshop…")
				bridge.Progress("Game output: " + output)
				b, err := host.StartBackend(ctx, work, output, *jobs, filepath.Dir(artifact))
				if err != nil {
					bridge.Fail(err)
					return
				}
				mu.Lock()
				backend = b
				mu.Unlock()
				bridge.Connect(b.URL)
				go func() {
					<-b.Done
					select {
					case <-ctx.Done():
						return
					default:
						wruntime.Quit(appctx)
					}
				}()
			}()
		},
		OnBeforeClose: func(appctx context.Context) bool {
			mu.Lock()
			b := backend
			mu.Unlock()
			if b == nil {
				return false
			}
			select {
			case <-b.Done:
				return false
			default:
			}
			answer, err := wruntime.MessageDialog(appctx, wruntime.MessageDialogOptions{
				Type: wruntime.QuestionDialog, Title: "Close Workshop?", Message: "Save any editor changes before closing. A running build must finish first.", Buttons: []string{"Cancel", "Close"}, DefaultButton: "Cancel", CancelButton: "Cancel"})
			if err != nil || answer != "Close" {
				return true
			}
			if err = b.RequestClose(); err != nil {
				_, _ = wruntime.MessageDialog(appctx, wruntime.MessageDialogOptions{Type: wruntime.WarningDialog, Title: "Builder is busy", Message: err.Error()})
				return true
			}
			return false
		},
		OnShutdown: func(context.Context) {
			cancel()
			startup.Wait()
			mu.Lock()
			b, release := backend, unlock
			mu.Unlock()
			if b != nil {
				b.Stop()
			}
			if release != nil {
				release()
			}
		},
	}
	enableSmokeTest(appOptions)
	rendererURL, stopRenderer, err := host.StartRenderer(appOptions.AssetServer.Handler)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer stopRenderer()
	bridge.SetRendererURL(rendererURL)
	appOptions.AssetServer.Handler = http.HandlerFunc(bridge.Bootstrap)
	err = wails.Run(appOptions)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
