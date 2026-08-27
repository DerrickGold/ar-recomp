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
	case "sync-funcs":
		return syncFuncs(args[1:])
	case "metadata":
		return generateMetadata(args[1:])
	case "rts-webs":
		return censusRTSWebs(args[1:])
	case "stub-census":
		return censusStubs(args[1:])
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
  sync-funcs         Regenerate recomp/funcs.h from bank cfg declarations
  metadata           Refresh the generated-code metadata sidecar
  rts-webs           Census pushed-continuation RTS dispatch patterns
  stub-census        Report unresolved control-flow traps in generated C
  inspect            Parse ROM/cfg inputs and show concurrent shard balance
  emit-function      Emit one function with the standalone Go pipeline
  opcode-diff        Differential-test opcode semantics with Harte vectors
  link-audit         Audit generated call-graph reachability and live traps
  baseline capture   Save a deterministic generated-output snapshot
  baseline verify    Compare a generated directory with a saved snapshot

These commands replace every tool in the normal tools/regen.sh pipeline.`)
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
	if err := flags.Parse(args); err != nil {
		return err
	}
	return tooling.RunLinkAudit(tooling.LinkAuditOptions{
		GenDir: *genDir, SourceDir: *sourceDir, RuntimeDir: *runtimeDir,
		ListOrphans: *orphans, Verbose: *verbose, Output: os.Stdout,
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
	_, err := tooling.CensusRTSWebs(tooling.RTSCensusOptions{ROMPath: *romPath, CFGDir: *cfgDir, Bank: bank, Suggest: *suggest, Output: os.Stdout})
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
	if report.LogicalGotos+report.LogicalDispatches > 0 {
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
	report, err := regen.Run(regen.Options{
		ROMPath: *romPath, ConfigDir: *cfgDir, OutputDir: *outDir, Jobs: *jobs,
		ChunkThresholdBytes: max(0, *chunkThreshold) * 1024, ChunkPCSpan: max(0, *chunkSpan), OnlyBanks: only,
		AllowStubs: *allowStubs,
		Progress:   func(format string, values ...any) { fmt.Printf("v2regen: "+format+"\n", values...) },
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
