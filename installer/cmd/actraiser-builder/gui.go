package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/builder"
	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
)

type guiFlags struct {
	root, outputDir, toolchainDir, optimize, snesbuild string
	appFormat, appImageTool, appImageRuntime           string
	readyFile                                          string
	importSearchDir                                    string
	jobs                                               int
	allowStubs, noOpen                                 bool
	standaloneOutput                                   bool
}

func runGUI(args []string) error {
	flags := flag.NewFlagSet("gui", flag.ContinueOnError)
	values := guiFlags{}
	flags.StringVar(&values.root, "root", ".", "game project root")
	flags.StringVar(&values.outputDir, "output-dir", "", "playable output folder (default: project root)")
	flags.BoolVar(&values.standaloneOutput, "standalone-output", false, "keep all playable data in output-dir, independent of build inputs")
	flags.StringVar(&values.importSearchDir, "import-search-dir", "", "folder beside the desktop Builder to check for previous installations")
	flags.StringVar(&values.toolchainDir, "toolchain-dir", "snesrecomp-go", "snesrecomp-go module directory")
	flags.StringVar(&values.optimize, "optimize", "-O2", "hermetic optimization level")
	flags.StringVar(&values.snesbuild, "snesbuild", "", "trusted snesbuild executable (default: bundled sibling, source build, then PATH)")
	flags.StringVar(&values.appFormat, "app-format", "", "desktop output: app, appimage, appdir, or folder (default: native application)")
	flags.StringVar(&values.appImageTool, "appimagetool", "", "local appimagetool executable")
	flags.StringVar(&values.appImageRuntime, "appimage-runtime", "", "local type-2 AppImage runtime")
	flags.IntVar(&values.jobs, "jobs", runtime.NumCPU(), "parallel generation/build workers")
	flags.BoolVar(&values.allowStubs, "allow-stubs", false, "complete despite the inherited hard-stub backlog")
	flags.BoolVar(&values.noOpen, "no-open", false, "print the local URL without opening a browser")
	flags.StringVar(&values.readyFile, "ready-file", "", "create a private JSON session descriptor for a desktop host")
	if err := flags.Parse(args); err != nil {
		return err
	}

	root, err := filepath.Abs(values.root)
	if err != nil {
		return err
	}
	outputDir := values.outputDir
	if outputDir == "" {
		outputDir = root
	} else if !filepath.IsAbs(outputDir) {
		outputDir = filepath.Join(root, outputDir)
	}
	outputDir, err = filepath.Abs(outputDir)
	if err != nil {
		return err
	}

	dataRoot := root
	if values.standaloneOutput {
		dataRoot = outputDir
		if values.importSearchDir == "" {
			values.importSearchDir = filepath.Dir(outputDir)
		}
		if err := builder.PrepareRuntimeAssets(root); err != nil {
			return err
		}
		if err := desktop.PreparePortableData(root, dataRoot); err != nil {
			return err
		}
	}
	fmt.Fprintf(os.Stdout, "Build workspace: %s\nGame output: %s\n", root, outputDir)
	return builder.Run(context.Background(), builder.Options{
		Title:           "ActRaiser Recomp Builder",
		ProjectRoot:     dataRoot,
		ImportSearchDir: values.importSearchDir,
		OpenBrowser:     !values.noOpen,
		Stdout:          os.Stdout,
		Ready: func(address string) error {
			if values.readyFile == "" {
				return nil
			}
			return writeGUIReadyFile(values.readyFile, address)
		},
		Build: func(ctx context.Context, romPath string, output io.Writer) (builder.Result, error) {
			return buildFromGUI(ctx, values, root, outputDir, romPath, output)
		},
		Launch: launchBuiltGame,
		// Lets the GUI open as a launcher beside an existing build, and refuse a
		// rebuild whose inputs have been cleaned away. See install_state.go for
		// the two file sets and why they differ.
		Detect: func() builder.InstallState {
			state := detectInstallState(root, outputDir)
			if values.standaloneOutput {
				state.Result.WorkingDir = dataRoot
			}
			return state
		},
		MeasureSlim: func() int64 {
			return measureSlimBytes(root)
		},
		Slim: func(output io.Writer) error {
			return slimInstall(root, outputDir, output)
		},
	})
}

func writeGUIReadyFile(path, address string) error {
	data, err := json.Marshal(struct {
		Schema int    `json:"schema"`
		URL    string `json:"url"`
	}{1, address})
	if err != nil {
		return err
	}
	// Only create the host's new private file; never follow or replace a
	// pre-existing path. The host owns its directory and removes it on exit.
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, writeErr := f.Write(append(data, '\n'))
	closeErr := f.Close()
	if writeErr != nil {
		return writeErr
	}
	return closeErr
}

// launchBuiltGame starts the game.
//
// PREFERRED PATH: run the binary directly, from the working directory the
// generated script would have used, with the same arguments. The script is still
// written and is still how you play without this GUI -- that is its actual
// purpose -- but the GUI no longer goes THROUGH it, for two reasons:
//
//   - `open`/`start` report success once the OS accepts the handoff, so a
//     missing, non-executable, or immediately-crashing game looked like a
//     successful launch. Running the binary means Start() fails for real.
//   - It is one fewer process hop, and it keeps working if the script is deleted.
//
// The argument list mirrors project.launcher(): the ROM path, then
// `--config config.ini` resolved against the working directory.
func launchBuiltGame(result builder.Result) error {
	if strings.HasSuffix(result.OutputPath, ".app") || strings.HasSuffix(result.OutputPath, ".AppImage") || strings.HasSuffix(result.OutputPath, ".AppDir") {
		path := result.OutputPath
		var arguments []string
		if strings.HasSuffix(path, ".app") {
			path = filepath.Join(path, "Contents", "MacOS", "actraiser-builder")
			arguments = []string{"app-launch"}
		} else if strings.HasSuffix(path, ".AppDir") {
			path = filepath.Join(path, "AppRun")
		}
		command := exec.Command(path, arguments...)
		command.Dir = result.WorkingDir
		detachFromBuilder(command)
		if err := command.Start(); err != nil {
			return fmt.Errorf("launch application: %w", err)
		}
		go func() { _ = command.Wait() }()
		return nil
	}
	if result.BinaryPath == "" {
		// Older Result (or a host that only knows the script): fall back to the
		// previous behaviour rather than refusing to launch.
		return launchViaScript(result)
	}
	info, err := os.Stat(result.BinaryPath)
	if err != nil {
		return fmt.Errorf("the built game is missing: %w", err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("%s is not a runnable file", result.BinaryPath)
	}
	arguments := []string{}
	if rom := findInstalledROM(result.WorkingDir); rom != "" {
		arguments = append(arguments, rom)
	}
	arguments = append(arguments, "--config", "config.ini")

	command := exec.Command(result.BinaryPath, arguments...)
	command.Dir = result.WorkingDir
	// Its own process group, so closing the builder window (or Ctrl-C in it, or
	// pressing "Close builder") does not signal the game to death. The previous
	// `open`/`start` path got this for free; running the binary directly does not.
	detachFromBuilder(command)
	if err := command.Start(); err != nil {
		return fmt.Errorf("launch game: %w", err)
	}
	// Reaped in the background so a finished game does not linger as a zombie
	// for the life of the builder; the GUI deliberately does not wait on it.
	go func() { _ = command.Wait() }()
	return nil
}

// launchViaScript is the pre-existing indirect path, kept as a fallback.
func launchViaScript(result builder.Result) error {
	if result.OutputPath == "" {
		return fmt.Errorf("the completed build has no launcher")
	}
	var command *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		command = exec.Command("open", result.OutputPath)
	case "windows":
		command = exec.Command("cmd", "/c", "start", "", result.OutputPath)
	default:
		command = exec.Command(result.OutputPath)
	}
	if err := command.Start(); err != nil {
		return fmt.Errorf("launch game: %w", err)
	}
	return nil
}
