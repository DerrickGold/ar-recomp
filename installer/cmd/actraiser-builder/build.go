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
	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
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
	var scratch string
	var environment []string
	if values.buildWorkspace != "" {
		if !values.standaloneOutput {
			return builder.Result{}, errors.New("bundled builds require a standalone game output")
		}
		if err := buildworkspace.Separate(root, values.buildWorkspace, outputDir); err != nil {
			return builder.Result{}, err
		}
		var err error
		scratch, err = buildworkspace.Scratch(values.buildWorkspace, values.inputID, romPath)
		if err != nil {
			return builder.Result{}, err
		}
		environment = append(os.Environ(), "ZIG_GLOBAL_CACHE_DIR="+filepath.Join(scratch, "zig-global"), "ZIG_LOCAL_CACHE_DIR="+filepath.Join(scratch, "zig-local"))
		fmt.Fprintf(output, "Bundled inputs: %s\nPrivate build scratch: %s\n", root, scratch)
	}
	run := func(executable string, output io.Writer, args ...string) (commandResult, error) {
		return runSnesbuildAt(ctx, executable, scratch, environment, output, args...)
	}
	fmt.Fprintf(output, "Build inputs: %s\nGame output: %s\nRuntime data: %s\n", root, outputDir, dataRoot)
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
	if scratch != "" {
		regenArgs = append(regenArgs, "--out-dir", filepath.Join(scratch, "generated"), "--funcs-out", filepath.Join(scratch, "include", "funcs.h"), "--metadata-out", filepath.Join(scratch, "metadata.json"), "--rts-report", filepath.Join(scratch, "rts.txt"), "--rts-previous", filepath.Join(scratch, "rts.previous.txt"))
		if err := os.MkdirAll(filepath.Join(scratch, "include"), 0700); err != nil {
			return builder.Result{}, err
		}
	}
	if values.allowStubs {
		regenArgs = append(regenArgs, "--allow-stubs")
	}
	if _, err := run(snesbuild, output, regenArgs...); err != nil {
		return builder.Result{}, err
	}
	if scratch != "" {
		if _, err := run(snesbuild, output, "toolchain", "status", "--root", root, "--cache-dir", filepath.Join(scratch, "toolchain")); err != nil {
			return builder.Result{}, fmt.Errorf("bundled compiler unavailable (no download attempted): %w", err)
		}
	} else {
		if err := prepareBuildToolchain(ctx, snesbuild, root, output); err != nil {
			return builder.Result{}, err
		}
	}
	buildArgs := []string{
		"build", "--root", root, "--rom", romPath,
		"--toolchain-dir", values.toolchainDir, "--jobs", fmt.Sprint(values.jobs),
		"--optimize", values.optimize, "--hermetic", "--verbose"}
	if scratch != "" {
		buildArgs = append(buildArgs, "--generated-dir", filepath.Join(scratch, "generated"), "--funcs-header", filepath.Join(scratch, "include", "funcs.h"), "--build-dir", filepath.Join(scratch, "objects"), "--input-id", values.inputID)
	}
	buildResult, err := run(snesbuild, output, buildArgs...)
	if err != nil {
		return builder.Result{}, err
	}
	binary, err := oneArtifact(buildResult, "game-binary")
	if err != nil {
		return builder.Result{}, err
	}
	if scratch != "" {
		return publishBundledGame(ctx, values, root, outputDir, romPath, binary, output)
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

// Publish a single player artifact. Legacy generic archives retain their
// separate loose-binary/launcher installation path above.
func publishBundledGame(ctx context.Context, values guiFlags, root, outputDir, romPath, binary string, output io.Writer) (builder.Result, error) {
	helper, err := os.Executable()
	if err != nil {
		return builder.Result{}, err
	}
	if values.appFormat == "folder" || runtime.GOOS == "windows" {
		installed, err := desktop.InstallGameFolder(binary, helper, romPath, outputDir)
		if err != nil {
			return builder.Result{}, err
		}
		return builder.Result{Message: "Build complete — your playable game is ready.", OutputPath: installed, BinaryPath: installed, WorkingDir: outputDir}, nil
	}
	builder.ReportBuildProgress(output, builder.BuildProgress{PhaseID: "install", Message: "Creating desktop application"})
	artifact, err := desktop.Package(ctx, desktop.PackageOptions{
		Binary: binary, Builder: helper, ROM: romPath, Root: root, DataRoot: outputDir,
		Destination: outputDir, Format: values.appFormat, Version: version,
		AppImageTool: values.appImageTool, AppImageRuntime: values.appImageRuntime,
		Replace: true, Output: output,
	})
	if err != nil {
		return builder.Result{}, err
	}
	if err := desktop.WritePortableMarker(artifact.Path, outputDir); err != nil {
		return builder.Result{}, err
	}
	if artifact.Backup != "" {
		fmt.Fprintf(output, "Previous application retained at %s\n", artifact.Backup)
	}
	probe := artifact.Path
	if strings.HasSuffix(probe, ".app") {
		probe = filepath.Join(probe, "Contents", "MacOS", desktop.Name)
	}
	if strings.HasSuffix(probe, ".AppDir") {
		probe = filepath.Join(probe, "usr", "bin", desktop.Name)
	}
	return builder.Result{Message: "Build complete — your playable game is ready.", OutputPath: artifact.Path, BinaryPath: probe, WorkingDir: outputDir}, nil
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
	return runSnesbuildAt(ctx, executable, "", nil, output, args...)
}

func runSnesbuildAt(ctx context.Context, executable, directory string, environment []string, output io.Writer, args ...string) (commandResult, error) {
	args = append(args, "--event-format", "jsonl")
	command := exec.Command(executable, args...)
	command.Dir, command.Env = directory, environment
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
	var cancelOnce sync.Once
	stop := func() {
		cancelOnce.Do(func() {
			cancelBuildProcess(command)
			// A descendant may still own an inherited pipe after a broken kill.
			// Closing our readers bounds cancellation instead of waiting forever.
			_ = stdout.Close()
			_ = stderr.Close()
		})
	}
	done := make(chan struct{})
	defer close(done)
	go func() {
		select {
		case <-ctx.Done():
			stop()
		case <-done:
		}
	}()

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
		stop()
		scanGroup.Wait()
		_ = command.Wait()
		return commandResult{}, ctx.Err()
	case streamErr = <-parseError:
		if streamErr != nil {
			stop()
			scanGroup.Wait()
			_ = command.Wait()
			if err := ctx.Err(); err != nil {
				return commandResult{}, err
			}
			return commandResult{}, streamErr
		}
	}
	<-stderrDone
	commandErr := command.Wait()
	scanGroup.Wait()
	if err := ctx.Err(); err != nil {
		return commandResult{}, err
	}
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
