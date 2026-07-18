// snesbuild is the cross-platform per-project driver for snesrecomp projects.
// It replaces shell-specific regeneration and CMake build wrappers while
// keeping every path explicit and relocatable.
package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/project"
)

var version = "dev"

func main() {
	if err := run(os.Args[1:]); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return
		}
		fmt.Fprintf(os.Stderr, "snesbuild: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) == 0 {
		usage()
		return errors.New("missing command")
	}
	switch args[0] {
	case "regen":
		return runRegen(args[1:])
	case "configure":
		return runConfigure(args[1:])
	case "build":
		return runBuild(args[1:])
	case "all":
		return runAll(args[1:])
	case "doctor":
		return runDoctor(args[1:])
	case "version", "--version":
		fmt.Printf("snesbuild %s (%s/%s)\n", version, runtime.GOOS, runtime.GOARCH)
		return nil
	case "help", "-h", "--help":
		usage()
		return nil
	default:
		usage()
		return fmt.Errorf("unknown command %q", args[0])
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, `Usage: snesbuild <command> [options]

Commands:
  regen       Regenerate C and all generated sidecars
  configure   Configure the native game build with CMake
  build       Configure (by default) and compile the native game
  all         Regenerate, configure, and compile in one command
  doctor      Report host tools and project inputs
  version     Print the driver version and target platform

The binary itself has no runtime dependencies. Regeneration needs only a local
ROM. Native compilation currently also needs CMake, a C compiler, and the
frontend dependencies required by the game project.`)
}

type regenFlags struct {
	root, rom, cfgDir, genDir, funcs, metadata, rtsReport, rtsPrevious string
	toolchainDir, goCommand                                            string
	jobs                                                               int
	allowStubs, runTests, noTests                                      bool
}

func addRegenFlags(flags *flag.FlagSet) *regenFlags {
	values := &regenFlags{}
	flags.StringVar(&values.root, "root", ".", "game project root")
	flags.StringVar(&values.rom, "rom", "game.sfc", "ROM path, relative to project root")
	flags.StringVar(&values.cfgDir, "cfg-dir", "recomp", "bank config directory")
	flags.StringVar(&values.genDir, "out-dir", "src/gen", "generated C directory")
	flags.StringVar(&values.funcs, "funcs-out", "recomp/funcs.h", "generated function header")
	flags.StringVar(&values.metadata, "metadata-out", "saves/gen_meta.json", "generated metadata sidecar")
	flags.StringVar(&values.rtsReport, "rts-report", "saves/rts_webs.txt", "current RTS-web census")
	flags.StringVar(&values.rtsPrevious, "rts-previous", "saves/rts_webs.prev.txt", "previous RTS-web census")
	flags.StringVar(&values.toolchainDir, "toolchain-dir", "snesrecomp-go", "snesrecomp-go module directory")
	flags.StringVar(&values.goCommand, "go-command", "go", "Go executable used only with --run-tests")
	flags.IntVar(&values.jobs, "jobs", runtime.NumCPU(), "parallel generation workers")
	flags.BoolVar(&values.allowStubs, "allow-stubs", false, "complete successfully despite the hard-stub gate")
	flags.BoolVar(&values.runTests, "run-tests", false, "run the Go toolchain tests after regeneration")
	flags.BoolVar(&values.noTests, "no-tests", false, "compatibility override for wrappers that default to --run-tests")
	return values
}

func (values *regenFlags) options() project.RegenOptions {
	paths := project.DefaultPaths(values.root)
	paths.ROM, paths.ConfigDir, paths.GeneratedDir = values.rom, values.cfgDir, values.genDir
	paths.FuncsHeader, paths.Metadata = values.funcs, values.metadata
	paths.RTSReport, paths.RTSPrevious = values.rtsReport, values.rtsPrevious
	paths.ToolchainDir = values.toolchainDir
	return project.RegenOptions{
		Paths: paths, Jobs: values.jobs, AllowStubs: values.allowStubs,
		RunTests: values.runTests && !values.noTests, GoCommand: values.goCommand,
		Stdout: os.Stdout, Stderr: os.Stderr,
	}
}

type stringList []string

func (values *stringList) String() string { return strings.Join(*values, " ") }
func (values *stringList) Set(value string) error {
	*values = append(*values, value)
	return nil
}

type buildFlags struct {
	root, buildDir, toolchainDir, cmake, config, generator, prefixPath string
	jobs                                                               int
	buildOnly                                                          bool
	cmakeArgs                                                          stringList
}

func addBuildFlags(flags *flag.FlagSet) *buildFlags {
	values := &buildFlags{}
	flags.StringVar(&values.root, "root", ".", "game project root")
	flags.StringVar(&values.buildDir, "build-dir", "build", "native build directory")
	flags.StringVar(&values.toolchainDir, "toolchain-dir", "snesrecomp-go", "snesrecomp-go module directory")
	flags.StringVar(&values.cmake, "cmake", "cmake", "CMake executable")
	flags.StringVar(&values.config, "config", "RelWithDebInfo", "CMake build configuration")
	flags.StringVar(&values.generator, "generator", "", "optional CMake generator")
	flags.StringVar(&values.prefixPath, "prefix-path", os.Getenv("CMAKE_PREFIX_PATH"), "CMake package prefix path")
	flags.IntVar(&values.jobs, "jobs", runtime.NumCPU(), "parallel native build jobs")
	flags.BoolVar(&values.buildOnly, "build-only", false, "skip CMake configure and use the existing build directory")
	flags.Var(&values.cmakeArgs, "cmake-arg", "additional CMake configure argument; repeat as needed")
	return values
}

func (values *buildFlags) options() project.BuildOptions {
	paths := project.DefaultPaths(values.root)
	paths.BuildDir, paths.ToolchainDir = values.buildDir, values.toolchainDir
	return project.BuildOptions{
		Paths: paths, CMakeCommand: values.cmake, Config: values.config,
		Generator: values.generator, PrefixPath: values.prefixPath, Jobs: values.jobs,
		CMakeArgs: append([]string(nil), values.cmakeArgs...), Configure: !values.buildOnly,
		Stdout: os.Stdout, Stderr: os.Stderr,
	}
}

func runRegen(args []string) error {
	flags := flag.NewFlagSet("regen", flag.ContinueOnError)
	values := addRegenFlags(flags)
	if err := flags.Parse(args); err != nil {
		return err
	}
	_, err := project.Regenerate(values.options())
	return err
}

func runConfigure(args []string) error {
	flags := flag.NewFlagSet("configure", flag.ContinueOnError)
	values := addBuildFlags(flags)
	if err := flags.Parse(args); err != nil {
		return err
	}
	options := values.options()
	options.Configure = false
	return project.Configure(options)
}

func runBuild(args []string) error {
	flags := flag.NewFlagSet("build", flag.ContinueOnError)
	values := addBuildFlags(flags)
	if err := flags.Parse(args); err != nil {
		return err
	}
	return project.Build(values.options())
}

func runAll(args []string) error {
	flags := flag.NewFlagSet("all", flag.ContinueOnError)
	regenValues := addRegenFlags(flags)
	buildValues := addBuildFlagsForAll(flags)
	if err := flags.Parse(args); err != nil {
		return err
	}
	if _, err := project.Regenerate(regenValues.options()); err != nil {
		return err
	}
	buildValues.root = regenValues.root
	buildValues.toolchainDir = regenValues.toolchainDir
	return project.Build(buildValues.options())
}

func addBuildFlagsForAll(flags *flag.FlagSet) *buildFlags {
	values := &buildFlags{}
	flags.StringVar(&values.buildDir, "build-dir", "build", "native build directory")
	flags.StringVar(&values.cmake, "cmake", "cmake", "CMake executable")
	flags.StringVar(&values.config, "config", "RelWithDebInfo", "CMake build configuration")
	flags.StringVar(&values.generator, "generator", "", "optional CMake generator")
	flags.StringVar(&values.prefixPath, "prefix-path", os.Getenv("CMAKE_PREFIX_PATH"), "CMake package prefix path")
	flags.IntVar(&values.jobs, "build-jobs", runtime.NumCPU(), "parallel native build jobs")
	flags.BoolVar(&values.buildOnly, "build-only", false, "skip CMake configure")
	flags.Var(&values.cmakeArgs, "cmake-arg", "additional CMake configure argument; repeat as needed")
	return values
}

func runDoctor(args []string) error {
	flags := flag.NewFlagSet("doctor", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	romPath := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	cmake := flags.String("cmake", "cmake", "CMake executable")
	requireBuild := flags.Bool("require-build", false, "fail unless native build dependencies are present")
	if err := flags.Parse(args); err != nil {
		return err
	}
	paths := project.DefaultPaths(*root)
	paths.ROM = *romPath
	resolved, err := paths.Resolve()
	if err != nil {
		return err
	}
	fmt.Printf("host            %s/%s\n", runtime.GOOS, runtime.GOARCH)
	fmt.Printf("project root    %s\n", resolved.Root)
	regenMissing := false
	checks := [][2]string{
		{"ROM", resolved.ROM},
		{"bank cfg dir", resolved.ConfigDir},
	}
	for _, check := range checks {
		if _, statErr := os.Stat(check[1]); statErr != nil {
			fmt.Printf("%-15s MISSING (%s)\n", check[0], check[1])
			regenMissing = true
		} else {
			fmt.Printf("%-15s ok (%s)\n", check[0], check[1])
		}
	}
	buildMissing := false
	runtimePath := filepath.Join(resolved.ToolchainDir, "runtime", "runner.cmake")
	if _, statErr := os.Stat(runtimePath); statErr != nil {
		fmt.Printf("%-15s MISSING (%s)\n", "runtime", runtimePath)
		buildMissing = true
	} else {
		fmt.Printf("%-15s ok (%s)\n", "runtime", runtimePath)
	}
	if path, lookErr := exec.LookPath(*cmake); lookErr != nil {
		fmt.Printf("cmake          MISSING (%s)\n", *cmake)
		buildMissing = true
	} else {
		fmt.Printf("cmake          ok (%s)\n", path)
	}
	if path, lookErr := exec.LookPath("go"); lookErr != nil {
		fmt.Println("go             optional/not found (needed only for source builds and --run-tests)")
	} else {
		fmt.Printf("go             optional/ok (%s)\n", path)
	}
	if regenMissing {
		return errors.New("regeneration inputs are incomplete")
	}
	fmt.Println("doctor: regeneration inputs are ready")
	if buildMissing {
		if *requireBuild {
			return errors.New("native build inputs are incomplete")
		}
		fmt.Println("doctor: native build inputs are incomplete (not required for regeneration)")
	} else {
		fmt.Println("doctor: build launcher inputs are ready; CMake will validate the compiler and frontend libraries during configure")
	}
	return nil
}
