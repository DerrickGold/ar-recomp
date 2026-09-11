package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"

	"github.com/DerrickGold/ar-recomp/installer/internal/builder"
	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
	"github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

const (
	snesbuildEventSchema  = "snesbuild-event"
	snesbuildEventVersion = 1
)

type snesbuildEvent struct {
	Schema    string `json:"schema"`
	Version   int    `json:"version"`
	Type      string `json:"type"`
	Phase     string `json:"phase"`
	Message   string `json:"message"`
	Completed int    `json:"completed"`
	Total     int    `json:"total"`
	Kind      string `json:"kind"`
	Path      string `json:"path"`
	Severity  string `json:"severity"`
}

type commandResult struct {
	Artifacts map[string][]string
}

func buildFromGUI(ctx context.Context, values guiFlags, root, outputDir, romPath string, output io.Writer) (builder.Result, error) {
	if err := ctx.Err(); err != nil {
		return builder.Result{}, err
	}
	dataRoot := root
	if values.standaloneOutput {
		dataRoot = outputDir
	}
	fmt.Fprintf(output, "Build workspace: %s\nGame output: %s\nRuntime data: %s\n", root, outputDir, dataRoot)
	if err := prepareNativeUS(dataRoot, romPath, output); err != nil {
		return builder.Result{}, err
	}
	snesbuild, err := discoverSnesbuild(values.snesbuild, root)
	if err != nil {
		return builder.Result{}, err
	}
	fmt.Fprintf(output, "Using generic build driver %s\n", snesbuild)
	if runtime.GOOS == "linux" {
		if err := checkBundledLinuxSDL(ctx, snesbuild); err != nil {
			return builder.Result{}, err
		}
	}

	regenArgs := []string{"regen", "--root", root, "--rom", romPath,
		"--toolchain-dir", values.toolchainDir, "--jobs", fmt.Sprint(values.jobs)}
	if values.allowStubs {
		regenArgs = append(regenArgs, "--allow-stubs")
	}
	if _, err := runSnesbuild(ctx, snesbuild, output, regenArgs...); err != nil {
		return builder.Result{}, err
	}
	if err := prepareBuildToolchain(ctx, snesbuild, root, output); err != nil {
		return builder.Result{}, err
	}
	buildResult, err := runSnesbuild(ctx, snesbuild, output,
		"build", "--root", root, "--rom", romPath,
		"--toolchain-dir", values.toolchainDir, "--jobs", fmt.Sprint(values.jobs),
		"--optimize", values.optimize, "--hermetic", "--verbose")
	if err != nil {
		return builder.Result{}, err
	}
	binary, err := oneArtifact(buildResult, "game-binary")
	if err != nil {
		return builder.Result{}, err
	}
	installResult, err := runSnesbuild(ctx, snesbuild, output,
		"install", "--root", dataRoot, "--binary", binary, "--rom", romPath,
		"--destination", outputDir)
	if err != nil {
		return builder.Result{}, err
	}
	installedBinary, err := oneArtifact(installResult, "game-binary")
	if err != nil {
		return builder.Result{}, err
	}
	launcher, err := oneArtifact(installResult, "launcher")
	if err != nil {
		return builder.Result{}, err
	}
	if values.standaloneOutput {
		executable, err := os.Executable()
		if err != nil {
			return builder.Result{}, err
		}
		if err := desktop.InstallArchiveHelper(executable, outputDir); err != nil {
			return builder.Result{}, err
		}
	}
	if values.appFormat != "folder" && (runtime.GOOS == "darwin" || runtime.GOOS == "linux") {
		executable, err := os.Executable()
		if err != nil {
			return builder.Result{}, err
		}
		builder.ReportBuildProgress(output, builder.BuildProgress{PhaseID: "install", Message: "Creating desktop application"})
		artifact, err := desktop.Package(ctx, desktop.PackageOptions{
			Binary: binary, Builder: executable, ROM: romPath, Root: root, DataRoot: dataRoot,
			Destination: outputDir, Format: values.appFormat, Version: version,
			AppImageTool: values.appImageTool, AppImageRuntime: values.appImageRuntime,
			Replace: true, Output: output,
		})
		if err != nil {
			return builder.Result{}, err
		}
		if err := desktop.WritePortableMarker(artifact.Path, dataRoot); err != nil {
			return builder.Result{}, err
		}
		if artifact.Backup != "" {
			fmt.Fprintf(output, "Previous application retained at %s\n", artifact.Backup)
		}
		launcher = artifact.Path
	}
	return builder.Result{
		Message:    "Build complete — your playable game is ready.",
		OutputPath: launcher, BinaryPath: installedBinary, WorkingDir: dataRoot,
	}, nil
}

// Fetch fills a cache; it does not discover the compiler carried beside the
// portable build driver. Ask the driver to locate that compiler first so a
// complete installer can build without any network access or duplicate SDK.
func prepareBuildToolchain(ctx context.Context, snesbuild, root string, output io.Writer) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	result, statusErr := runSnesbuild(ctx, snesbuild, io.Discard,
		"toolchain", "status", "--root", root)
	if statusErr == nil {
		path, err := oneArtifact(result, "toolchain")
		if err != nil {
			return err
		}
		fmt.Fprintf(output, "toolchain: using existing compiler %s\n", path)
		return nil
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	// An explicit broken override will still win during compilation. Fetching
	// another compiler cannot repair it, and must not hide that choice.
	if os.Getenv("SNESBUILD_ZIG") != "" {
		return fmt.Errorf("configured SNESBUILD_ZIG toolchain is unavailable: %w", statusErr)
	}
	_, err := runSnesbuild(ctx, snesbuild, output, "toolchain", "fetch", "--root", root)
	return err
}

func prepareNativeUS(root, romPath string, output io.Writer) error {
	builder.ReportBuildProgress(output, builder.BuildProgress{
		PhaseID: "localization", Message: "Preparing native US language source",
		Completed: 0, Total: 1,
	})
	fmt.Fprintln(output, "\n=== Preparing native US language source ===")
	file, err := os.Open(romPath)
	if err != nil {
		return fmt.Errorf("open selected ROM: %w", err)
	}
	data, readErr := io.ReadAll(io.LimitReader(file, (1<<20)+1))
	closeErr := file.Close()
	if readErr != nil {
		return fmt.Errorf("read selected ROM: %w", readErr)
	}
	if closeErr != nil {
		return fmt.Errorf("close selected ROM: %w", closeErr)
	}
	directory := filepath.Join(root, "game-assets", "languages", "native-us")
	if _, err := localization.EnsureNativeUSSource(directory, data); err != nil {
		return err
	}
	builder.ReportBuildProgress(output, builder.BuildProgress{
		PhaseID: "localization", Completed: 1, Total: 1,
	})
	return nil
}

func discoverSnesbuild(explicit, root string) (string, error) {
	name := "snesbuild"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	if explicit != "" {
		if !filepath.IsAbs(explicit) {
			explicit = filepath.Join(root, explicit)
		}
		return validateExecutable(explicit)
	}
	if executable, err := os.Executable(); err == nil {
		if path, validateErr := validateExecutable(filepath.Join(filepath.Dir(executable), name)); validateErr == nil {
			return path, nil
		}
	}
	if sourceTree(root) {
		for _, candidate := range []string{
			filepath.Join(root, "build", name),
			filepath.Join(root, "snesrecomp-go", "build", name),
			filepath.Join(root, "snesrecomp-go", name),
		} {
			if path, err := validateExecutable(candidate); err == nil {
				return path, nil
			}
		}
		if path, err := exec.LookPath(name); err == nil {
			return filepath.Abs(path)
		}
	}
	return "", fmt.Errorf("snesbuild was not found; pass --snesbuild explicitly or install it beside actraiser-builder")
}

func sourceTree(root string) bool {
	for _, marker := range []string{".git", "CMakeLists.txt", "Makefile"} {
		if _, err := os.Stat(filepath.Join(root, marker)); err == nil {
			return true
		}
	}
	return false
}

func validateExecutable(path string) (string, error) {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	info, err := os.Stat(absolute)
	if err != nil || !info.Mode().IsRegular() {
		if err == nil {
			err = errors.New("not a regular file")
		}
		return "", fmt.Errorf("build driver %s is unavailable: %w", absolute, err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0o111 == 0 {
		return "", fmt.Errorf("build driver %s is not executable", absolute)
	}
	return absolute, nil
}

func runSnesbuild(ctx context.Context, executable string, output io.Writer, args ...string) (commandResult, error) {
	args = append(args, "--event-format", "jsonl")
	command := exec.Command(executable, args...)
	configureBuildProcess(command)
	stdout, err := command.StdoutPipe()
	if err != nil {
		return commandResult{}, err
	}
	stderr, err := command.StderrPipe()
	if err != nil {
		return commandResult{}, err
	}
	if err := command.Start(); err != nil {
		return commandResult{}, fmt.Errorf("start snesbuild %s: %w", args[0], err)
	}

	result := commandResult{Artifacts: make(map[string][]string)}
	parseError := make(chan error, 1)
	stderrDone := make(chan struct{})
	var scanGroup sync.WaitGroup
	scanGroup.Add(2)
	go func() {
		defer scanGroup.Done()
		parseError <- scanEventStream(stdout, output, &result)
	}()
	go func() {
		defer scanGroup.Done()
		defer close(stderrDone)
		scanner := bufio.NewScanner(stderr)
		scanner.Buffer(make([]byte, 4096), 4<<20)
		for scanner.Scan() {
			fmt.Fprintln(output, scanner.Text())
		}
	}()
	var streamErr error
	select {
	case <-ctx.Done():
		cancelBuildProcess(command)
		scanGroup.Wait()
		_ = command.Wait()
		return commandResult{}, ctx.Err()
	case streamErr = <-parseError:
		if streamErr != nil {
			cancelBuildProcess(command)
			scanGroup.Wait()
			_ = command.Wait()
			return commandResult{}, streamErr
		}
	}
	<-stderrDone
	commandErr := command.Wait()
	scanGroup.Wait()
	if commandErr != nil {
		return commandResult{}, fmt.Errorf("snesbuild %s failed: %w", args[0], commandErr)
	}
	return result, nil
}

func scanEventStream(input io.Reader, output io.Writer, result *commandResult) error {
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 4096), 4<<20)
	for scanner.Scan() {
		var event snesbuildEvent
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			return fmt.Errorf("malformed snesbuild event: %w", err)
		}
		if event.Schema != snesbuildEventSchema || event.Version != snesbuildEventVersion {
			return fmt.Errorf("unsupported snesbuild event contract %q version %d", event.Schema, event.Version)
		}
		switch event.Type {
		case "phase":
			if event.Phase == "" {
				return errors.New("snesbuild phase event has no phase")
			}
			if event.Message != "" {
				fmt.Fprintf(output, "\n=== %s ===\n", event.Message)
			}
			builder.ReportBuildProgress(output, builder.BuildProgress{PhaseID: event.Phase, Message: event.Message})
		case "progress":
			if event.Phase == "" || event.Total <= 0 || event.Completed < 0 || event.Completed > event.Total {
				return fmt.Errorf("invalid snesbuild progress event for phase %q", event.Phase)
			}
			builder.ReportBuildProgress(output, builder.BuildProgress{
				PhaseID: event.Phase, Completed: event.Completed, Total: event.Total,
			})
		case "artifact":
			if event.Kind == "" || !filepath.IsAbs(event.Path) {
				return fmt.Errorf("invalid snesbuild artifact event %q %q", event.Kind, event.Path)
			}
			result.Artifacts[event.Kind] = append(result.Artifacts[event.Kind], event.Path)
		case "diagnostic":
			if event.Message != "" {
				fmt.Fprintln(output, strings.TrimRight(event.Message, "\r\n"))
			}
		default:
			return fmt.Errorf("unknown snesbuild event type %q", event.Type)
		}
	}
	return scanner.Err()
}

func oneArtifact(result commandResult, kind string) (string, error) {
	paths := result.Artifacts[kind]
	if len(paths) != 1 {
		return "", fmt.Errorf("snesbuild returned %d %s artifacts, expected one", len(paths), kind)
	}
	return paths[0], nil
}
