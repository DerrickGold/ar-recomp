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
	case "xref":
		return runXref(args[1:])
	case "disasm":
		return runDisasm(args[1:])
	case "rom-info":
		return runROMInfo(args[1:])
	case "spc-disasm":
		return runSPCDisasm(args[1:])
	case "apu-audit":
		return runAPUAudit(args[1:])
	case "quintet-lzss":
		return runQuintetLZSS(args[1:])
	case "poll-census":
		return runPollCensus(args[1:])
	case "rts-webs":
		return runRTSWebs(args[1:])
	case "link-audit":
		return runLinkAudit(args[1:])
	case "dispatch-census":
		return runDispatchCensus(args[1:])
	case "trace-inspect":
		return runTraceInspect(args[1:])
	case "trace-diff":
		return runTraceDiff(args[1:])
	case "replay-bench":
		return runReplayBench(args[1:])
	case "wram":
		return tooling.RunWRAMCommand(args[1:], ".", os.Stdout)
	case "mx-diff":
		return tooling.RunMXDiffCommand(args[1:], ".", os.Stdout)
	case "chr-render":
		return tooling.RunCHRRenderCommand(args[1:], ".", os.Stdout)
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
  xref        Find decoded instruction references to an address (read-only)
  disasm      Disassemble ROM code with live M/X tracking (read-only)
  rom-info    Report cartridge identity, header, and vectors (read-only)
  spc-disasm  Disassemble an SPC700 payload or ROM upload block (read-only)
  apu-audit   Validate live BRR samples and CPU/APU port handshakes
  quintet-lzss
              Decode a bit-packed Quintet LZSS blob
  poll-census Classify decoded hardware-status read and polling sites
  rts-webs    Census pushed and stack-captured continuation patterns
  link-audit  Audit generated reachability, traps, and tail-call suspects
  dispatch-census
              Summarize observed runtime targets without editing authored cfg
  trace-inspect
              Slice, summarize, and diagnose unified runtime traces
  trace-diff  Compare reference and recompiled WRAM traces (read-only)
  replay-bench
              Run deterministic manifest-defined replay benchmarks and A/B gates
  wram        Inspect, compare, and scan WRAM snapshots (read-only)
  mx-diff     Compare game-frame M/X traces (read-only)
  chr-render  Render SNES 4bpp ROM, VRAM, and icon sheets
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
	dispatchAnalysis := flags.String("dispatch-analysis", "", "optional dispatch-census JSON evidence, relative to project root")
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
		DispatchAnalysisPath: resolveProjectOptional(resolved.Root, *dispatchAnalysis),
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

func resolveProjectOptional(root, path string) string {
	if strings.TrimSpace(path) == "" || filepath.IsAbs(path) {
		return path
	}
	return filepath.Join(root, path)
}

func runXref(args []string) error {
	address := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		address, args = args[0], args[1:]
	}
	flags := flag.NewFlagSet("xref", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	romPath := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	cfgDir := flags.String("cfg-dir", "recomp", "bank config directory, relative to project root")
	jobs := flags.Int("jobs", runtime.NumCPU(), "parallel decode workers")
	bankValue := flags.String("bank", "", "optional hexadecimal source bank")
	kind := flags.String("kind", "all", "access filter: all, read, write, read-write, control, branch, or pointer-read")
	wramMirrors := flags.Bool("wram-mirrors", false, "for a 16-bit query, include long bank $00/$7E/$7F WRAM mirrors")
	rawWords := flags.Bool("data-words", false, "also scan raw ROM words as explicitly unowned evidence")
	targetMinusOne := flags.Bool("target-minus-one", false, "with --data-words, also match target-1 continuation tables")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *targetMinusOne && !*rawWords {
		return errors.New("xref --target-minus-one requires --data-words")
	}
	if address == "" && flags.NArg() == 1 {
		address = flags.Arg(0)
	} else if flags.NArg() != 0 {
		return errors.New("xref needs exactly one address")
	}
	if strings.TrimSpace(address) == "" {
		return errors.New("xref needs an address such as $1C, $C210, or $00:C210")
	}
	query, err := tooling.ParseXrefQuery(address)
	if err != nil {
		return err
	}
	paths := project.DefaultPaths(*root)
	paths.ROM, paths.ConfigDir = *romPath, *cfgDir
	resolved, err := paths.Resolve()
	if err != nil {
		return err
	}
	var onlyBank *byte
	if strings.TrimSpace(*bankValue) != "" {
		value, parseErr := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(*bankValue), "0x"), "0X"), 16, 8)
		if parseErr != nil {
			return fmt.Errorf("parse --bank: %w", parseErr)
		}
		bank := byte(value)
		onlyBank = &bank
	}
	report, err := tooling.BuildXref(tooling.XrefOptions{
		ROMPath: resolved.ROM, CFGDir: resolved.ConfigDir, Jobs: *jobs,
		OnlyBank: onlyBank, Query: query, AccessFilter: *kind, IncludeWRAMMirrors: *wramMirrors,
		IncludeRawWords: *rawWords, IncludeTargetMinusOne: *targetMinusOne,
	})
	if err != nil {
		return err
	}
	return tooling.WriteXrefReport(os.Stdout, report, *format)
}

func runDisasm(args []string) error {
	address := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		address, args = args[0], args[1:]
	}
	flags := flag.NewFlagSet("disasm", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	romPath := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	metadataPath := flags.String("metadata", "saves/gen_meta.json", "optional metadata path, relative to project root")
	mx := flags.String("mx", "0,0", "entry widths as m,x")
	count := flags.Int("count", 24, "maximum instruction count")
	flags.IntVar(count, "n", 24, "maximum instruction count")
	untilFlow := flags.Bool("until-flow", false, "stop after a return or unconditional transfer")
	raw := flags.Bool("raw", false, "show instruction bytes in text output")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if address == "" && flags.NArg() == 1 {
		address = flags.Arg(0)
	} else if flags.NArg() != 0 {
		return errors.New("disasm needs exactly one BB:AAAA address")
	}
	if strings.TrimSpace(address) == "" {
		return errors.New("disasm needs an address such as $01:9C6F")
	}
	startPC, err := tooling.ParseProgramAddress(address)
	if err != nil {
		return err
	}
	parts := strings.Split(*mx, ",")
	if len(parts) != 2 {
		return fmt.Errorf("parse --mx %q (want m,x with each value 0 or 1)", *mx)
	}
	parseWidth := func(name, value string) (uint8, error) {
		parsed, parseErr := strconv.ParseUint(strings.TrimSpace(value), 10, 1)
		if parseErr != nil {
			return 0, fmt.Errorf("parse --mx %s=%q (want 0 or 1)", name, value)
		}
		return uint8(parsed), nil
	}
	m, err := parseWidth("m", parts[0])
	if err != nil {
		return err
	}
	x, err := parseWidth("x", parts[1])
	if err != nil {
		return err
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	report, err := tooling.BuildDisassembly(tooling.DisassemblyOptions{
		ROMPath: resolveProjectOptional(absoluteRoot, *romPath), MetadataPath: resolveProjectOptional(absoluteRoot, *metadataPath),
		StartPC: startPC, EntryM: m, EntryX: x, Count: *count, UntilFlow: *untilFlow,
	})
	if err != nil {
		return err
	}
	return tooling.WriteDisassemblyReport(os.Stdout, report, *format, *raw)
}

func runROMInfo(args []string) error {
	flags := flag.NewFlagSet("rom-info", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	romPath := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() > 1 {
		return errors.New("rom-info accepts at most one positional ROM path")
	}
	if flags.NArg() == 1 {
		*romPath = flags.Arg(0)
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	report, err := tooling.BuildROMInfo(tooling.ROMInfoOptions{ROMPath: resolveProjectOptional(absoluteRoot, *romPath)})
	if err != nil {
		return err
	}
	return tooling.WriteROMInfo(os.Stdout, report, *format)
}

func runSPCDisasm(args []string) error {
	if len(args) < 2 || strings.HasPrefix(args[0], "-") || strings.HasPrefix(args[1], "-") {
		return errors.New("spc-disasm needs start and end ARAM addresses before its options")
	}
	startText, endText := args[0], args[1]
	args = args[2:]
	flags := flag.NewFlagSet("spc-disasm", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	inputPath := flags.String("input", "game.sfc", "ROM or raw payload path, relative to project root")
	uploadOffsetValue := flags.String("upload-block", "", "optional file offset of [length16][ARAM target16][payload]")
	fileOffsetValue := flags.String("file-offset", "0", "raw payload file offset when --upload-block is absent")
	loadAddressValue := flags.String("load-address", "0", "raw payload ARAM load address when --upload-block is absent")
	findReferenceValue := flags.String("find-ref", "", "only instructions with this literal DP/absolute operand")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	start, err := tooling.ParseSPCAddress(startText)
	if err != nil {
		return err
	}
	end, err := tooling.ParseSPCAddress(endText)
	if err != nil {
		return err
	}
	parseOffset := func(name, text string) (int, error) {
		value, parseErr := strconv.ParseInt(strings.TrimSpace(text), 0, 64)
		if parseErr != nil || value < 0 || int64(int(value)) != value {
			return 0, fmt.Errorf("parse %s %q as non-negative file offset", name, text)
		}
		return int(value), nil
	}
	fileOffset, err := parseOffset("--file-offset", *fileOffsetValue)
	if err != nil {
		return err
	}
	loadAddress, err := tooling.ParseSPCAddress(*loadAddressValue)
	if err != nil {
		return fmt.Errorf("parse --load-address: %w", err)
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	options := tooling.SPCDisassemblyOptions{
		InputPath: resolveProjectOptional(absoluteRoot, *inputPath), FileOffset: fileOffset, LoadAddress: loadAddress,
		StartAddress: start, EndAddress: end,
	}
	if strings.TrimSpace(*uploadOffsetValue) != "" {
		value, err := parseOffset("--upload-block", *uploadOffsetValue)
		if err != nil {
			return err
		}
		options.UploadBlockOffset = &value
	}
	if strings.TrimSpace(*findReferenceValue) != "" {
		value, err := tooling.ParseSPCAddress(*findReferenceValue)
		if err != nil {
			return fmt.Errorf("parse --find-ref: %w", err)
		}
		options.FindReference = &value
	}
	report, err := tooling.BuildSPCDisassembly(options)
	if err != nil {
		return err
	}
	return tooling.WriteSPCDisassembly(os.Stdout, report, *format)
}

func runAPUAudit(args []string) error {
	flags := flag.NewFlagSet("apu-audit", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	prefix := flags.String("prefix", "", "capture prefix for .aram, .dsp, .written, and .audio.jsonl sidecars")
	spcPath := flags.String("spc", "", "standard SPC snapshot (alternative to --aram/--dsp)")
	aramPath := flags.String("aram", "", "64 KiB raw ARAM snapshot")
	dspPath := flags.String("dsp", "", "128-byte raw DSP register snapshot")
	writtenPath := flags.String("written", "", "optional 8192-byte ARAM write-coverage bitmap")
	tracePath := flags.String("trace", "", "optional snesrecomp audio event JSONL")
	directoryValue := flags.String("directory-page", "", "override DSP DIR page as hexadecimal byte")
	sourceValue := flags.String("sources", "", "additional comma-separated hexadecimal SRCN values")
	format := flags.String("format", "text", "report format: text or json")
	strict := flags.Bool("strict", false, "fail after reporting an invalid live sample")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("apu-audit accepts only named options")
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	resolve := func(value string) string {
		return resolveProjectOptional(absoluteRoot, value)
	}
	sources, err := tooling.ParseAPUSourceList(*sourceValue)
	if err != nil {
		return err
	}
	var directoryPage *uint8
	if strings.TrimSpace(*directoryValue) != "" {
		address, parseErr := tooling.ParseSPCAddress(*directoryValue)
		if parseErr != nil || address > 0xff {
			return fmt.Errorf("parse --directory-page %q (want hexadecimal byte)", *directoryValue)
		}
		value := uint8(address)
		directoryPage = &value
	}
	report, err := tooling.BuildAPUAudit(tooling.APUAuditOptions{
		Prefix: resolve(*prefix), SPCPath: resolve(*spcPath), ARAMPath: resolve(*aramPath),
		DSPPath: resolve(*dspPath), WrittenPath: resolve(*writtenPath), TracePath: resolve(*tracePath),
		DirectoryPage: directoryPage, Sources: sources,
	})
	if err != nil {
		return err
	}
	if err := tooling.WriteAPUAudit(os.Stdout, report, *format); err != nil {
		return err
	}
	if *strict && report.Summary.InvalidSamples != 0 {
		return fmt.Errorf("APU audit found %d invalid live sample(s)", report.Summary.InvalidSamples)
	}
	return nil
}

func runQuintetLZSS(args []string) error {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return errors.New("quintet-lzss needs a linear input offset before its options")
	}
	offsetText, args := args[0], args[1:]
	flags := flag.NewFlagSet("quintet-lzss", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	inputPath := flags.String("input", "game.sfc", "ROM or compressed input path, relative to project root")
	size := flags.Int("size", 0, "exact decompressed size (default: little-endian word at offset)")
	outputPath := flags.String("out", "", "optional output path, relative to project root")
	comparePath := flags.String("compare", "", "optional expected binary, relative to project root")
	compareOffset := flags.Int("compare-offset", 0, "byte offset within --compare")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	offset, err := strconv.ParseInt(strings.TrimSpace(offsetText), 0, 64)
	if err != nil || offset < 0 || int64(int(offset)) != offset {
		return fmt.Errorf("parse input offset %q as a non-negative integer", offsetText)
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	headered := true
	flags.Visit(func(item *flag.Flag) {
		if item.Name == "size" {
			headered = false
		}
	})
	report, output, err := tooling.BuildQuintetLZSS(tooling.QuintetLZSSOptions{
		InputPath: resolveProjectOptional(absoluteRoot, *inputPath), Offset: int(offset), Size: *size, Headered: headered,
		ComparePath: resolveProjectOptional(absoluteRoot, *comparePath), CompareOffset: *compareOffset,
	})
	if err != nil {
		return err
	}
	if strings.TrimSpace(*outputPath) != "" {
		resolved := resolveProjectOptional(absoluteRoot, *outputPath)
		if err := os.WriteFile(resolved, output, 0o644); err != nil {
			return fmt.Errorf("write decompressed output %s: %w", resolved, err)
		}
		report.NoWrite = false
		fmt.Fprintf(os.Stderr, "quintet-lzss: wrote %d bytes to %s\n", len(output), resolved)
	}
	return tooling.WriteQuintetLZSSReport(os.Stdout, report, *format)
}

func runPollCensus(args []string) error {
	flags := flag.NewFlagSet("poll-census", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	romPath := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	cfgDir := flags.String("cfg-dir", "recomp", "bank config directory, relative to project root")
	jobs := flags.Int("jobs", runtime.NumCPU(), "parallel decode workers")
	bankValue := flags.String("bank", "", "optional hexadecimal source bank")
	registerValue := flags.String("registers", "4210,4212", "comma-separated 16-bit addresses (hardware status, APU ports, or WRAM flags)")
	interruptSync := flags.Bool("interrupt-sync", true, "also census low-WRAM flags written by decoded NMI/IRQ ownership")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	registers, err := parsePollRegisters(*registerValue)
	if err != nil {
		return err
	}
	paths := project.DefaultPaths(*root)
	paths.ROM, paths.ConfigDir = *romPath, *cfgDir
	resolved, err := paths.Resolve()
	if err != nil {
		return err
	}
	var onlyBank *byte
	if strings.TrimSpace(*bankValue) != "" {
		value, parseErr := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(*bankValue), "0x"), "0X"), 16, 8)
		if parseErr != nil {
			return fmt.Errorf("parse --bank: %w", parseErr)
		}
		bank := byte(value)
		onlyBank = &bank
	}
	report, err := tooling.BuildPollCensus(tooling.PollCensusOptions{
		ROMPath: resolved.ROM, CFGDir: resolved.ConfigDir, Jobs: *jobs, OnlyBank: onlyBank, Registers: registers,
		DiscoverInterruptSync: *interruptSync,
	})
	if err != nil {
		return err
	}
	return tooling.WritePollCensus(os.Stdout, report, *format)
}

func runRTSWebs(args []string) error {
	flags := flag.NewFlagSet("rts-webs", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	romPath := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	cfgDir := flags.String("cfg-dir", "recomp", "configuration directory, relative to project root")
	bankValue := flags.String("bank", "", "optional hexadecimal bank")
	suggest := flags.Bool("suggest", false, "print review-only cfg suggestions")
	yieldHelpers := flags.Bool("yield-helpers", false, "also detect JSR helpers that capture return PCs into small indexed fields")
	yieldFieldMax := flags.Uint("yield-field-max", 0x40, "exclusive maximum indexed field offset for --yield-helpers")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *yieldFieldMax == 0 || *yieldFieldMax > 0x100 {
		return errors.New("--yield-field-max must be within 1-0x100")
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	var bank *byte
	if strings.TrimSpace(*bankValue) != "" {
		value, parseErr := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(*bankValue), "0x"), "0X"), 16, 8)
		if parseErr != nil {
			return fmt.Errorf("parse --bank: %w", parseErr)
		}
		parsed := byte(value)
		bank = &parsed
	}
	_, err = tooling.CensusRTSWebs(tooling.RTSCensusOptions{
		ROMPath: resolveProjectOptional(absoluteRoot, *romPath), CFGDir: resolveProjectOptional(absoluteRoot, *cfgDir),
		Bank: bank, Suggest: *suggest, YieldHelpers: *yieldHelpers, YieldFieldMax: uint16(*yieldFieldMax), Output: os.Stdout,
	})
	return err
}

func parsePollRegisters(value string) ([]uint16, error) {
	var registers []uint16
	seen := make(map[uint16]struct{})
	for _, part := range strings.Split(value, ",") {
		text := strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(part), "0x"), "0X")
		text = strings.TrimPrefix(text, "$")
		parsed, err := strconv.ParseUint(text, 16, 16)
		if err != nil {
			return nil, fmt.Errorf("parse poll address %q: %w", part, err)
		}
		register := uint16(parsed)
		if _, duplicate := seen[register]; !duplicate {
			seen[register] = struct{}{}
			registers = append(registers, register)
		}
	}
	if len(registers) == 0 {
		return nil, errors.New("poll-census needs at least one 16-bit address")
	}
	return registers, nil
}

func runLinkAudit(args []string) error {
	flags := flag.NewFlagSet("link-audit", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	genDir := flags.String("gen-dir", "src/gen", "generated C directory, relative to project root")
	sourceDir := flags.String("src-dir", "src", "hand-written game source directory, relative to project root")
	runtimeDir := flags.String("runtime-dir", "snesrecomp-go/runtime/src", "runtime source directory, relative to project root")
	orphans := flags.Bool("orphans", false, "list every orphan function")
	verbose := flags.Bool("verbose", false, "show partial M/X variant coverage")
	flags.BoolVar(verbose, "v", false, "show partial M/X variant coverage")
	tailCalls := flags.Bool("tailcalls", false, "rank repeated tail-call-past-end source/target pairs")
	tailCallMinimum := flags.Int("tailcall-min", 2, "minimum repeated source/target count to list")
	if err := flags.Parse(args); err != nil {
		return err
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	return tooling.RunLinkAudit(tooling.LinkAuditOptions{
		GenDir: resolveProjectOptional(absoluteRoot, *genDir), SourceDir: resolveProjectOptional(absoluteRoot, *sourceDir),
		RuntimeDir: resolveProjectOptional(absoluteRoot, *runtimeDir), ListOrphans: *orphans, Verbose: *verbose,
		ListTailCalls: *tailCalls, TailCallMinimum: *tailCallMinimum, Output: os.Stdout,
	})
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

func runTraceInspect(args []string) error {
	tracePath := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		tracePath, args = args[0], args[1:]
	}
	flags := flag.NewFlagSet("trace-inspect", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	summary := flags.Bool("summary", false, "show aggregate channel and diagnostic counts")
	diagnose := flags.Bool("diagnose", false, "rank M/X, garbage, and dispatch-miss findings")
	metadataPath := flags.String("metadata", "saves/gen_meta.json", "optional metadata path, relative to project root")
	romPath := flags.String("rom", "", "optional ROM path for paired-call guards, relative to project root")
	channelValue := flags.String("ch", "", "comma-separated channels to include")
	functionValue := flags.String("fn", "", "only events whose function contains this text")
	misdecodes := flags.Bool("misdecodes", false, "only mismatched function-entry M/X events")
	leaks := flags.Bool("leaks", false, "only call-site M/X leak events")
	vmadd := flags.Bool("vmadd", false, "only VMADD events")
	vramValue := flags.String("vram", "", "VRAM word address or hexadecimal low-high range")
	wramValue := flags.String("wram", "", "WRAM offset or hexadecimal low-high range")
	around := flags.Int64("around", -1, "select events around this sequence number")
	window := flags.Uint64("window", 15, "sequence distance for --around")
	limit := flags.Int("limit", 200, "maximum selected events")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if tracePath == "" && flags.NArg() == 1 {
		tracePath = flags.Arg(0)
	} else if flags.NArg() != 0 {
		return errors.New("trace-inspect needs exactly one trace path")
	}
	if strings.TrimSpace(tracePath) == "" {
		return errors.New("trace-inspect needs a runtime JSONL trace path")
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	options := tooling.TraceInspectOptions{
		TracePath:    resolveProjectOptional(absoluteRoot, tracePath),
		MetadataPath: resolveProjectOptional(absoluteRoot, *metadataPath), ROMPath: resolveProjectOptional(absoluteRoot, *romPath),
		Summary: *summary, Diagnose: *diagnose, Function: *functionValue,
		Misdecodes: *misdecodes, Leaks: *leaks, VMADD: *vmadd,
		Window: *window, Limit: *limit,
	}
	if strings.TrimSpace(*channelValue) != "" {
		options.Channels = strings.Split(*channelValue, ",")
	}
	if strings.TrimSpace(*vramValue) != "" {
		value, err := tooling.ParseTraceRange(*vramValue)
		if err != nil {
			return err
		}
		options.VRAM = &value
	}
	if strings.TrimSpace(*wramValue) != "" {
		value, err := tooling.ParseTraceRange(*wramValue)
		if err != nil {
			return err
		}
		options.WRAM = &value
	}
	if *around >= 0 {
		value := uint64(*around)
		options.Around = &value
	}
	report, err := tooling.BuildTraceInspection(options)
	if err != nil {
		return err
	}
	return tooling.WriteTraceInspection(os.Stdout, report, *format)
}

func runTraceDiff(args []string) error {
	if len(args) < 3 || strings.HasPrefix(args[0], "-") || strings.HasPrefix(args[1], "-") || strings.HasPrefix(args[2], "-") {
		return errors.New("trace-diff needs MODE ORACLE_JSONL RECOMP_JSONL before its options")
	}
	mode, oraclePath, recompPath := args[0], args[1], args[2]
	flags := flag.NewFlagSet("trace-diff", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	top := flags.Int("top", 40, "maximum divergence rows")
	skipZeroPage := flags.Bool("skip-zp", false, "ignore WRAM offsets $0000-$01FF")
	low := flags.Uint64("lo", 0, "lowest WRAM offset to compare")
	high := flags.Uint64("hi", 0x1ffff, "highest WRAM offset to compare")
	minPrefix := flags.Int("min-prefix", 0, "sequence mode: minimum matching prefix before reporting")
	fromGameFrame := flags.Uint64("from-gf", 0, "aligned mode: first game frame")
	toGameFrame := flags.Uint64("to-gf", ^uint64(0), "aligned mode: last game frame")
	clockLow := flags.Uint64("clock-low", 0x88, "aligned mode: low byte of game-frame clock")
	clockHigh := flags.Uint64("clock-high", 0x89, "aligned mode: high byte of game-frame clock")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args[3:]); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("trace-diff options must follow MODE ORACLE_JSONL RECOMP_JSONL")
	}
	if *low > *high || *high > 0x1ffff {
		return fmt.Errorf("trace-diff WRAM range %#x-%#x is outside 0-0x1ffff", *low, *high)
	}
	if *clockLow > 0x1ffff || *clockHigh > 0x1ffff {
		return errors.New("trace-diff clock offsets must be within 0-0x1ffff")
	}
	absoluteRoot, err := filepath.Abs(*root)
	if err != nil {
		return fmt.Errorf("resolve project root: %w", err)
	}
	report, err := tooling.BuildTraceDiff(tooling.TraceDiffOptions{
		Mode:       mode,
		OraclePath: resolveProjectOptional(absoluteRoot, oraclePath), RecompPath: resolveProjectOptional(absoluteRoot, recompPath),
		Top: *top, SkipZeroPage: *skipZeroPage, Low: uint32(*low), High: uint32(*high), MinPrefix: *minPrefix,
		FromGameFrame: *fromGameFrame, ToGameFrame: *toGameFrame,
		ClockLow: uint32(*clockLow), ClockHigh: uint32(*clockHigh),
	})
	if err != nil {
		return err
	}
	return tooling.WriteTraceDiff(os.Stdout, report, *format)
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
		pinnedVersion, err := toolchain.PinnedVersion()
		if err != nil {
			return err
		}
		fmt.Printf("pinned Zig      %s\n", pinnedVersion)
		fmt.Printf("release         %s\n", url)
		fmt.Printf("sha256          %s\n", sha)
		located, err := toolchain.Locate(cache)
		if err != nil {
			fmt.Printf("local zig       MISSING\n")
			return err
		}
		fmt.Printf("local zig       %s (%s, via %s)\n", located.Version, located.Path, located.Source)
		if located.Version != pinnedVersion {
			fmt.Printf("note            local version differs from the pin; hermetic release builds should use %s\n", pinnedVersion)
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
