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
	"strconv"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/project"
	"github.com/DerrickGold/snesrecomp-go/internal/toolchain"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
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
	case "analyze":
		return runAnalyze(args[1:])
	case "dispatch-census":
		return runDispatchCensus(args[1:])
	case "configure":
		return runConfigure(args[1:])
	case "build":
		return runBuild(args[1:])
	case "all":
		return runAll(args[1:])
	case "gui":
		return runGUI(args[1:])
	case "audio-preview":
		return runAudioPreview(args[1:])
	case "toolchain":
		return runToolchain(args[1:])
	case "runtime":
		return runRuntime(args[1:])
	case "sdl":
		return runSDL(args[1:])
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
  analyze     Compare inferred control-flow facts with authored cfg (read-only)
  dispatch-census
              Summarize observed runtime targets without editing authored cfg
  configure   Configure the native game build with CMake
  build       Configure (by default) and compile the native game
              (--hermetic compiles with the pinned Zig toolchain, no CMake)
  all         Regenerate, configure, and compile in one command
  gui         Open the local graphical hermetic game builder
  audio-preview
              Render local ActRaiser soundtrack comparison WAVs in pure Go
  toolchain   Report, fetch, or pin the hermetic C toolchain (Zig)
  runtime     Build a target-specific vended runner archive
  sdl         Stage the pinned SDL3 redistributable for a cross target
  doctor      Report host tools and project inputs
  version     Print the driver version and target platform

The binary itself has no runtime dependencies. Regeneration needs only a local
ROM. The default CMake build needs CMake, a C compiler, and the frontend
dependencies of the game project; --hermetic replaces CMake and the compiler
with the pinned Zig toolchain, leaving only the frontend's native libraries
(for example SDL3) as external inputs. Build commands use the independently
authored portable runner.`)
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
	zig, sdlInclude, sdlLib, optimize, target                          string
	verbose                                                            bool
}

func addHermeticFlags(flags *flag.FlagSet, values *buildFlags) {
	flags.BoolVar(&values.hermetic, "hermetic", false, "build with the pinned Zig toolchain instead of CMake")
	flags.StringVar(&values.zig, "zig", "", "Zig executable (default: $SNESBUILD_ZIG, project cache, then PATH)")
	flags.StringVar(&values.sdlInclude, "sdl-include", "", "SDL3 header directory (default: auto-discover)")
	flags.StringVar(&values.sdlLib, "sdl-lib", "", "SDL3 library directory (default: auto-discover)")
	flags.StringVar(&values.optimize, "optimize", "-O2", "hermetic optimization level")
	flags.StringVar(&values.target, "target", "",
		"cross-compile to a Zig target triple, e.g. x86_64-windows-gnu (default: host)")
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
		SDLIncludeDir: values.sdlInclude, SDLLibDir: values.sdlLib, Target: values.target,
		Verbose: values.verbose,
		Stdout:  os.Stdout, Stderr: os.Stderr,
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

func runAnalyze(args []string) error {
	flags := flag.NewFlagSet("analyze", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	romPath := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	cfgDir := flags.String("cfg-dir", "recomp", "bank config directory, relative to project root")
	jobs := flags.Int("jobs", runtime.NumCPU(), "parallel decode workers")
	bankValue := flags.String("bank", "", "optional hexadecimal bank")
	format := flags.String("format", "text", "report format: text or json")
	verbose := flags.Bool("verbose", false, "show all comparisons, callers, and decode issues")
	flags.BoolVar(verbose, "v", false, "show all comparisons, callers, and decode issues")
	strict := flags.Bool("strict", false, "fail after reporting independently proven semantic conflicts")
	compareAuthored := flags.Bool("compare-authored", true, "compare inferred facts with authored cfg declarations")
	noWrite := flags.Bool("no-write", true, "require analysis to remain read-only")
	dryRun := flags.Bool("dry-run", true, "require analysis to remain read-only")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if !*compareAuthored {
		return errors.New("analyze currently requires --compare-authored")
	}
	if !*noWrite || !*dryRun {
		return errors.New("analyze is intentionally read-only; disabling --no-write or --dry-run is not supported")
	}
	paths := project.DefaultPaths(*root)
	paths.ROM, paths.ConfigDir = *romPath, *cfgDir
	resolved, err := paths.Resolve()
	if err != nil {
		return err
	}
	var onlyBank *byte
	if strings.TrimSpace(*bankValue) != "" {
		value, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(*bankValue), "0x"), "0X"), 16, 8)
		if err != nil {
			return fmt.Errorf("parse --bank: %w", err)
		}
		bank := byte(value)
		onlyBank = &bank
	}
	report, err := tooling.AnalyzeAuthoredShadow(tooling.ShadowAnalysisOptions{
		ROMPath: resolved.ROM, CFGDir: resolved.ConfigDir, Jobs: *jobs, OnlyBank: onlyBank,
	})
	if err != nil {
		return err
	}
	if err := tooling.WriteShadowReport(os.Stdout, report, *format, *verbose); err != nil {
		return err
	}
	if *strict && report.Summary.Conflicts > 0 {
		return fmt.Errorf("shadow analysis found %d semantic conflict(s)", report.Summary.Conflicts)
	}
	return nil
}

func runDispatchCensus(args []string) error {
	flags := flag.NewFlagSet("dispatch-census", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	tracePath := flags.String("trace", "", "runtime JSONL captured with SNESRECOMP_TRACE_CHANNELS=dispatch, relative to --root unless absolute")
	romPath := flags.String("rom", "", "optional ROM path, relative to project root")
	format := flags.String("format", "text", "report format: text or json")
	suggest := flags.Bool("suggest", true, "include candidate cfg func lines for missing generated bodies")
	outPath := flags.String("out-analysis", "", "optional deterministic JSON evidence path, relative to project root")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if strings.TrimSpace(*tracePath) == "" {
		return errors.New("dispatch-census requires --trace <runtime.jsonl>")
	}
	resolve := func(path string) string {
		if path == "" || filepath.IsAbs(path) {
			return path
		}
		return filepath.Join(*root, path)
	}
	report, err := tooling.LoadDispatchCensus(resolve(*tracePath), resolve(*romPath))
	if err != nil {
		return err
	}
	if err := tooling.WriteDispatchCensus(os.Stdout, report, *format, *suggest); err != nil {
		return err
	}
	if strings.TrimSpace(*outPath) != "" {
		resolved := resolve(*outPath)
		if err := tooling.WriteDispatchCensusFile(resolved, report); err != nil {
			return err
		}
		fmt.Fprintf(os.Stderr, "dispatch-census: wrote observed evidence to %s (authored cfg unchanged)\n", resolved)
	}
	return nil
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
	sdl := flags.Bool("sdl", false, "print the SDL3 pin (url sha archive kind) instead of the Zig pin")
	steamDeckSDL := flags.Bool(
		"steam-deck-sdl", false,
		"print Steam Deck SDL3 header/runtime pins instead of the Zig pin")
	subcommand := "status"
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		subcommand, args = args[0], args[1:]
	}
	if err := flags.Parse(args); err != nil {
		return err
	}
	if subcommand == "pin" {
		if *sdl && *steamDeckSDL {
			return fmt.Errorf("--sdl and --steam-deck-sdl are mutually exclusive")
		}
		if *steamDeckSDL {
			headersURL, headersSHA, headersArchive,
				runtimeURL, runtimeSHA, runtimeArchive :=
				toolchain.SteamDeckSDL3Pins()
			fmt.Printf("%s %s %s %s %s %s\n",
				headersURL, headersSHA, headersArchive,
				runtimeURL, runtimeSHA, runtimeArchive)
			return nil
		}
		if *sdl {
			url, sha, archive, kind, err := toolchain.SDL3Pin(*goos, *goarch)
			if err != nil {
				return err
			}
			fmt.Printf("%s %s %s %s\n", url, sha, archive, kind)
			return nil
		}
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

func runRuntime(args []string) error {
	flags := flag.NewFlagSet("runtime", flag.ContinueOnError)
	runtimeDir := flags.String("runtime-dir", "snesrecomp-go/runtime",
		"runner source directory")
	output := flags.String("output", "",
		"archive output (default <runtime-dir>/lib/<target>/<archive>)")
	target := flags.String("target", "", "Zig target triple (default: host)")
	zigPath := flags.String("zig", "", "Zig executable")
	cacheDir := flags.String("cache-dir", "", "host Zig cache directory")
	optimize := flags.String("optimize", "-O2", "optimization level")
	jobs := flags.Int("jobs", runtime.NumCPU(), "parallel compile jobs")
	portable := flags.Bool("portable", false, "disable target SIMD implementations")
	verbose := flags.Bool("verbose", false, "print each runner source as it compiles")
	subcommand := "archive"
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		subcommand, args = args[0], args[1:]
	}
	if err := flags.Parse(args); err != nil {
		return err
	}
	if subcommand != "archive" {
		return fmt.Errorf("unknown runtime subcommand %q (expected archive)", subcommand)
	}
	zig := *zigPath
	if zig == "" {
		cache := *cacheDir
		if cache == "" {
			cache = toolchainCacheDir(".")
		}
		located, err := toolchain.Locate(cache)
		if err != nil {
			located, err = toolchain.Fetch(cache, os.Stdout)
			if err != nil {
				return err
			}
		}
		fmt.Printf("runtime archive: using Zig %s (%s, via %s)\n",
			located.Version, located.Path, located.Source)
		zig = located.Path
	}
	_, err := project.BuildRuntimeArchive(project.RuntimeArchiveOptions{
		RuntimeDir: *runtimeDir, OutputPath: *output, ZigPath: zig,
		Target: *target, Optimize: *optimize, Jobs: *jobs,
		SIMD: !*portable, Verbose: *verbose, Stdout: os.Stdout,
	})
	return err
}

// runSDL stages the pinned SDL3 redistributable for a cross target. It exists
// so `build --hermetic --target <t>` has a real SDL to link against: the host's
// own SDL3 is never right for another platform, and the point of a cross build
// is to prove the link a user's machine would do, not an approximation of it.
func runSDL(args []string) error {
	flags := flag.NewFlagSet("sdl", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	buildDir := flags.String("build-dir", "build", "native build directory")
	target := flags.String("target", "", "Zig target triple, e.g. x86_64-windows-gnu")
	cacheDir := flags.String("cache-dir", "", "download cache (default <root>/build/toolchain)")
	stageDir := flags.String("dir", "", "stage directory (default <build-dir>/hermetic/<target>/sdl3)")
	subcommand := "stage"
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		subcommand, args = args[0], args[1:]
	}
	if err := flags.Parse(args); err != nil {
		return err
	}
	if subcommand != "stage" {
		return fmt.Errorf("unknown sdl subcommand %q (expected stage)", subcommand)
	}
	if *target == "" {
		return fmt.Errorf("sdl stage needs --target (e.g. --target x86_64-windows-gnu)")
	}
	cache := *cacheDir
	if cache == "" {
		cache = toolchainCacheDir(*root)
	}
	stage := *stageDir
	if stage == "" {
		// Resolve through Paths rather than joining by hand so an absolute
		// --build-dir lands in the same place the build itself will look.
		paths := project.DefaultPaths(*root)
		paths.BuildDir = *buildDir
		resolved, err := paths.Resolve()
		if err != nil {
			return err
		}
		stage = project.CrossSDL3Dir(resolved.BuildDir, *target)
	}
	return toolchain.StageSDL3(*target, cache, stage, os.Stdout)
}

func runDoctor(args []string) error {
	flags := flag.NewFlagSet("doctor", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	romPath := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	cmake := flags.String("cmake", "cmake", "CMake executable")
	toolchainDir := flags.String("toolchain-dir", "snesrecomp-go", "snesrecomp-go module directory")
	requireBuild := flags.Bool("require-build", false, "fail unless native build dependencies are present")
	if err := flags.Parse(args); err != nil {
		return err
	}
	paths := project.DefaultPaths(*root)
	paths.ROM = *romPath
	paths.ToolchainDir = *toolchainDir
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
	runtimeDir := project.RunnerDirectory(resolved.ToolchainDir)
	runtimePath := filepath.Join(runtimeDir, "runner.cmake")
	archivePath, archiveErr := project.VendedRuntimeArchivePath(runtimeDir, "")
	archiveInfo, archiveStatErr := os.Stat(archivePath)
	includeInfo, includeStatErr := os.Stat(filepath.Join(runtimeDir, "include"))
	if archiveErr == nil && archiveStatErr == nil &&
		archiveInfo.Mode().IsRegular() && archiveInfo.Size() > 0 &&
		includeStatErr == nil && includeInfo.IsDir() {
		fmt.Printf("%-15s ok (vended archive %s)\n", "runner", archivePath)
	} else if _, statErr := os.Stat(runtimePath); statErr == nil {
		fmt.Printf("%-15s ok (%s source fallback)\n", "runner", project.RunnerName)
	} else {
		fmt.Printf("%-15s MISSING (%s or matching runtime/lib archive)\n",
			"runner", runtimePath)
		buildMissing = true
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
	manifest, manifestErr := project.LoadManifest(manifestPath)
	if manifestErr != nil {
		fmt.Printf("%-15s not found (%s; hermetic builds need it)\n", "snesbuild.ini", manifestPath)
	} else {
		fmt.Printf("%-15s ok (%d game sources, target %s)\n", "snesbuild.ini", len(manifest.Sources), manifest.Name)
		for _, warning := range project.ManifestDriftWarnings(resolved.Root, manifest) {
			fmt.Printf("%-15s WARNING %s\n", "snesbuild.ini", warning)
		}
	}
	// Report symbols the project appears to owe before leaving the linker to
	// name an unfamiliar symbol. The checker fails only for identifiers absent
	// from authored build sources; unusual macro-authored definitions remain a
	// warning because the native linker is the final authority.
	var manifestSources []string
	if manifestErr == nil {
		manifestSources = manifest.Sources
	}
	contractMissing, contractErr := reportGameContract(
		os.Stdout, resolved.Root, resolved.ConfigDir, resolved.GeneratedDir,
		filepath.Join(runtimeDir, "include", "snesrecomp", "game",
			"required_symbols.h"), manifestSources)
	if contractErr != nil {
		fmt.Printf("%-15s SKIPPED (%v)\n", "game contract", contractErr)
	}
	if regenMissing {
		return errors.New("regeneration inputs are incomplete")
	}
	fmt.Println("doctor: regeneration inputs are ready")
	if contractMissing {
		return errors.New("the project does not define every symbol the runner requires")
	}
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
