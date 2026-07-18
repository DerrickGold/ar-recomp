package project

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

type BuildOptions struct {
	Paths
	CMakeCommand string
	Config       string
	Generator    string
	PrefixPath   string
	Jobs         int
	CMakeArgs    []string
	Configure    bool
	Stdout       io.Writer
	Stderr       io.Writer
}

func Configure(options BuildOptions) error {
	paths, err := options.Paths.Resolve()
	if err != nil {
		return err
	}
	options.Paths = paths
	applyBuildDefaults(&options)
	if _, err := os.Stat(filepath.Join(paths.ToolchainDir, "runtime", "runner.cmake")); err != nil {
		return fmt.Errorf("bundled runtime is incomplete: %w", err)
	}
	args := []string{"-S", paths.Root, "-B", paths.BuildDir, "-DCMAKE_BUILD_TYPE=" + options.Config}
	if options.Generator != "" {
		args = append(args, "-G", options.Generator)
	}
	if options.PrefixPath != "" {
		args = append(args, "-DCMAKE_PREFIX_PATH="+options.PrefixPath)
	}
	args = append(args, options.CMakeArgs...)
	step(options.Stdout, fmt.Sprintf("Configuring project (%s)", options.Config))
	return runExternal(options.CMakeCommand, args, paths.Root, options.Stdout, options.Stderr)
}

func Build(options BuildOptions) error {
	paths, err := options.Paths.Resolve()
	if err != nil {
		return err
	}
	options.Paths = paths
	applyBuildDefaults(&options)
	generated, err := filepath.Glob(filepath.Join(paths.GeneratedDir, "*.c"))
	if err != nil {
		return err
	}
	if len(generated) == 0 {
		return fmt.Errorf("%s is empty; regenerate before building", paths.GeneratedDir)
	}
	if options.Configure {
		if err := Configure(options); err != nil {
			return err
		}
	}
	args := []string{"--build", paths.BuildDir, "--parallel", fmt.Sprintf("%d", options.Jobs)}
	if options.Config != "" {
		args = append(args, "--config", options.Config)
	}
	step(options.Stdout, fmt.Sprintf("Building project (%d jobs)", options.Jobs))
	return runExternal(options.CMakeCommand, args, paths.Root, options.Stdout, options.Stderr)
}

func applyBuildDefaults(options *BuildOptions) {
	if options.CMakeCommand == "" {
		options.CMakeCommand = "cmake"
	}
	if options.Config == "" {
		options.Config = "RelWithDebInfo"
	}
	if options.Jobs <= 0 {
		options.Jobs = runtime.NumCPU()
	}
	if options.Stdout == nil {
		options.Stdout = io.Discard
	}
	if options.Stderr == nil {
		options.Stderr = io.Discard
	}
}

func runExternal(name string, args []string, directory string, stdout, stderr io.Writer) error {
	fmt.Fprintf(stdout, "+ %s %s\n", name, strings.Join(args, " "))
	command := exec.Command(name, args...)
	command.Dir = directory
	command.Stdout = stdout
	command.Stderr = stderr
	if err := command.Run(); err != nil {
		return fmt.Errorf("%s failed: %w", name, err)
	}
	return nil
}
