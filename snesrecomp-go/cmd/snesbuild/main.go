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
	"github.com/DerrickGold/snesrecomp-go/internal/toolchain"
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
	case "toolchain":
		return runToolchain(args[1:])
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
              (--hermetic compiles with the pinned Zig toolchain, no CMake)
  all         Regenerate, configure, and compile in one command
  toolchain   Report, fetch, or pin the hermetic C toolchain (Zig)
  doctor      Report host tools and project inputs
  version     Print the driver version and target platform

The binary itself has no runtime dependencies. Regeneration needs only a local
ROM. The default CMake build needs CMake, a C compiler, and the frontend
dependencies of the game project; --hermetic replaces CMake and the compiler
with the pinned Zig toolchain, leaving only the frontend's native libraries
(for example SDL2) as external inputs.`)
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
	hermetic                                                           bool
	zig, sdlInclude, sdlLib, optimize                                  string
	verbose                                                            bool
}

func addHermeticFlags(flags *flag.FlagSet, values *buildFlags) {
	flags.BoolVar(&values.hermetic, "hermetic", false, "build with the pinned Zig toolchain instead of CMake")
	flags.StringVar(&values.zig, "zig", "", "Zig executable (default: $SNESBUILD_ZIG, project cache, then PATH)")
	flags.StringVar(&values.sdlInclude, "sdl-include", "", "SDL2 header directory (default: auto-discover)")
	flags.StringVar(&values.sdlLib, "sdl-lib", "", "SDL2 library directory (default: auto-discover)")
	flags.StringVar(&values.optimize, "optimize", "-O2", "hermetic optimization level")
	flags.BoolVar(&values.verbose, "verbose", false, "print each hermetic compile command")
}

func toolchainCacheDir(root string) string {
	return filepath.Join(root, "build", "toolchain")
}

func (values *buildFlags) hermeticOptions() (project.HermeticOptions, error) {
	paths := project.DefaultPaths(values.root)
	paths.BuildDir, paths.ToolchainDir = values.buildDir, values.toolchainDir
	zigPath := values.zig
	if zigPath == "" {
		located, err := toolchain.Locate(toolchainCacheDir(values.root))
		if err != nil {
			return project.HermeticOptions{}, err
		}
		fmt.Printf("hermetic: using Zig %s (%s, via %s)\n", located.Version, located.Path, located.Source)
		zigPath = located.Path
	}
	return project.HermeticOptions{
		Paths: paths, ZigPath: zigPath, Jobs: values.jobs, Optimize: values.optimize,
		SDLIncludeDir: values.sdlInclude, SDLLibDir: values.sdlLib, Verbose: values.verbose,
		Stdout: os.Stdout, Stderr: os.Stderr,
	}, nil
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
	addHermeticFlags(flags, values)
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
	if values.hermetic {
		options, err := values.hermeticOptions()
		if err != nil {
			return err
		}
		_, err = project.HermeticBuild(options)
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
	if buildValues.hermetic {
		options, err := buildValues.hermeticOptions()
		if err != nil {
			return err
		}
		_, err = project.HermeticBuild(options)
		return err
	}
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
	addHermeticFlags(flags, values)
	return values
}

func runToolchain(args []string) error {
	flags := flag.NewFlagSet("toolchain", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	cacheDir := flags.String("cache-dir", "", "toolchain cache directory (default <root>/build/toolchain)")
	goos := flags.String("goos", runtime.GOOS, "target OS for `pin`")
	goarch := flags.String("goarch", runtime.GOARCH, "target architecture for `pin`")
	subcommand := "status"
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		subcommand, args = args[0], args[1:]
	}
	if err := flags.Parse(args); err != nil {
		return err
	}
	if subcommand == "pin" {
		url, sha, archive, err := toolchain.Pin(*goos, *goarch)
		if err != nil {
			return err
		}
		fmt.Printf("%s %s %s\n", url, sha, archive)
		return nil
	}
	cache := *cacheDir
	if cache == "" {
		cache = toolchainCacheDir(*root)
	}
	switch subcommand {
	case "status":
		url, sha, err := toolchain.PinnedURL()
		if err != nil {
			return err
		}
		fmt.Printf("pinned Zig      %s\n", toolchain.PinnedZigVersion)
		fmt.Printf("release         %s\n", url)
		fmt.Printf("sha256          %s\n", sha)
		located, err := toolchain.Locate(cache)
		if err != nil {
			fmt.Printf("local zig       MISSING\n")
			return err
		}
		fmt.Printf("local zig       %s (%s, via %s)\n", located.Version, located.Path, located.Source)
		if located.Version != toolchain.PinnedZigVersion {
			fmt.Printf("note            local version differs from the pin; hermetic release builds should use %s\n", toolchain.PinnedZigVersion)
		}
		return nil
	case "fetch":
		_, err := toolchain.Fetch(cache, os.Stdout)
		return err
	default:
		return fmt.Errorf("unknown toolchain subcommand %q (expected status, fetch, or pin)", subcommand)
	}
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
	if zig, zigErr := toolchain.Locate(toolchainCacheDir(*root)); zigErr != nil {
		fmt.Printf("zig            not found (hermetic builds need it: `snesbuild toolchain fetch`)\n")
	} else {
		fmt.Printf("zig            ok (%s, %s, via %s)\n", zig.Version, zig.Path, zig.Source)
	}
	manifestPath := filepath.Join(resolved.Root, project.ManifestFileName)
	if manifest, manifestErr := project.LoadManifest(manifestPath); manifestErr != nil {
		fmt.Printf("%-15s not found (%s; hermetic builds need it)\n", "snesbuild.ini", manifestPath)
	} else {
		fmt.Printf("%-15s ok (%d game sources, target %s)\n", "snesbuild.ini", len(manifest.Sources), manifest.Name)
		for _, warning := range project.ManifestDriftWarnings(resolved.Root, manifest) {
			fmt.Printf("%-15s WARNING %s\n", "snesbuild.ini", warning)
		}
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
