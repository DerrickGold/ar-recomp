package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"

	"github.com/DerrickGold/snesrecomp-go/internal/buildgui"
	"github.com/DerrickGold/snesrecomp-go/internal/project"
	"github.com/DerrickGold/snesrecomp-go/internal/toolchain"
)

type guiFlags struct {
	root, outputDir, toolchainDir, optimize string
	jobs                                    int
	allowStubs, noOpen                      bool
}

func runGUI(args []string) error {
	flags := flag.NewFlagSet("gui", flag.ContinueOnError)
	values := guiFlags{}
	flags.StringVar(&values.root, "root", ".", "game project root")
	flags.StringVar(&values.outputDir, "output-dir", "", "playable output folder (default: project root)")
	flags.StringVar(&values.toolchainDir, "toolchain-dir", "snesrecomp-go", "snesrecomp-go module directory")
	flags.StringVar(&values.optimize, "optimize", "-O2", "hermetic optimization level")
	flags.IntVar(&values.jobs, "jobs", runtime.NumCPU(), "parallel generation/build workers")
	flags.BoolVar(&values.allowStubs, "allow-stubs", false, "complete despite the inherited hard-stub backlog")
	flags.BoolVar(&values.noOpen, "no-open", false, "print the local URL without opening a browser")
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

	return buildgui.Run(context.Background(), buildgui.Options{
		Title:       "ActRaiser Recomp Builder",
		ProjectRoot: root,
		OpenBrowser: !values.noOpen,
		Stdout:      os.Stdout,
		Build: func(ctx context.Context, romPath string, output io.Writer) (buildgui.Result, error) {
			return buildFromGUI(ctx, values, root, outputDir, romPath, output)
		},
		Launch: launchBuiltGame,
		// Lets the GUI open as a launcher beside an existing build, and refuse a
		// rebuild whose inputs have been cleaned away. See install_state.go for
		// the two file sets and why they differ.
		Detect: func() buildgui.InstallState {
			return detectInstallState(root, outputDir)
		},
		MeasureSlim: func() int64 {
			return measureSlimBytes(root)
		},
		Slim: func(output io.Writer) error {
			return slimInstall(root, outputDir, output)
		},
	})
}

func buildFromGUI(
	ctx context.Context,
	values guiFlags,
	root, outputDir, romPath string,
	output io.Writer,
) (buildgui.Result, error) {
	select {
	case <-ctx.Done():
		return buildgui.Result{}, ctx.Err()
	default:
	}
	paths := project.DefaultPaths(root)
	paths.ROM = romPath
	paths.ToolchainDir = values.toolchainDir
	if _, err := project.Regenerate(project.RegenOptions{
		Paths: paths, Jobs: values.jobs, AllowStubs: values.allowStubs,
		Stdout: output, Stderr: output,
	}); err != nil {
		return buildgui.Result{}, err
	}

	zig, err := toolchain.Locate(toolchainCacheDir(root))
	if err != nil {
		fmt.Fprintln(output, "\nNo local Zig toolchain found; fetching the pinned release.")
		zig, err = toolchain.Fetch(toolchainCacheDir(root), output)
		if err != nil {
			return buildgui.Result{}, err
		}
	}
	fmt.Fprintf(output, "\nHermetic compiler: Zig %s (%s)\n", zig.Version, zig.Source)
	binary, err := project.HermeticBuild(project.HermeticOptions{
		Paths: paths, ZigPath: zig.Path, Jobs: values.jobs,
		Optimize: values.optimize, Stdout: output, Stderr: output,
	})
	if err != nil {
		return buildgui.Result{}, err
	}
	select {
	case <-ctx.Done():
		return buildgui.Result{}, ctx.Err()
	default:
	}
	installed, err := project.InstallPlayable(project.InstallOptions{
		BinaryPath: binary, ROMPath: romPath,
		ProjectRoot: root, DestinationDir: outputDir,
	})
	if err != nil {
		return buildgui.Result{}, err
	}
	fmt.Fprintf(output, "\nPlayable game installed at %s\nLauncher written to %s\n",
		installed.BinaryPath, installed.Launcher)
	return buildgui.Result{
		Message:    "Build complete — your playable game is ready.",
		OutputPath: installed.Launcher,
		BinaryPath: installed.BinaryPath,
		WorkingDir: root,
	}, nil
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
func launchBuiltGame(result buildgui.Result) error {
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
func launchViaScript(result buildgui.Result) error {
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
