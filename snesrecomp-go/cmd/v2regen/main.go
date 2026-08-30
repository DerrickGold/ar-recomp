package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/artifact"
	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
	"github.com/DerrickGold/snesrecomp-go/internal/regen"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
	"github.com/DerrickGold/snesrecomp-go/internal/work"
)

var bankConfigName = regexp.MustCompile(`(?i)^bank([0-9a-f]+)\.cfg$`)

type baselineMetadata struct {
	Version     int               `json:"version"`
	CapturedAt  string            `json:"captured_at"`
	Command     string            `json:"command"`
	WallSeconds float64           `json:"wall_seconds"`
	ExitCode    int               `json:"exit_code"`
	Jobs        int               `json:"jobs"`
	Status      string            `json:"status"`
	Note        string            `json:"note,omitempty"`
	Manifest    artifact.Manifest `json:"manifest"`
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintf(os.Stderr, "v2regen: %v\n", err)
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
		return regenerate(args[1:])
	case "analyze":
		return analyze(args[1:])
	case "xref":
		return crossReference(args[1:])
	case "disasm":
		return disassemble(args[1:])
	case "rom-info":
		return inspectROM(args[1:])
	case "spc-disasm":
		return disassembleSPC(args[1:])
	case "apu-audit":
		return auditAPU(args[1:])
	case "quintet-lzss":
		return decompressQuintetLZSS(args[1:])
	case "poll-census":
		return censusPolls(args[1:])
	case "sync-funcs":
		return syncFuncs(args[1:])
	case "metadata":
		return generateMetadata(args[1:])
	case "rts-webs":
		return censusRTSWebs(args[1:])
	case "stub-census":
		return censusStubs(args[1:])
	case "dispatch-census":
		return censusDispatch(args[1:])
	case "trace-inspect":
		return inspectTrace(args[1:])
	case "trace-diff":
		return diffTrace(args[1:])
	case "wram":
		return tooling.RunWRAMCommand(args[1:], ".", os.Stdout)
	case "mx-diff":
		return tooling.RunMXDiffCommand(args[1:], ".", os.Stdout)
	case "chr-render":
		return tooling.RunCHRRenderCommand(args[1:], ".", os.Stdout)
	case "inspect":
		return inspect(args[1:])
	case "emit-function":
		return emitFunction(args[1:])
	case "opcode-diff":
		return opcodeDiff(args[1:])
	case "link-audit":
		return linkAudit(args[1:])
	case "baseline":
		return baseline(args[1:])
	case "help", "-h", "--help":
		usage()
		return nil
	default:
		usage()
		return fmt.Errorf("unknown command %q", args[0])
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, `Usage: v2regen <command> [options]

Commands:
  regen              Regenerate all C banks with the concurrent Go pipeline
  analyze            Compare independent static facts with authored cfg (no-write)
  xref               Find decoded instruction references to an address (no-write)
  disasm             Disassemble ROM code with live M/X tracking (no-write)
  rom-info           Report cartridge identity, header, and vectors (no-write)
  spc-disasm         Disassemble an SPC700 payload or ROM upload block (no-write)
  apu-audit          Validate live BRR samples and CPU/APU port handshakes
  quintet-lzss       Decode a bit-packed Quintet LZSS blob
  poll-census        Classify decoded hardware-status read and polling sites
  sync-funcs         Regenerate recomp/funcs.h from bank cfg declarations
  metadata           Refresh the generated-code metadata sidecar
  rts-webs           Census pushed-continuation RTS dispatch patterns
  stub-census        Report unresolved control-flow traps in generated C
  dispatch-census    Summarize runtime dynamic targets and missing entries
  trace-inspect      Slice, summarize, and diagnose unified runtime traces
  trace-diff         Compare reference and recompiled WRAM traces (no-write)
  wram               Inspect, compare, and scan WRAM snapshots (no-write)
  mx-diff            Compare game-frame M/X traces (no-write)
  chr-render         Render SNES 4bpp ROM, VRAM, and icon sheets
  inspect            Parse ROM/cfg inputs and show concurrent shard balance
  emit-function      Emit one function with the standalone Go pipeline
  opcode-diff        Differential-test opcode semantics with Harte vectors
  link-audit         Audit generated call-graph reachability and live traps
  baseline capture   Save a deterministic generated-output snapshot
  baseline verify    Compare a generated directory with a saved snapshot

These commands replace every tool in the normal tools/regen.sh pipeline.`)
}

func auditAPU(args []string) error {
	flags := flag.NewFlagSet("apu-audit", flag.ContinueOnError)
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
		Prefix: *prefix, SPCPath: *spcPath, ARAMPath: *aramPath, DSPPath: *dspPath,
		WrittenPath: *writtenPath, TracePath: *tracePath, DirectoryPage: directoryPage,
		Sources: sources,
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

func disassemble(args []string) error {
	address := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		address, args = args[0], args[1:]
	}
	flags := flag.NewFlagSet("disasm", flag.ContinueOnError)
	romPath := flags.String("rom", "game.sfc", "headered or headerless LoROM image")
	metadataPath := flags.String("metadata", "saves/gen_meta.json", "optional generated metadata sidecar")
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
	report, err := tooling.BuildDisassembly(tooling.DisassemblyOptions{
		ROMPath: *romPath, MetadataPath: *metadataPath, StartPC: startPC,
		EntryM: m, EntryX: x, Count: *count, UntilFlow: *untilFlow,
	})
	if err != nil {
		return err
	}
	return tooling.WriteDisassemblyReport(os.Stdout, report, *format, *raw)
}

func inspectROM(args []string) error {
	flags := flag.NewFlagSet("rom-info", flag.ContinueOnError)
	romPath := flags.String("rom", "game.sfc", "headered or headerless SNES ROM image")
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
	report, err := tooling.BuildROMInfo(tooling.ROMInfoOptions{ROMPath: *romPath})
	if err != nil {
		return err
	}
	return tooling.WriteROMInfo(os.Stdout, report, *format)
}

func disassembleSPC(args []string) error {
	if len(args) < 2 || strings.HasPrefix(args[0], "-") || strings.HasPrefix(args[1], "-") {
		return errors.New("spc-disasm needs start and end ARAM addresses before its options")
	}
	startText, endText := args[0], args[1]
	args = args[2:]
	flags := flag.NewFlagSet("spc-disasm", flag.ContinueOnError)
	inputPath := flags.String("input", "game.sfc", "ROM or raw SPC700 payload")
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
	options := tooling.SPCDisassemblyOptions{
		InputPath: *inputPath, FileOffset: fileOffset, LoadAddress: loadAddress,
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

func decompressQuintetLZSS(args []string) error {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return errors.New("quintet-lzss needs a linear input offset before its options")
	}
	offsetText, args := args[0], args[1:]
	flags := flag.NewFlagSet("quintet-lzss", flag.ContinueOnError)
	inputPath := flags.String("input", "game.sfc", "ROM or compressed input file")
	size := flags.Int("size", 0, "exact decompressed size (default: little-endian word at offset)")
	outputPath := flags.String("out", "", "optional decompressed output path")
	comparePath := flags.String("compare", "", "optional expected binary to compare")
	compareOffset := flags.Int("compare-offset", 0, "byte offset within --compare")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args); err != nil {
		return err
	}
	offset, err := strconv.ParseInt(strings.TrimSpace(offsetText), 0, 64)
	if err != nil || offset < 0 || int64(int(offset)) != offset {
		return fmt.Errorf("parse input offset %q as a non-negative integer", offsetText)
	}
	headered := true
	flags.Visit(func(item *flag.Flag) {
		if item.Name == "size" {
			headered = false
		}
	})
	report, output, err := tooling.BuildQuintetLZSS(tooling.QuintetLZSSOptions{
		InputPath: *inputPath, Offset: int(offset), Size: *size, Headered: headered,
		ComparePath: *comparePath, CompareOffset: *compareOffset,
	})
	if err != nil {
		return err
	}
	if strings.TrimSpace(*outputPath) != "" {
		if err := os.WriteFile(*outputPath, output, 0o644); err != nil {
			return fmt.Errorf("write decompressed output %s: %w", *outputPath, err)
		}
		report.NoWrite = false
		fmt.Fprintf(os.Stderr, "quintet-lzss: wrote %d bytes to %s\n", len(output), *outputPath)
	}
	return tooling.WriteQuintetLZSSReport(os.Stdout, report, *format)
}

func censusPolls(args []string) error {
	flags := flag.NewFlagSet("poll-census", flag.ContinueOnError)
	romPath := flags.String("rom", "game.sfc", "headered or headerless LoROM image")
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
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
		ROMPath: *romPath, CFGDir: *cfgDir, Jobs: *jobs, OnlyBank: onlyBank, Registers: registers,
		DiscoverInterruptSync: *interruptSync,
	})
	if err != nil {
		return err
	}
	return tooling.WritePollCensus(os.Stdout, report, *format)
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

func crossReference(args []string) error {
	address := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		address, args = args[0], args[1:]
	}
	flags := flag.NewFlagSet("xref", flag.ContinueOnError)
	romPath := flags.String("rom", "game.sfc", "headered or headerless LoROM image")
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
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
		ROMPath: *romPath, CFGDir: *cfgDir, Jobs: *jobs, OnlyBank: onlyBank, Query: query,
		AccessFilter: *kind, IncludeWRAMMirrors: *wramMirrors,
		IncludeRawWords: *rawWords, IncludeTargetMinusOne: *targetMinusOne,
	})
	if err != nil {
		return err
	}
	return tooling.WriteXrefReport(os.Stdout, report, *format)
}

func censusDispatch(args []string) error {
	flags := flag.NewFlagSet("dispatch-census", flag.ContinueOnError)
	tracePath := flags.String("trace", "", "runtime JSONL captured with SNESRECOMP_TRACE_CHANNELS=dispatch")
	romPath := flags.String("rom", "", "optional ROM whose SHA-256 is stored with the evidence")
	format := flags.String("format", "text", "report format: text or json")
	suggest := flags.Bool("suggest", true, "include candidate cfg func lines for missing generated bodies")
	outPath := flags.String("out-analysis", "", "optional deterministic JSON evidence output (never edits cfg)")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if strings.TrimSpace(*tracePath) == "" {
		return errors.New("dispatch-census requires --trace <runtime.jsonl>")
	}
	report, err := tooling.LoadDispatchCensus(*tracePath, *romPath)
	if err != nil {
		return err
	}
	if err := tooling.WriteDispatchCensus(os.Stdout, report, *format, *suggest); err != nil {
		return err
	}
	if strings.TrimSpace(*outPath) != "" {
		if err := tooling.WriteDispatchCensusFile(*outPath, report); err != nil {
			return err
		}
		fmt.Fprintf(os.Stderr, "dispatch-census: wrote observed evidence to %s (authored cfg unchanged)\n", *outPath)
	}
	return nil
}

func inspectTrace(args []string) error {
	tracePath := ""
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		tracePath, args = args[0], args[1:]
	}
	flags := flag.NewFlagSet("trace-inspect", flag.ContinueOnError)
	summary := flags.Bool("summary", false, "show aggregate channel and diagnostic counts")
	diagnose := flags.Bool("diagnose", false, "rank M/X, garbage, and dispatch-miss findings")
	metadataPath := flags.String("metadata", "saves/gen_meta.json", "optional generated metadata sidecar")
	romPath := flags.String("rom", "", "optional ROM for paired-call continuation guards")
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
	options := tooling.TraceInspectOptions{
		TracePath: tracePath, MetadataPath: *metadataPath, ROMPath: *romPath,
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

func diffTrace(args []string) error {
	if len(args) < 3 || strings.HasPrefix(args[0], "-") || strings.HasPrefix(args[1], "-") || strings.HasPrefix(args[2], "-") {
		return errors.New("trace-diff needs MODE ORACLE_JSONL RECOMP_JSONL before its options")
	}
	mode, oraclePath, recompPath := args[0], args[1], args[2]
	flags := flag.NewFlagSet("trace-diff", flag.ContinueOnError)
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
	report, err := tooling.BuildTraceDiff(tooling.TraceDiffOptions{
		Mode: mode, OraclePath: oraclePath, RecompPath: recompPath,
		Top: *top, SkipZeroPage: *skipZeroPage, Low: uint32(*low), High: uint32(*high), MinPrefix: *minPrefix,
		FromGameFrame: *fromGameFrame, ToGameFrame: *toGameFrame,
		ClockLow: uint32(*clockLow), ClockHigh: uint32(*clockHigh),
	})
	if err != nil {
		return err
	}
	return tooling.WriteTraceDiff(os.Stdout, report, *format)
}

func analyze(args []string) error {
	flags := flag.NewFlagSet("analyze", flag.ContinueOnError)
	romPath := flags.String("rom", "game.sfc", "headered or headerless LoROM image")
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
	jobs := flags.Int("jobs", runtime.NumCPU(), "parallel decode workers")
	bankValue := flags.String("bank", "", "optional hexadecimal bank")
	format := flags.String("format", "text", "report format: text or json")
	verbose := flags.Bool("verbose", false, "show all comparisons, callers, and decode issues")
	flags.BoolVar(verbose, "v", false, "show all comparisons, callers, and decode issues")
	strict := flags.Bool("strict", false, "fail after reporting independently proven semantic conflicts")
	dispatchAnalysis := flags.String("dispatch-analysis", "", "optional dispatch-census JSON evidence used to rank unresolved sites")
	compareAuthored := flags.Bool("compare-authored", true, "compare inferred facts with authored cfg declarations")
	noWrite := flags.Bool("no-write", true, "require analysis to remain read-only")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if !*compareAuthored {
		return errors.New("analyze currently requires --compare-authored")
	}
	if !*noWrite {
		return errors.New("analyze is intentionally read-only; --no-write=false is not supported")
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
		ROMPath: *romPath, CFGDir: *cfgDir, Jobs: *jobs, OnlyBank: onlyBank,
		DispatchAnalysisPath: *dispatchAnalysis,
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

func opcodeDiff(args []string) error {
	flags := flag.NewFlagSet("opcode-diff", flag.ContinueOnError)
	cacheDir := flags.String("cache-dir", "tools/oracle/harte_cache", "directory containing Tom Harte JSON vectors")
	runtimeDir := flags.String("runtime-dir", "snesrecomp-go/runtime/src", "bundled runtime header directory")
	workDir := flags.String("work-dir", "build/opcode_diff", "temporary harness build directory")
	opcodeValues := flags.String("opcodes", "", "comma-separated hexadecimal opcodes")
	all := flags.Bool("all", false, "test every supported non-control-flow opcode")
	count := flags.Int("count", 64, "vectors per opcode")
	mode := flags.String("mode", "native", "vector mode: native or emu")
	maxShow := flags.Int("max-show", 12, "maximum failing examples to print")
	keep := flags.Bool("keep", false, "keep generated C and harness binary")
	if err := flags.Parse(args); err != nil {
		return err
	}
	var opcodes []byte
	if strings.TrimSpace(*opcodeValues) != "" {
		for _, token := range strings.Split(*opcodeValues, ",") {
			value, err := strconv.ParseUint(strings.TrimSpace(token), 16, 8)
			if err != nil {
				return fmt.Errorf("parse --opcodes %q: %w", token, err)
			}
			opcodes = append(opcodes, byte(value))
		}
	}
	return tooling.RunOpcodeDiff(tooling.OpcodeDiffOptions{
		CacheDir: *cacheDir, RuntimeSourceDir: *runtimeDir, WorkDir: *workDir,
		Opcodes: opcodes, All: *all, Count: *count, Mode: *mode, MaxShow: *maxShow, Keep: *keep,
	})
}

func linkAudit(args []string) error {
	flags := flag.NewFlagSet("link-audit", flag.ContinueOnError)
	genDir := flags.String("gen-dir", "src/gen", "generated C directory")
	sourceDir := flags.String("src-dir", "src", "hand-written game source directory")
	runtimeDir := flags.String("runtime-dir", "snesrecomp-go/runtime/src", "bundled runtime source directory")
	orphans := flags.Bool("orphans", false, "list every orphan function")
	verbose := flags.Bool("verbose", false, "show partial M/X variant coverage")
	flags.BoolVar(verbose, "v", false, "show partial M/X variant coverage")
	tailCalls := flags.Bool("tailcalls", false, "rank repeated tail-call-past-end source/target pairs")
	tailCallMinimum := flags.Int("tailcall-min", 2, "minimum repeated source/target count to list")
	if err := flags.Parse(args); err != nil {
		return err
	}
	return tooling.RunLinkAudit(tooling.LinkAuditOptions{
		GenDir: *genDir, SourceDir: *sourceDir, RuntimeDir: *runtimeDir,
		ListOrphans: *orphans, Verbose: *verbose, ListTailCalls: *tailCalls,
		TailCallMinimum: *tailCallMinimum, Output: os.Stdout,
	})
}

func syncFuncs(args []string) error {
	flags := flag.NewFlagSet("sync-funcs", flag.ContinueOnError)
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
	output := flags.String("out", "recomp/funcs.h", "generated header path")
	if err := flags.Parse(args); err != nil {
		return err
	}
	count, err := tooling.SyncFuncs(*cfgDir, *output)
	if err != nil {
		return err
	}
	fmt.Printf("sync-funcs: wrote %d function declarations to %s\n", count, *output)
	return nil
}

func generateMetadata(args []string) error {
	flags := flag.NewFlagSet("metadata", flag.ContinueOnError)
	genDir := flags.String("gen-dir", "src/gen", "generated C directory")
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
	output := flags.String("out", "saves/gen_meta.json", "metadata sidecar path")
	if err := flags.Parse(args); err != nil {
		return err
	}
	started := time.Now()
	report, err := tooling.GenerateMetadata(*genDir, *cfgDir, *output, started)
	if err != nil {
		return err
	}
	fmt.Println(tooling.FormatMetadataReport(report, *output, time.Since(started)))
	return nil
}

func censusRTSWebs(args []string) error {
	flags := flag.NewFlagSet("rts-webs", flag.ContinueOnError)
	romPath := flags.String("rom", "game.sfc", "headered or headerless LoROM image")
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
	bankValue := flags.String("bank", "", "optional hexadecimal bank")
	suggest := flags.Bool("suggest", false, "print candidate cfg declarations for uncovered pushes")
	yieldHelpers := flags.Bool("yield-helpers", false, "also detect JSR helpers that capture return PCs into small indexed fields")
	yieldFieldMax := flags.Uint("yield-field-max", 0x40, "exclusive maximum indexed field offset for --yield-helpers")
	if err := flags.Parse(args); err != nil {
		return err
	}
	var bank *byte
	if strings.TrimSpace(*bankValue) != "" {
		value, err := strconv.ParseUint(strings.TrimPrefix(*bankValue, "0x"), 16, 8)
		if err != nil {
			return fmt.Errorf("parse --bank: %w", err)
		}
		parsed := byte(value)
		bank = &parsed
	}
	if *yieldFieldMax == 0 || *yieldFieldMax > 0x100 {
		return errors.New("--yield-field-max must be within 1-0x100")
	}
	_, err := tooling.CensusRTSWebs(tooling.RTSCensusOptions{
		ROMPath: *romPath, CFGDir: *cfgDir, Bank: bank, Suggest: *suggest,
		YieldHelpers: *yieldHelpers, YieldFieldMax: uint16(*yieldFieldMax), Output: os.Stdout,
	})
	return err
}

func censusStubs(args []string) error {
	flags := flag.NewFlagSet("stub-census", flag.ContinueOnError)
	genDir := flags.String("gen-dir", "src/gen", "generated C directory")
	verbose := flags.Bool("verbose", false, "list every variant occurrence")
	flags.BoolVar(verbose, "v", false, "list every variant occurrence")
	if err := flags.Parse(args); err != nil {
		return err
	}
	report, err := tooling.CensusStubs(*genDir, *verbose, os.Stdout)
	if err != nil {
		return err
	}
	if report.LogicalTotal() > 0 {
		return errors.New("stub census failed")
	}
	return nil
}

func regenerate(args []string) error {
	flags := flag.NewFlagSet("regen", flag.ContinueOnError)
	romPath := flags.String("rom", "game.sfc", "headered or headerless LoROM image")
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
	outDir := flags.String("out-dir", "src/gen", "generated C output directory")
	jobs := flags.Int("jobs", runtime.NumCPU(), "parallel function workers")
	banksValue := flags.String("banks", "", "optional comma-separated hexadecimal banks")
	chunkThreshold := flags.Int("bank-chunk-threshold-kib", 4096, "split banks at or above this generated size")
	chunkSpan := flags.Int("bank-chunk-pc-span", 0x800, "stable PC span per split translation unit")
	allowStubs := flags.Bool("allow-stubs", false, "write complete output and report stubs without failing this command")
	provenAnalysis := flags.Bool("experimental-proven-analysis", false, "apply closed static dispatch facts and exact direct-call M/X in memory (requires an isolated --out-dir)")
	_ = flags.String("prefix", "", "deprecated compatibility option")
	if err := flags.Parse(args); err != nil {
		return err
	}
	var only map[byte]struct{}
	if strings.TrimSpace(*banksValue) != "" {
		only = make(map[byte]struct{})
		for _, token := range strings.Split(*banksValue, ",") {
			value, err := strconv.ParseUint(strings.TrimSpace(token), 16, 8)
			if err != nil {
				return fmt.Errorf("parse --banks %q: %w", token, err)
			}
			only[byte(value)] = struct{}{}
		}
	}
	var provenFacts []analysis.DispatchFact
	var provenEntryFacts []analysis.EntryFact
	if *provenAnalysis {
		requestedOutput, pathErr := filepath.Abs(*outDir)
		if pathErr != nil {
			return fmt.Errorf("resolve --out-dir: %w", pathErr)
		}
		if filepath.Base(requestedOutput) == "gen" && filepath.Base(filepath.Dir(requestedOutput)) == "src" {
			return errors.New("--experimental-proven-analysis requires an isolated --out-dir; refusing to replace src/gen")
		}
		shadow, analyzeErr := tooling.AnalyzeAuthoredShadow(tooling.ShadowAnalysisOptions{
			ROMPath: *romPath, CFGDir: *cfgDir, Jobs: *jobs,
		})
		if analyzeErr != nil {
			return fmt.Errorf("experimental proven analysis: %w", analyzeErr)
		}
		if shadow.Summary.Conflicts > 0 {
			return fmt.Errorf("experimental proven analysis found %d authored conflict(s):\n%s\nrun `v2regen analyze --rom <rom> --cfg-dir <cfg> --verbose` for the complete comparison", shadow.Summary.Conflicts, tooling.FormatShadowConflicts(shadow))
		}
		var rejected int
		provenFacts, rejected = tooling.SelectStaticProvenAutomaticDispatchFacts(shadow)
		fmt.Printf("v2regen: experimental analysis selected %d proven automatic fact(s); kept %d open/probable fact(s) report-only\n", len(provenFacts), rejected)
		provenEntryFacts = tooling.SelectStaticProvenRoutineEntryFacts(shadow)
		fmt.Printf("v2regen: experimental analysis selected %d statically derivable routine root(s); continuations and other entry kinds remain report-only\n", len(provenEntryFacts))
	}
	report, err := regen.Run(regen.Options{
		ROMPath: *romPath, ConfigDir: *cfgDir, OutputDir: *outDir, Jobs: *jobs,
		ChunkThresholdBytes: max(0, *chunkThreshold) * 1024, ChunkPCSpan: max(0, *chunkSpan), OnlyBanks: only,
		AllowStubs:                    *allowStubs,
		ProvenDispatchFacts:           provenFacts,
		ProvenEntryFacts:              provenEntryFacts,
		ExperimentalExactDirectCallMX: *provenAnalysis,
		Progress:                      func(format string, values ...any) { fmt.Printf("v2regen: "+format+"\n", values...) },
	})
	fmt.Printf("v2regen: %d banks, %d -> %d variants, %d files (%d changed), %s\n", report.Banks, report.InitialEntries, report.FinalEntries, report.Files, report.ChangedFiles, report.Elapsed.Round(time.Millisecond))
	if report.UnresolvedIndirects > 0 {
		fmt.Printf("v2regen: %d unresolved indirect sites emitted as traps\n", report.UnresolvedIndirects)
	}
	if report.StubHits > 0 {
		fmt.Printf("v2regen: STUB LINT: %d marker(s)\n", report.StubHits)
	}
	return err
}

func emitFunction(args []string) error {
	flags := flag.NewFlagSet("emit-function", flag.ContinueOnError)
	romPath := flags.String("rom", "game.sfc", "headered or headerless LoROM image")
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
	bankValue := flags.String("bank", "00", "hexadecimal ROM bank")
	startValue := flags.String("start", "8000", "hexadecimal function entry PC")
	entryM := flags.Int("m", 1, "entry accumulator-width flag")
	entryX := flags.Int("x", 1, "entry index-width flag")
	name := flags.String("name", "", "optional C base name")
	allowUnresolved := flags.Bool("allow-unresolved", false, "emit runtime traps for unresolved indirect sites")
	if err := flags.Parse(args); err != nil {
		return err
	}
	bank64, err := strconv.ParseUint(strings.TrimPrefix(*bankValue, "0x"), 16, 8)
	if err != nil {
		return fmt.Errorf("parse --bank: %w", err)
	}
	start64, err := strconv.ParseUint(strings.TrimPrefix(*startValue, "0x"), 16, 16)
	if err != nil {
		return fmt.Errorf("parse --start: %w", err)
	}
	image, err := romimage.Load(*romPath)
	if err != nil {
		return err
	}
	bank := byte(bank64)
	cfgPath := filepath.Join(*cfgDir, fmt.Sprintf("bank%02X.cfg", bank))
	bankConfig, err := config.Load(cfgPath)
	if err != nil {
		return err
	}
	context := codegen.NewContext()
	context.ROMSize = len(image)
	for _, entry := range bankConfig.Entries {
		if entry.Name != "" {
			context.Names[uint32(bank)<<16|uint32(entry.Start)] = entry.Name
		}
	}
	decodeOptions := emitter.DecodeOptionsFromConfig(bank, bankConfig)
	var exitMX *decoder.MX
	entryAddress := uint32(bank)<<16 | uint32(uint16(start64))
	for _, declaration := range bankConfig.ExitMXAt {
		if declaration.Address&0xffffff == entryAddress {
			value := decoder.MX{M: int8(declaration.Exit.M), X: int8(declaration.Exit.X)}
			exitMX = &value
			break
		}
	}
	result, err := emitter.EmitFunction(image, bank, uint16(start64), uint8(*entryM), uint8(*entryX), emitter.FunctionOptions{
		Name:              *name,
		Decode:            decodeOptions,
		Codegen:           context,
		HLEFunction:       bankConfig.HLEFunctions[uint16(start64)],
		HLEDispatch:       bankConfig.HLEDispatch,
		ExitMX:            exitMX,
		UnresolvedAllowed: *allowUnresolved,
	})
	if err != nil {
		return err
	}
	fmt.Print(result.Source)
	return nil
}

func inspect(args []string) error {
	flags := flag.NewFlagSet("inspect", flag.ContinueOnError)
	cfgDir := flags.String("cfg-dir", "recomp", "directory containing bankXX.cfg")
	romPath := flags.String("rom", "", "optional ROM to validate/load")
	jobs := flags.Int("jobs", runtime.NumCPU(), "planned worker count")
	chunkSize := flags.Int("chunk-size", 32, "entries per stable emit chunk")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *romPath != "" {
		image, err := romimage.Load(*romPath)
		if err != nil {
			return err
		}
		fmt.Printf("ROM: %s (%d bytes, copier header removed if present)\n", *romPath, len(image))
	}

	paths, err := filepath.Glob(filepath.Join(*cfgDir, "bank*.cfg"))
	if err != nil {
		return fmt.Errorf("find cfg files: %w", err)
	}
	sort.Strings(paths)
	if len(paths) == 0 {
		return fmt.Errorf("no bank*.cfg under %s", *cfgDir)
	}
	var banks []work.Bank
	totalEntries := 0
	for _, cfgPath := range paths {
		match := bankConfigName.FindStringSubmatch(filepath.Base(cfgPath))
		if match == nil {
			continue
		}
		filenameBank, err := strconv.ParseUint(match[1], 16, 8)
		if err != nil {
			return fmt.Errorf("parse bank from %s: %w", cfgPath, err)
		}
		cfg, err := config.Load(cfgPath)
		if err != nil {
			return err
		}
		if cfg.Bank != byte(filenameBank) {
			fmt.Printf("WARN: %s declares bank $%02X; filename selects $%02X\n", filepath.Base(cfgPath), cfg.Bank, filenameBank)
		}
		banks = append(banks, work.Bank{ID: byte(filenameBank), Entries: cfg.Entries})
		totalEntries += len(cfg.Entries)
		fmt.Printf("bank $%02X: %4d initial entries\n", filenameBank, len(cfg.Entries))
	}

	workers := work.Shard(banks, *jobs, *chunkSize)
	maxWeight := 0
	for _, worker := range workers {
		maxWeight = max(maxWeight, worker.Weight)
	}
	fmt.Printf("\n%d cfg banks, %d initial entries\n", len(banks), totalEntries)
	fmt.Printf("per-function plan: %d workers, chunks <= %d entries\n", len(workers), *chunkSize)
	for _, worker := range workers {
		fmt.Printf("  worker %2d: %4d entries in %d chunks\n", worker.ID, worker.Weight, len(worker.Chunks))
	}
	currentLargest := 0
	for _, bank := range banks {
		currentLargest = max(currentLargest, len(bank.Entries))
	}
	fmt.Printf("current bank-granularity critical load: %d entries\n", currentLargest)
	fmt.Printf("planned function-chunk critical load: %d entries\n", maxWeight)
	if maxWeight > 0 {
		fmt.Printf("entry-count balance improvement: %.2fx (cost per entry is not uniform)\n", float64(currentLargest)/float64(maxWeight))
	}
	return nil
}

func baseline(args []string) error {
	if len(args) == 0 {
		return errors.New("baseline needs capture or verify")
	}
	switch args[0] {
	case "capture":
		return captureBaseline(args[1:])
	case "verify":
		return verifyBaseline(args[1:])
	default:
		return fmt.Errorf("unknown baseline command %q", args[0])
	}
}

func captureBaseline(args []string) error {
	flags := flag.NewFlagSet("baseline capture", flag.ContinueOnError)
	source := flags.String("source", "src/gen", "generated artifact directory")
	archivePath := flags.String("archive", "build/baseline/generated-src.tar.gz", "output archive")
	metadataPath := flags.String("metadata", "build/baseline/generated-src.json", "output metadata/manifest")
	command := flags.String("command", "", "baseline command")
	wallSeconds := flags.Float64("wall-seconds", 0, "measured wall-clock seconds")
	exitCode := flags.Int("exit-code", 0, "baseline process exit code")
	jobs := flags.Int("jobs", 1, "baseline worker count")
	note := flags.String("note", "", "baseline notes")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(*archivePath), 0o755); err != nil {
		return fmt.Errorf("create archive directory: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(*metadataPath), 0o755); err != nil {
		return fmt.Errorf("create metadata directory: %w", err)
	}
	manifest, err := artifact.Capture(*source, *archivePath)
	if err != nil {
		return err
	}
	status := "success"
	if *exitCode != 0 {
		status = "failed-after-emit"
	}
	metadata := baselineMetadata{
		Version:     1,
		CapturedAt:  time.Now().UTC().Format(time.RFC3339),
		Command:     *command,
		WallSeconds: *wallSeconds,
		ExitCode:    *exitCode,
		Jobs:        *jobs,
		Status:      status,
		Note:        *note,
		Manifest:    manifest,
	}
	encoded, err := json.MarshalIndent(metadata, "", "  ")
	if err != nil {
		return fmt.Errorf("encode baseline metadata: %w", err)
	}
	encoded = append(encoded, '\n')
	if err := os.WriteFile(*metadataPath, encoded, 0o644); err != nil {
		return fmt.Errorf("write baseline metadata: %w", err)
	}
	fmt.Printf("captured %d files in %s\n", len(manifest.Files), *archivePath)
	fmt.Printf("wrote hashes and run metadata to %s\n", *metadataPath)
	return nil
}

func verifyBaseline(args []string) error {
	flags := flag.NewFlagSet("baseline verify", flag.ContinueOnError)
	archivePath := flags.String("archive", "build/baseline/generated-src.tar.gz", "baseline archive")
	actualDir := flags.String("actual", "src/gen", "generated directory to compare")
	if err := flags.Parse(args); err != nil {
		return err
	}
	expected, err := artifact.FromArchive(*archivePath)
	if err != nil {
		return err
	}
	actual, err := artifact.FromDir(*actualDir)
	if err != nil {
		return err
	}
	differences := artifact.Compare(expected, actual)
	if len(differences) == 0 {
		fmt.Printf("baseline parity: %d/%d files byte-identical\n", len(actual.Files), len(expected.Files))
		return nil
	}
	for _, difference := range differences {
		switch {
		case difference.Expected == nil:
			fmt.Printf("EXTRA   %s\n", difference.Path)
		case difference.Actual == nil:
			fmt.Printf("MISSING %s\n", difference.Path)
		default:
			fmt.Printf("CHANGED %s expected=%s actual=%s\n", difference.Path, difference.Expected.SHA256, difference.Actual.SHA256)
		}
	}
	return fmt.Errorf("baseline parity failed: %d differing files", len(differences))
}
