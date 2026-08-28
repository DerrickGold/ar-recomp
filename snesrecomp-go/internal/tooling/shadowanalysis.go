package tooling

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const shadowReportVersion = 2

type ShadowAnalysisOptions struct {
	ROMPath  string
	CFGDir   string
	Jobs     int
	OnlyBank *byte
}

type ShadowROM struct {
	SHA256 string `json:"sha256"`
	Size   int    `json:"size"`
	Mapper string `json:"mapper"`
}

type ShadowSummary struct {
	analysis.ComparisonSummary
	InitialVariants        int `json:"initial_variants"`
	FinalVariants          int `json:"final_variants"`
	VariantPasses          int `json:"variant_passes"`
	RawUnresolvedEmissions int `json:"raw_unresolved_emissions"`
	UniqueUnresolvedSites  int `json:"unique_unresolved_sites"`
	DecodeIssues           int `json:"decode_issues"`
}

type ShadowCaller struct {
	FunctionEntry uint32           `json:"function_entry"`
	LiveMX        analysis.MXState `json:"live_mx"`
}

type ShadowUnresolvedSite struct {
	SitePC           uint32         `json:"site_pc"`
	InstructionBytes string         `json:"instruction_bytes"`
	Mnemonic         string         `json:"mnemonic"`
	AddressingMode   string         `json:"addressing_mode"`
	Operand          uint32         `json:"operand"`
	Reason           string         `json:"reason"`
	Reachability     string         `json:"reachability"`
	Callers          []ShadowCaller `json:"callers"`
}

type ShadowDecodeIssue struct {
	FunctionEntry uint32           `json:"function_entry"`
	EntryMX       analysis.MXState `json:"entry_mx"`
	Error         string           `json:"error"`
}

type ShadowReport struct {
	Version      int                    `json:"version"`
	Mode         string                 `json:"mode"`
	NoWrite      bool                   `json:"no_write"`
	ROM          ShadowROM              `json:"rom"`
	Summary      ShadowSummary          `json:"summary"`
	Comparisons  []analysis.Comparison  `json:"comparisons"`
	Unresolved   []ShadowUnresolvedSite `json:"unresolved_sites,omitempty"`
	DecodeIssues []ShadowDecodeIssue    `json:"decode_issues,omitempty"`
	Limitations  []string               `json:"limitations,omitempty"`
}

// SelectStaticProvenAutomaticDispatchFacts returns only independently inferred
// facts that are safe to feed into the experimental AOT regeneration path.
// Observations and probable/open tables remain report-only: production code
// generation must not turn a finite sample or a heuristic bound into policy.
func SelectStaticProvenAutomaticDispatchFacts(report ShadowReport) ([]analysis.DispatchFact, int) {
	var selected []analysis.DispatchFact
	rejected := 0
	for _, comparison := range report.Comparisons {
		if comparison.Status != analysis.ComparisonAutomatic || comparison.Inferred == nil {
			continue
		}
		fact := *comparison.Inferred
		if !fact.TargetSetClosed || len(fact.Targets) == 0 || len(fact.UnknownFields) != 0 || !hasOnlyStaticProof(fact.Evidence) {
			rejected++
			continue
		}
		fact.Normalize()
		selected = append(selected, fact)
	}
	sort.Slice(selected, func(i, j int) bool { return selected[i].SitePC < selected[j].SitePC })
	return selected, rejected
}

func hasOnlyStaticProof(evidence []analysis.Evidence) bool {
	if len(evidence) == 0 {
		return false
	}
	for _, item := range evidence {
		if item.Confidence != analysis.ConfidenceProven || !strings.HasPrefix(item.Source, "static.") {
			return false
		}
	}
	return true
}

type shadowBank struct {
	ID     byte
	Config *config.Config
}

type shadowDecodeResult struct {
	facts      []analysis.DispatchFact
	unresolved []decoder.UnresolvedIndirect
	demands    map[decoder.Variant]struct{}
	seedReach  []shadowContinuationReach
	spans      []shadowDecodedSpan
	issue      *ShadowDecodeIssue
}

type shadowDecodedSpan struct {
	PC            uint32
	FunctionEntry uint32
	Length        uint8
}

type shadowContinuationReach struct {
	SitePC     uint32
	Target     uint32
	BytesAbove int
	LiveMX     analysis.MXState
	EntryEdge  bool
}

type shadowInferenceStats struct {
	initialVariants int
	finalVariants   int
	passes          int
}

var shadowBankConfigRE = regexp.MustCompile(`(?i)^bank([0-9a-f]+)\.cfg$`)

// AnalyzeAuthoredShadow runs inference with authored dispatch declarations
// withheld, then compares the normalized results.  It has no output path and
// performs no filesystem writes.
func AnalyzeAuthoredShadow(options ShadowAnalysisOptions) (ShadowReport, error) {
	if options.Jobs <= 0 {
		options.Jobs = 1
	}
	image, err := romimage.Load(options.ROMPath)
	if err != nil {
		return ShadowReport{}, err
	}
	banks, err := loadShadowBanks(options.CFGDir, options.OnlyBank)
	if err != nil {
		return ShadowReport{}, err
	}
	authored, err := authoredDispatchFacts(image, banks)
	if err != nil {
		return ShadowReport{}, err
	}
	allRegions := collectShadowDataRegions(banks)
	calleeExitMX := collectShadowExitMX(banks)
	inferred, rawUnresolved, unresolved, issues, inferenceStats, err := inferShadowFacts(image, banks, allRegions, calleeExitMX, options.Jobs)
	if err != nil {
		return ShadowReport{}, err
	}
	comparisons, comparisonSummary := analysis.CompareDispatchFacts(authored, inferred)
	hash := sha256.Sum256(image)
	report := ShadowReport{
		Version: shadowReportVersion,
		Mode:    "compare_authored",
		NoWrite: true,
		ROM:     ShadowROM{SHA256: hex.EncodeToString(hash[:]), Size: len(image), Mapper: "lorom"},
		Summary: ShadowSummary{
			ComparisonSummary:      comparisonSummary,
			InitialVariants:        inferenceStats.initialVariants,
			FinalVariants:          inferenceStats.finalVariants,
			VariantPasses:          inferenceStats.passes,
			RawUnresolvedEmissions: rawUnresolved,
			UniqueUnresolvedSites:  len(unresolved),
			DecodeIssues:           len(issues),
		},
		Comparisons:  comparisons,
		Unresolved:   unresolved,
		DecodeIssues: issues,
		Limitations: []string{
			"configured func entries, entry M/X states, and exit_mx_at routes seed the read-only call-target variant fixed point",
			"an open table is a partial match until value/bounds provenance proves its complete target set",
			"a compatible guard proves safe coverage of inferred continuations, not that every guarded edge executes",
			"an authored-only result means unproven, not disproven or runtime-reachable",
			"the current ROM reader is explicitly LoROM; mapper generalization is a later milestone",
		},
	}
	return report, nil
}

func loadShadowBanks(cfgDir string, only *byte) ([]shadowBank, error) {
	paths, err := filepath.Glob(filepath.Join(cfgDir, "bank*.cfg"))
	if err != nil {
		return nil, err
	}
	sort.Strings(paths)
	if len(paths) == 0 {
		return nil, fmt.Errorf("no bank*.cfg under %s", cfgDir)
	}
	var banks []shadowBank
	for _, path := range paths {
		match := shadowBankConfigRE.FindStringSubmatch(filepath.Base(path))
		if match == nil {
			continue
		}
		value, parseErr := strconv.ParseUint(match[1], 16, 8)
		if parseErr != nil {
			return nil, fmt.Errorf("parse bank from %s: %w", path, parseErr)
		}
		bank := byte(value)
		if only != nil && bank != *only {
			continue
		}
		cfg, loadErr := config.Load(path)
		if loadErr != nil {
			return nil, loadErr
		}
		banks = append(banks, shadowBank{ID: bank, Config: cfg})
	}
	if only != nil && len(banks) == 0 {
		return nil, fmt.Errorf("no bank%02X.cfg under %s", *only, cfgDir)
	}
	return banks, nil
}

func collectShadowDataRegions(banks []shadowBank) []decoder.DataRegion {
	var result []decoder.DataRegion
	for _, bank := range banks {
		for _, region := range bank.Config.DataRegions {
			result = append(result, decoder.DataRegion{Bank: region.Bank, Start: region.Start, End: region.End})
		}
	}
	return result
}

func collectShadowExitMX(banks []shadowBank) map[decoder.Variant]decoder.MX {
	result := make(map[decoder.Variant]decoder.MX)
	for _, bank := range banks {
		for _, declaration := range bank.Config.ExitMXAt {
			for m := uint8(0); m < 2; m++ {
				for x := uint8(0); x < 2; x++ {
					result[decoder.Variant{Address: declaration.Address & 0xffffff, M: m, X: x}] = decoder.MX{M: int8(declaration.Exit.M & 1), X: int8(declaration.Exit.X & 1)}
				}
			}
		}
	}
	return result
}

func authoredDispatchFacts(image romimage.Image, banks []shadowBank) ([]analysis.DispatchFact, error) {
	var facts []analysis.DispatchFact
	for _, bank := range banks {
		for _, dispatch := range bank.Config.IndirectDispatch {
			instruction, err := decodeShadowInstruction(image, bank.ID, dispatch.SitePC, 0, 0)
			if err != nil {
				return nil, fmt.Errorf("authored indirect dispatch $%02X:%04X: %w", bank.ID, dispatch.SitePC, err)
			}
			auth := decoder.DispatchAuth{
				Count: dispatch.Count, IndexReg: dispatch.IndexReg,
				TableBases: append([]uint16(nil), dispatch.TableBases...),
				ReturnPC:   dispatch.ReturnPC, SEPMask: dispatch.SEPMask,
			}
			targets, ok := decoder.ResolveDispatchTargets(image, bank.ID, instruction, auth)
			if !ok {
				return nil, fmt.Errorf("authored indirect dispatch $%02X:%04X table cannot be read", bank.ID, dispatch.SitePC)
			}
			if instruction.Mnemonic == "PHA" {
				for index, target := range targets {
					if target == 0 || uint16(target+1) < 0x8000 {
						targets[index] = 0
					} else {
						targets[index] = target&0xff0000 | uint32(uint16(target+1))
					}
				}
			}
			dispatchKind := "short"
			if instruction.Length == 4 || len(dispatch.TableBases) == 3 {
				dispatchKind = "long"
			}
			fact := analysis.DispatchFact{
				SitePC:           decoder.Address24(bank.ID, dispatch.SitePC),
				InstructionBytes: shadowInstructionBytes(image, bank.ID, dispatch.SitePC, instruction.Length),
				Mnemonic:         instruction.Mnemonic, AddressingMode: instruction.Mode.String(),
				Transfer:        authoredTransferForInstruction(instruction, dispatch),
				TargetEntryKind: analysis.EntryComputed,
				Targets:         normalizeShadowTargets(bank.ID, targets, dispatchKind), TargetSetClosed: true,
				IndexRegister: dispatch.IndexReg, TableBases: normalizeTableBases(bank.ID, dispatch.TableBases),
				SEPMask:  dispatch.SEPMask,
				Evidence: []analysis.Evidence{{Source: "authored.config.indirect_dispatch", Confidence: analysis.ConfidenceAuthored}},
			}
			if len(fact.TableBases) == 0 && instruction.Operand >= 0x8000 {
				fact.TableBases = []uint32{decoder.Address24(bank.ID, uint16(instruction.Operand))}
			}
			if dispatch.ReturnPC != nil {
				value := decoder.Address24(bank.ID, *dispatch.ReturnPC)
				fact.ReturnPC = &value
			}
			fact.Normalize()
			facts = append(facts, fact)
		}
		for _, dispatch := range bank.Config.RTSDispatch {
			instruction, err := decodeShadowInstruction(image, bank.ID, dispatch.SitePC, 1, 1)
			if err != nil {
				return nil, fmt.Errorf("authored RTS dispatch $%02X:%04X: %w", bank.ID, dispatch.SitePC, err)
			}
			targets := make([]uint32, 0, len(dispatch.Targets))
			for _, target := range dispatch.Targets {
				targets = append(targets, decoder.Address24(bank.ID, target))
			}
			fact := analysis.DispatchFact{
				SitePC:           decoder.Address24(bank.ID, dispatch.SitePC),
				InstructionBytes: shadowInstructionBytes(image, bank.ID, dispatch.SitePC, instruction.Length),
				Mnemonic:         instruction.Mnemonic, AddressingMode: instruction.Mode.String(),
				Transfer: analysis.TransferResume, TargetEntryKind: analysis.EntryContinuation,
				Targets: targets, TargetSetClosed: true,
				Evidence: []analysis.Evidence{{Source: "authored.config.rts_dispatch", Confidence: analysis.ConfidenceAuthored}},
			}
			fact.Normalize()
			facts = append(facts, fact)
		}
	}
	return facts, nil
}

func inferShadowFacts(image romimage.Image, banks []shadowBank, regions []decoder.DataRegion, calleeExitMX map[decoder.Variant]decoder.MX, jobs int) ([]analysis.DispatchFact, int, []ShadowUnresolvedSite, []ShadowDecodeIssue, shadowInferenceStats, error) {
	entries := make(map[byte][]config.Entry, len(banks))
	bankConfigs := make(map[byte]*config.Config, len(banks))
	stats := shadowInferenceStats{}
	for _, bank := range banks {
		entries[bank.ID] = append([]config.Entry(nil), bank.Config.Entries...)
		bankConfigs[bank.ID] = bank.Config
		stats.initialVariants += len(bank.Config.Entries)
	}

	var results []shadowDecodeResult
	const passLimit = 32
	for pass := 1; pass <= passLimit; pass++ {
		stats.passes = pass
		var demands map[decoder.Variant]struct{}
		results, demands = runShadowDecodePass(image, banks, entries, regions, calleeExitMX, jobs)
		added := applyShadowDemands(image, entries, bankConfigs, regions, demands)
		if added == 0 {
			break
		}
		if pass == passLimit {
			return nil, 0, nil, nil, stats, fmt.Errorf("shadow variant discovery did not converge in %d passes", passLimit)
		}
	}
	for _, bankEntries := range entries {
		stats.finalVariants += len(bankEntries)
	}

	facts, rawUnresolved, unresolved, issues := summarizeShadowResults(image, results)
	facts = inferShadowContinuationFacts(image, banks, regions, calleeExitMX, facts, collectShadowContinuationReaches(results))
	return facts, rawUnresolved, unresolved, issues, stats, nil
}

func runShadowDecodePass(image romimage.Image, banks []shadowBank, entries map[byte][]config.Entry, regions []decoder.DataRegion, calleeExitMX map[decoder.Variant]decoder.MX, jobs int) ([]shadowDecodeResult, map[decoder.Variant]struct{}) {
	type task struct {
		bank     shadowBank
		entry    config.Entry
		siblings map[uint16]struct{}
	}
	var tasks []task
	for _, bank := range banks {
		bankEntries := entries[bank.ID]
		siblings := make(map[uint16]struct{}, len(bankEntries))
		for _, entry := range bankEntries {
			siblings[entry.Start] = struct{}{}
		}
		for _, entry := range bankEntries {
			ownSiblings := make(map[uint16]struct{}, len(siblings)-1)
			for pc := range siblings {
				if pc != entry.Start {
					ownSiblings[pc] = struct{}{}
				}
			}
			tasks = append(tasks, task{bank: bank, entry: entry, siblings: ownSiblings})
		}
	}
	input := make(chan task)
	output := make(chan shadowDecodeResult)
	var workers sync.WaitGroup
	for range max(1, jobs) {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for item := range input {
				options := decoder.Options{
					End: item.entry.End, DataRegions: regions,
					HLEDispatch:     item.bank.Config.HLEDispatch,
					CalleeExitMX:    calleeExitMX,
					SiblingEntryPCs: item.siblings,
				}
				graph, err := decoder.DecodeFunction(image, item.bank.ID, item.entry.Start, item.entry.EntryMX.M, item.entry.EntryMX.X, options)
				if err != nil {
					output <- shadowDecodeResult{issue: &ShadowDecodeIssue{
						FunctionEntry: decoder.Address24(item.bank.ID, item.entry.Start),
						EntryMX:       analysis.MXState{M: item.entry.EntryMX.M & 1, X: item.entry.EntryMX.X & 1}, Error: err.Error(),
					}}
					continue
				}
				facts := inferredFactsFromGraph(image, item.bank.ID, item.entry.Start, graph)
				output <- shadowDecodeResult{
					facts: facts, unresolved: graph.UnresolvedIndirects,
					demands:   discoverShadowDemands(item.bank.ID, graph, item.siblings),
					seedReach: discoverShadowContinuationReaches(item.bank.ID, graph),
					spans:     shadowDecodedSpans(item.bank.ID, item.entry.Start, graph),
				}
			}
		}()
	}
	go func() {
		for _, item := range tasks {
			input <- item
		}
		close(input)
		workers.Wait()
		close(output)
	}()

	var results []shadowDecodeResult
	demands := make(map[decoder.Variant]struct{})
	for result := range output {
		results = append(results, result)
		for demand := range result.demands {
			demands[demand] = struct{}{}
		}
	}
	return results, demands
}

func shadowDecodedSpans(bank byte, functionStart uint16, graph *decoder.Graph) []shadowDecodedSpan {
	result := make([]shadowDecodedSpan, 0, len(graph.Order))
	entry := decoder.Address24(bank, functionStart)
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		result = append(result, shadowDecodedSpan{
			PC: decoded.Key.PC & 0xffffff, FunctionEntry: entry,
			Length: decoded.Instruction.Length,
		})
	}
	return result
}

// discoverShadowDemands propagates the decoder's finite live M/X state across
// direct calls and statically recovered dispatch edges. It records demands
// only in this in-memory analysis run; authored dynamic-dispatch declarations
// are intentionally absent.
func discoverShadowDemands(bank byte, graph *decoder.Graph, siblingStarts map[uint16]struct{}) map[decoder.Variant]struct{} {
	result := make(map[decoder.Variant]struct{})
	one := func(address uint32, m, x uint8) {
		result[decoder.Variant{Address: address & 0xffffff, M: m & 1, X: x & 1}] = struct{}{}
	}
	for _, decoded := range graph.Instructions {
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		if len(instruction.DispatchEntries) > 0 {
			if instruction.DispatchKind == "rts_trick" {
				continue
			}
			for _, target := range instruction.DispatchEntries {
				if target == 0 {
					continue
				}
				if instruction.DispatchKind != "long" {
					target = decoder.Address24(bank, uint16(target))
				}
				if (instruction.Mnemonic == "JSL" || (instruction.Mnemonic == "JMP" && instruction.Mode == cpu65816.LONG)) && instruction.DispatchIndexReg == "" {
					one(target, 1, 1)
				} else {
					m, x := instruction.M&1, instruction.X&1
					if instruction.DispatchTerminal {
						m, x = 1, 1
					} else {
						if instruction.DispatchSEP&0x20 != 0 {
							m = 1
						}
						if instruction.DispatchSEP&0x10 != 0 {
							x = 1
						}
					}
					one(target, m, x)
				}
			}
			continue
		}
		switch {
		case instruction.Mnemonic == "JSR" && instruction.Mode == cpu65816.ABS:
			one(decoder.Address24(bank, uint16(instruction.Operand)), instruction.M, instruction.X)
		case instruction.Mnemonic == "JSL":
			one(instruction.Operand, instruction.M, instruction.X)
		case instruction.Mnemonic == "JMP" && instruction.Mode == cpu65816.LONG:
			one(instruction.Operand, instruction.M, instruction.X)
		}
		for _, successor := range decoded.Successors {
			if _, known := siblingStarts[uint16(successor.PC)]; !known {
				continue
			}
			if graph.Instructions[successor] != nil {
				continue
			}
			result[decoder.Variant{Address: successor.PC & 0xffffff, M: successor.M & 1, X: successor.X & 1}] = struct{}{}
		}
	}
	return result
}

func discoverShadowContinuationReaches(bank byte, graph *decoder.Graph) []shadowContinuationReach {
	predecessors := shadowPredecessors(graph)
	var result []shadowContinuationReach
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		register := ""
		wordWidth := false
		switch instruction.Mnemonic {
		case "PHA":
			register, wordWidth = "LDA", key.M == 0
		case "PHX":
			register, wordWidth = "LDX", key.X == 0
		case "PHY":
			register, wordWidth = "LDY", key.X == 0
		default:
			continue
		}
		if !wordWidth {
			continue
		}
		preds := predecessors[key]
		if len(preds) != 1 {
			continue
		}
		load := graph.Instructions[preds[0]]
		if load == nil || load.Instruction == nil || load.Instruction.Mnemonic != register || load.Instruction.Mode != cpu65816.IMM {
			continue
		}
		target := uint16(load.Instruction.Operand + 1)
		if target < 0x8000 {
			continue
		}
		result = append(result, followShadowContinuationSeed(bank, graph, decoded.Successors, decoder.Address24(bank, target))...)
	}
	return result
}

func followShadowContinuationSeed(bank byte, graph *decoder.Graph, starts []decoder.DecodeKey, target uint32) []shadowContinuationReach {
	type state struct {
		key   decoder.DecodeKey
		above int
	}
	queue := make([]state, 0, len(starts))
	for _, key := range starts {
		queue = append(queue, state{key: key})
	}
	seen := make(map[state]struct{})
	var result []shadowContinuationReach
	for len(queue) > 0 && len(seen) < 4096 {
		current := queue[0]
		queue = queue[1:]
		if current.above < 0 || current.above > 32 {
			continue
		}
		if _, found := seen[current]; found {
			continue
		}
		seen[current] = struct{}{}
		decoded := graph.Instructions[current.key]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		if instruction.Mnemonic == "RTS" {
			result = append(result, shadowContinuationReach{
				SitePC: instruction.Address & 0xffffff, Target: target & 0xffffff, BytesAbove: current.above,
				LiveMX: analysis.MXState{M: current.key.M & 1, X: current.key.X & 1},
			})
			continue
		}
		if current.above == 0 && (instruction.Mnemonic == "BRA" || instruction.Mnemonic == "BRL" || (instruction.Mnemonic == "JMP" && instruction.Mode != cpu65816.INDIR && instruction.Mode != cpu65816.INDIRX)) {
			for _, successor := range decoded.Successors {
				result = append(result, shadowContinuationReach{
					SitePC: successor.PC & 0xffffff, Target: target & 0xffffff, BytesAbove: current.above,
					LiveMX: analysis.MXState{M: successor.M & 1, X: successor.X & 1}, EntryEdge: true,
				})
			}
		}
		delta, valid := shadowStackDelta(instruction, current.key)
		if !valid {
			continue
		}
		nextAbove := current.above + delta
		if nextAbove < 0 {
			continue
		}
		for _, successor := range decoded.Successors {
			queue = append(queue, state{key: successor, above: nextAbove})
		}
	}
	return result
}

func shadowStackDelta(instruction *cpu65816.Instruction, key decoder.DecodeKey) (int, bool) {
	switch instruction.Mnemonic {
	case "PHA":
		if key.M != 0 {
			return 1, true
		}
		return 2, true
	case "PHX", "PHY":
		if key.X != 0 {
			return 1, true
		}
		return 2, true
	case "PHP", "PHB", "PHK":
		return 1, true
	case "PHD", "PEA", "PER":
		return 2, true
	case "PLA":
		if key.M != 0 {
			return -1, true
		}
		return -2, true
	case "PLX", "PLY":
		if key.X != 0 {
			return -1, true
		}
		return -2, true
	case "PLP", "PLB":
		return -1, true
	case "PLD":
		return -2, true
	case "TCS", "TXS":
		return 0, false
	default:
		return 0, true
	}
}

func collectShadowContinuationReaches(results []shadowDecodeResult) []shadowContinuationReach {
	seen := make(map[shadowContinuationReach]struct{})
	for _, result := range results {
		for _, reach := range result.seedReach {
			seen[reach] = struct{}{}
		}
	}
	values := make([]shadowContinuationReach, 0, len(seen))
	for value := range seen {
		values = append(values, value)
	}
	sort.Slice(values, func(i, j int) bool {
		if values[i].SitePC != values[j].SitePC {
			return values[i].SitePC < values[j].SitePC
		}
		if values[i].Target != values[j].Target {
			return values[i].Target < values[j].Target
		}
		if values[i].BytesAbove != values[j].BytesAbove {
			return values[i].BytesAbove < values[j].BytesAbove
		}
		if values[i].EntryEdge != values[j].EntryEdge {
			return !values[i].EntryEdge
		}
		if values[i].LiveMX.M != values[j].LiveMX.M {
			return values[i].LiveMX.M < values[j].LiveMX.M
		}
		return values[i].LiveMX.X < values[j].LiveMX.X
	})
	return values
}

func applyShadowDemands(image romimage.Image, entries map[byte][]config.Entry, bankConfigs map[byte]*config.Config, regions []decoder.DataRegion, demands map[decoder.Variant]struct{}) int {
	keys := make([]decoder.Variant, 0, len(demands))
	for demand := range demands {
		keys = append(keys, demand)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].Address != keys[j].Address {
			return keys[i].Address < keys[j].Address
		}
		if keys[i].M != keys[j].M {
			return keys[i].M < keys[j].M
		}
		return keys[i].X < keys[j].X
	})
	added := 0
	for _, demand := range keys {
		address := demand.Address & 0xffffff
		pc := uint16(address)
		offset, err := romimage.LoROMOffset(byte(address>>16), pc)
		if err != nil || offset < 0 || offset >= len(image) {
			continue
		}
		bank := shadowCanonicalBank(bankConfigs, byte(address>>16))
		if bankConfigs[bank] == nil || shadowInDataRegion(regions, bank, pc) {
			continue
		}
		bankEntries := entries[bank]
		found := false
		baseIndex := -1
		for index := range bankEntries {
			entry := &bankEntries[index]
			if entry.Start != pc {
				continue
			}
			if baseIndex < 0 {
				baseIndex = index
			}
			if entry.EntryMX.M&1 == demand.M&1 && entry.EntryMX.X&1 == demand.X&1 {
				found = true
				break
			}
		}
		if found {
			continue
		}
		entry := config.Entry{Name: fmt.Sprintf("bank_%02X_%04X", bank, pc), Start: pc, EntryMX: config.MX{M: demand.M & 1, X: demand.X & 1}}
		if baseIndex >= 0 {
			entry = bankEntries[baseIndex]
			entry.EntryMX = config.MX{M: demand.M & 1, X: demand.X & 1}
		}
		entries[bank] = append(bankEntries, entry)
		added++
	}
	return added
}

func shadowCanonicalBank(banks map[byte]*config.Config, bank byte) byte {
	if banks[bank] != nil {
		return bank
	}
	if bank < 0x40 || (bank >= 0x80 && bank < 0xc0) {
		if banks[bank^0x80] != nil {
			return bank ^ 0x80
		}
	}
	return bank
}

func shadowInDataRegion(regions []decoder.DataRegion, bank byte, pc uint16) bool {
	for _, region := range regions {
		if region.Bank == bank && pc >= region.Start && pc < region.End {
			return true
		}
	}
	return false
}

func summarizeShadowResults(image romimage.Image, results []shadowDecodeResult) ([]analysis.DispatchFact, int, []ShadowUnresolvedSite, []ShadowDecodeIssue) {
	factMap := make(map[uint32]analysis.DispatchFact)
	interiorOwners := make(map[uint32][]shadowDecodedSpan)
	unresolvedMap := make(map[uint32]*ShadowUnresolvedSite)
	rawUnresolved := 0
	var issues []ShadowDecodeIssue
	for _, result := range results {
		if result.issue != nil {
			issues = append(issues, *result.issue)
			continue
		}
		for _, fact := range result.facts {
			mergeShadowFact(factMap, fact)
		}
		for _, span := range result.spans {
			for offset := uint8(1); offset < span.Length; offset++ {
				address := (span.PC & 0xff0000) | uint32(uint16(span.PC)+uint16(offset))
				interiorOwners[address] = append(interiorOwners[address], span)
			}
		}
		for _, unresolved := range result.unresolved {
			rawUnresolved++
			site := unresolved.SitePC & 0xffffff
			record := unresolvedMap[site]
			if record == nil {
				instruction, _ := decodeShadowInstruction(image, byte(site>>16), uint16(site), unresolved.EntryM, unresolved.EntryX)
				record = &ShadowUnresolvedSite{
					SitePC: site, Mnemonic: unresolved.Mnemonic, AddressingMode: unresolved.Mode.String(), Operand: unresolved.Operand,
					Reason: "runtime target has no finite statically proven set", Reachability: "configuration_or_static_call_rooted",
				}
				if instruction != nil {
					record.InstructionBytes = shadowInstructionBytes(image, byte(site>>16), uint16(site), instruction.Length)
				}
				unresolvedMap[site] = record
			}
			record.Callers = append(record.Callers, ShadowCaller{FunctionEntry: unresolved.FunctionEntry & 0xffffff, LiveMX: analysis.MXState{M: unresolved.EntryM & 1, X: unresolved.EntryX & 1}})
		}
	}
	facts := make([]analysis.DispatchFact, 0, len(factMap))
	for _, fact := range factMap {
		classifyShadowFactOwnership(&fact, interiorOwners[fact.SitePC&0xffffff])
		fact.Normalize()
		facts = append(facts, fact)
	}
	sort.Slice(facts, func(i, j int) bool { return facts[i].SitePC < facts[j].SitePC })
	unresolved := make([]ShadowUnresolvedSite, 0, len(unresolvedMap))
	for _, record := range unresolvedMap {
		sort.Slice(record.Callers, func(i, j int) bool {
			if record.Callers[i].FunctionEntry != record.Callers[j].FunctionEntry {
				return record.Callers[i].FunctionEntry < record.Callers[j].FunctionEntry
			}
			if record.Callers[i].LiveMX.M != record.Callers[j].LiveMX.M {
				return record.Callers[i].LiveMX.M < record.Callers[j].LiveMX.M
			}
			return record.Callers[i].LiveMX.X < record.Callers[j].LiveMX.X
		})
		record.Callers = dedupeShadowCallers(record.Callers)
		unresolved = append(unresolved, *record)
	}
	sort.Slice(unresolved, func(i, j int) bool { return unresolved[i].SitePC < unresolved[j].SitePC })
	sort.Slice(issues, func(i, j int) bool {
		if issues[i].FunctionEntry != issues[j].FunctionEntry {
			return issues[i].FunctionEntry < issues[j].FunctionEntry
		}
		if issues[i].EntryMX.M != issues[j].EntryMX.M {
			return issues[i].EntryMX.M < issues[j].EntryMX.M
		}
		return issues[i].EntryMX.X < issues[j].EntryMX.X
	})
	return facts, rawUnresolved, unresolved, issues
}

func classifyShadowFactOwnership(fact *analysis.DispatchFact, owners []shadowDecodedSpan) {
	if fact.TargetSetClosed || len(owners) == 0 || fact.CodeOwnership != "" {
		return
	}
	decoderHeuristic := false
	for _, evidence := range fact.Evidence {
		if evidence.Source == "static.decoder.auto_dispatch" && evidence.Confidence == analysis.ConfidenceProbable {
			decoderHeuristic = true
			break
		}
	}
	if !decoderHeuristic {
		return
	}
	for _, entry := range fact.FunctionEntries {
		if entry&0xffffff == fact.SitePC&0xffffff {
			return
		}
	}
	sort.Slice(owners, func(i, j int) bool {
		if owners[i].PC != owners[j].PC {
			return owners[i].PC < owners[j].PC
		}
		return owners[i].FunctionEntry < owners[j].FunctionEntry
	})
	owner := owners[0]
	fact.CodeOwnership = analysis.OwnershipGarbageOnly
	fact.Evidence = append(fact.Evidence, analysis.Evidence{
		Source: "static.code_ownership.overlapping_instruction", Confidence: analysis.ConfidenceProbable,
		Detail: fmt.Sprintf("site $%06X is strictly inside the instruction at $%06X decoded from entry $%06X", fact.SitePC&0xffffff, owner.PC&0xffffff, owner.FunctionEntry&0xffffff),
	})
}

func inferShadowContinuationFacts(image romimage.Image, banks []shadowBank, regions []decoder.DataRegion, calleeExitMX map[decoder.Variant]decoder.MX, facts []analysis.DispatchFact, seedReach []shadowContinuationReach) []analysis.DispatchFact {
	bankConfigs := make(map[byte]*config.Config, len(banks))
	for _, bank := range banks {
		bankConfigs[bank.ID] = bank.Config
	}
	type dispatcher struct {
		fact         analysis.DispatchFact
		continuation uint32
	}
	dispatchers := make(map[uint32]dispatcher)
	for _, fact := range facts {
		if !shadowFactHasEvidence(fact, "static.stack_shape.pushed_target_rts") || !fact.TargetSetClosed {
			continue
		}
		continuation, ok := recoverContinuationBeforeRTS(image, fact.SitePC)
		if ok {
			dispatchers[fact.SitePC&0xffffff] = dispatcher{fact: fact, continuation: continuation}
		}
	}

	type continuationFact struct {
		targets []uint32
		liveMX  []analysis.MXState
		detail  []string
	}
	continuations := make(map[uint32]*continuationFact)
	add := func(site, target uint32, liveMX analysis.MXState, detail string) {
		site &= 0xffffff
		target &= 0xffffff
		if uint16(site) < 0x8000 || uint16(target) < 0x8000 {
			return
		}
		record := continuations[site]
		if record == nil {
			record = &continuationFact{}
			continuations[site] = record
		}
		record.targets = appendUniqueAddresses(record.targets, target)
		record.liveMX = append(record.liveMX, liveMX)
		record.detail = append(record.detail, detail)
	}

	for _, dispatch := range dispatchers {
		states := dispatch.fact.LiveMX
		if len(states) == 0 {
			states = []analysis.MXState{{}}
		}
		for _, target := range dispatch.fact.Targets {
			for _, state := range states {
				for _, exit := range collectReachableRTSSites(image, bankConfigs, regions, calleeExitMX, target, state) {
					add(exit.SitePC, dispatch.continuation, exit.LiveMX, "computed handler exit resumes its statically pushed dispatcher continuation")
				}
			}
		}
	}
	for _, reach := range seedReach {
		if reach.EntryEdge {
			if reach.BytesAbove != 0 {
				continue
			}
			for _, exit := range collectReachableRTSSites(image, bankConfigs, regions, calleeExitMX, reach.SitePC, reach.LiveMX) {
				if dispatch, ok := dispatchers[exit.SitePC&0xffffff]; ok {
					add(dispatch.continuation, reach.Target, exit.LiveMX, "an outer continuation reaches a nested dispatcher through a direct handler entry")
				} else {
					add(exit.SitePC, reach.Target, exit.LiveMX, "a direct handler entry preserves its scanner continuation through the terminal RTS")
				}
			}
			continue
		}
		switch reach.BytesAbove {
		case 4:
			if dispatch, ok := dispatchers[reach.SitePC&0xffffff]; ok {
				add(dispatch.continuation, reach.Target, reach.LiveMX, "an outer continuation remains below a nested handler and its dispatcher continuation")
			}
		}
	}

	factMap := make(map[uint32]analysis.DispatchFact, len(facts)+len(continuations))
	for _, fact := range facts {
		mergeShadowFact(factMap, fact)
	}
	for site, record := range continuations {
		instruction, err := decodeShadowInstruction(image, byte(site>>16), uint16(site), 1, 1)
		if err != nil || instruction == nil || instruction.Mnemonic != "RTS" {
			continue
		}
		details := shadowUniqueStrings(record.detail)
		evidence := make([]analysis.Evidence, 0, len(details))
		for _, detail := range details {
			evidence = append(evidence, analysis.Evidence{Source: "static.stack_provenance.continuation", Confidence: analysis.ConfidenceProven, Detail: detail})
		}
		fact := analysis.DispatchFact{
			SitePC: site, InstructionBytes: shadowInstructionBytes(image, byte(site>>16), uint16(site), instruction.Length),
			Mnemonic: instruction.Mnemonic, AddressingMode: instruction.Mode.String(), LiveMX: record.liveMX,
			Transfer: analysis.TransferResume, TargetEntryKind: analysis.EntryContinuation,
			Targets: record.targets, TargetSetClosed: true, Evidence: evidence,
		}
		fact.Normalize()
		mergeShadowFact(factMap, fact)
	}
	result := make([]analysis.DispatchFact, 0, len(factMap))
	for _, fact := range factMap {
		fact.Normalize()
		result = append(result, fact)
	}
	sort.Slice(result, func(i, j int) bool { return result[i].SitePC < result[j].SitePC })
	return result
}

func shadowFactHasEvidence(fact analysis.DispatchFact, source string) bool {
	for _, evidence := range fact.Evidence {
		if evidence.Source == source {
			return true
		}
	}
	return false
}

func shadowUniqueStrings(values []string) []string {
	sort.Strings(values)
	result := values[:0]
	for _, value := range values {
		if len(result) == 0 || result[len(result)-1] != value {
			result = append(result, value)
		}
	}
	return result
}

func recoverContinuationBeforeRTS(image romimage.Image, site uint32) (uint32, bool) {
	bank := byte(site >> 16)
	pc := uint16(site)
	start := uint16(0x8000)
	if pc > 64 && pc-64 >= 0x8000 {
		start = pc - 64
	}
	bytes, err := image.Slice(bank, start, int(pc-start))
	if err != nil {
		return 0, false
	}
	for index := len(bytes) - 4; index >= 0; index-- {
		if bytes[index] != 0xa9 || bytes[index+3] != 0x48 {
			continue
		}
		raw := uint16(bytes[index+1]) | uint16(bytes[index+2])<<8
		target := raw + 1
		if target >= 0x8000 {
			return decoder.Address24(bank, target), true
		}
	}
	return 0, false
}

type shadowRTSExit struct {
	SitePC uint32
	LiveMX analysis.MXState
}

func collectReachableRTSSites(image romimage.Image, bankConfigs map[byte]*config.Config, regions []decoder.DataRegion, calleeExitMX map[decoder.Variant]decoder.MX, target uint32, state analysis.MXState) []shadowRTSExit {
	bank := byte(target >> 16)
	bank = shadowCanonicalBank(bankConfigs, bank)
	cfg := bankConfigs[bank]
	if cfg == nil {
		return nil
	}
	pc := uint16(target)
	options := decoder.Options{DataRegions: regions, HLEDispatch: cfg.HLEDispatch, CalleeExitMX: calleeExitMX}
	graph, err := decoder.DecodeFunction(image, bank, pc, state.M&1, state.X&1, options)
	if err != nil || graph == nil {
		return nil
	}
	seen := make(map[shadowRTSExit]struct{})
	for key, decoded := range graph.Instructions {
		if decoded == nil || decoded.Instruction == nil || decoded.Instruction.Mnemonic != "RTS" {
			continue
		}
		exit := shadowRTSExit{SitePC: decoded.Instruction.Address & 0xffffff, LiveMX: analysis.MXState{M: key.M & 1, X: key.X & 1}}
		seen[exit] = struct{}{}
	}
	result := make([]shadowRTSExit, 0, len(seen))
	for exit := range seen {
		result = append(result, exit)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].SitePC != result[j].SitePC {
			return result[i].SitePC < result[j].SitePC
		}
		if result[i].LiveMX.M != result[j].LiveMX.M {
			return result[i].LiveMX.M < result[j].LiveMX.M
		}
		return result[i].LiveMX.X < result[j].LiveMX.X
	})
	return result
}

func inferredFactsFromGraph(image romimage.Image, bank byte, functionStart uint16, graph *decoder.Graph) []analysis.DispatchFact {
	var result []analysis.DispatchFact
	predecessors := shadowPredecessors(graph)
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		if len(instruction.DispatchEntries) > 0 {
			fact := inferredDecoderDispatchFact(image, bank, functionStart, key, instruction)
			result = append(result, fact)
		}
		if instruction.Mnemonic == "PHA" {
			if fact, ok := inferPHARTSShape(image, bank, functionStart, graph, predecessors, key); ok {
				result = append(result, fact)
			}
		}
		if instruction.Mnemonic == "RTS" || instruction.Mnemonic == "RTL" {
			if fact, ok := inferLocalRTSChain(image, bank, functionStart, graph, predecessors, key); ok {
				result = append(result, fact)
			}
			if fact, ok := inferRTSDispatchShape(image, bank, functionStart, graph, predecessors, key); ok {
				result = append(result, fact)
			}
			if fact, ok := inferImmediateRTS(image, bank, functionStart, graph, predecessors, key); ok {
				result = append(result, fact)
			}
		}
	}
	return result
}

func inferLocalRTSChain(image romimage.Image, bank byte, functionStart uint16, graph *decoder.Graph, predecessors map[decoder.DecodeKey][]decoder.DecodeKey, rtsKey decoder.DecodeKey) (analysis.DispatchFact, bool) {
	rtsDecoded := graph.Instructions[rtsKey]
	if rtsDecoded == nil || rtsDecoded.Instruction == nil || rtsDecoded.Instruction.Mnemonic != "RTS" {
		return analysis.DispatchFact{}, false
	}
	rtsPC := uint16(rtsDecoded.Instruction.Address)
	var targets []uint32
	for key, decoded := range graph.Instructions {
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		register := ""
		wordWidth := false
		switch instruction.Mnemonic {
		case "PHA":
			register, wordWidth = "LDA", key.M == 0
		case "PHX":
			register, wordWidth = "LDX", key.X == 0
		case "PHY":
			register, wordWidth = "LDY", key.X == 0
		}
		if !wordWidth || !shadowGraphReaches(graph, key, rtsKey) {
			continue
		}
		preds := predecessors[key]
		if len(preds) != 1 {
			continue
		}
		load := graph.Instructions[preds[0]]
		if load == nil || load.Instruction == nil || load.Instruction.Mnemonic != register || load.Instruction.Mode != cpu65816.IMM {
			continue
		}
		target := uint16(load.Instruction.Operand + 1)
		if target < functionStart || target > rtsPC {
			continue
		}
		targets = appendUniqueAddresses(targets, decoder.Address24(bank, target))
	}
	if len(targets) < 2 {
		return analysis.DispatchFact{}, false
	}
	rts := rtsDecoded.Instruction
	fact := analysis.DispatchFact{
		SitePC: rts.Address & 0xffffff, FunctionEntries: []uint32{decoder.Address24(bank, functionStart)},
		InstructionBytes: shadowInstructionBytes(image, bank, rtsPC, rts.Length),
		Mnemonic:         rts.Mnemonic, AddressingMode: rts.Mode.String(), LiveMX: []analysis.MXState{{M: rtsKey.M & 1, X: rtsKey.X & 1}},
		Transfer: analysis.TransferResume, TargetEntryKind: analysis.EntryContinuation,
		Targets: targets, TargetSetClosed: true,
		Evidence: []analysis.Evidence{{Source: "static.stack_provenance.local_rts_chain", Confidence: analysis.ConfidenceProven, Detail: "multiple finite word pushes in the owned region converge on this RTS chain site"}},
	}
	fact.Normalize()
	return fact, true
}

func shadowGraphReaches(graph *decoder.Graph, start, target decoder.DecodeKey) bool {
	queue := append([]decoder.DecodeKey(nil), graph.Instructions[start].Successors...)
	seen := make(map[decoder.DecodeKey]struct{})
	for len(queue) > 0 && len(seen) < 4096 {
		key := queue[0]
		queue = queue[1:]
		if key == target {
			return true
		}
		if _, found := seen[key]; found {
			continue
		}
		seen[key] = struct{}{}
		if decoded := graph.Instructions[key]; decoded != nil {
			queue = append(queue, decoded.Successors...)
		}
	}
	return false
}

func inferredDecoderDispatchFact(image romimage.Image, bank byte, functionStart uint16, key decoder.DecodeKey, instruction *cpu65816.Instruction) analysis.DispatchFact {
	targets := normalizeShadowTargets(bank, instruction.DispatchEntries, instruction.DispatchKind)
	tableBases := normalizeTableBases(bank, instruction.DispatchTableBase)
	if len(tableBases) == 0 && (instruction.Mode == cpu65816.INDIRX || instruction.Mode == cpu65816.INDIR) && instruction.Operand >= 0x8000 {
		tableBases = []uint32{decoder.Address24(bank, uint16(instruction.Operand))}
	}
	fact := analysis.DispatchFact{
		SitePC: instruction.Address & 0xffffff, FunctionEntries: []uint32{decoder.Address24(bank, functionStart)},
		InstructionBytes: shadowInstructionBytes(image, bank, uint16(instruction.Address), instruction.Length),
		Mnemonic:         instruction.Mnemonic, AddressingMode: instruction.Mode.String(), LiveMX: []analysis.MXState{{M: key.M & 1, X: key.X & 1}},
		Transfer: transferForInstruction(instruction, instruction.DispatchReturn != nil), TargetEntryKind: analysis.EntryComputed,
		TargetCandidates: targets, TargetSetClosed: false, IndexRegister: instruction.DispatchIndexReg,
		TableBases: tableBases, SEPMask: instruction.DispatchSEP,
		UnknownFields: []string{"targets"},
		Evidence:      []analysis.Evidence{{Source: "static.decoder.auto_dispatch", Confidence: analysis.ConfidenceProbable, Detail: "target entries recovered without authored dispatch directives; table bound remains heuristic"}},
	}
	if instruction.DispatchReturn != nil {
		value := decoder.Address24(bank, *instruction.DispatchReturn)
		fact.ReturnPC = &value
	}
	fact.Normalize()
	return fact
}

func inferPHARTSShape(image romimage.Image, bank byte, functionStart uint16, graph *decoder.Graph, predecessors map[decoder.DecodeKey][]decoder.DecodeKey, phaKey decoder.DecodeKey) (analysis.DispatchFact, bool) {
	pha := graph.Instructions[phaKey]
	if pha == nil || pha.Instruction == nil || phaKey.M != 0 {
		return analysis.DispatchFact{}, false
	}
	sepMask, reachesRTS := shadowPHAReachesRTS(graph, phaKey)
	if !reachesRTS {
		return analysis.DispatchFact{}, false
	}
	var returnPC *uint32
	var tableBases []uint32
	var targets []uint32
	targetSetClosed := false
	unknownTable := true
	malformedReturnPush := false
	var dynamicTableLoad *decoder.DecodeKey
	var backward []decoder.DecodeKey
	cursor := phaKey
	for steps := 0; steps < 48; steps++ {
		preds := predecessors[cursor]
		if len(preds) != 1 {
			break
		}
		cursor = preds[0]
		backward = append(backward, cursor)
	}
	for _, cursor := range backward {
		decoded := graph.Instructions[cursor]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		if returnPC == nil && (instruction.Mnemonic == "PHY" || instruction.Mnemonic == "PHX" || instruction.Mnemonic == "PHA") {
			register := "LDA"
			wordWidth := cursor.M == 0
			if instruction.Mnemonic == "PHY" {
				register = "LDY"
				wordWidth = cursor.X == 0
			} else if instruction.Mnemonic == "PHX" {
				register = "LDX"
				wordWidth = cursor.X == 0
			}
			pushPreds := predecessors[cursor]
			if len(pushPreds) == 1 {
				load := graph.Instructions[pushPreds[0]]
				if load != nil && load.Instruction != nil && load.Instruction.Mnemonic == register && load.Instruction.Mode == cpu65816.IMM {
					if !wordWidth {
						// A one-byte push cannot supply the word below a PHA/RTS
						// target. Treat this width variant as an invalid
						// stack interpretation, not as an alternate transfer.
						malformedReturnPush = true
					} else {
						value := decoder.Address24(bank, uint16(load.Instruction.Operand+1))
						returnPC = &value
					}
				}
			}
		}
	}
	if malformedReturnPush {
		return analysis.DispatchFact{}, false
	}
	for _, cursor := range backward {
		decoded := graph.Instructions[cursor]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		if instruction.Mnemonic != "LDA" {
			if shadowWritesAccumulator(instruction) {
				break
			}
			continue
		}
		switch instruction.Mode {
		case cpu65816.ABSX, cpu65816.ABSY, cpu65816.LONGX:
			targetBank := bank
			if instruction.Mode == cpu65816.LONGX {
				targetBank = byte(instruction.Operand >> 16)
			}
			base := uint16(instruction.Operand)
			if base >= 0x8000 {
				tableBases = []uint32{decoder.Address24(targetBank, base)}
				unknownTable = false
			} else if instruction.Mode == cpu65816.LONGX {
				key := cursor
				dynamicTableLoad = &key
			}
		}
		break
	}
	if shadowHasImmediateWordContinuation(image, bank, uint16(pha.Instruction.Address)) {
		if phaKey.X != 0 {
			// The bytes immediately preceding this site own a 16-bit
			// LDY/LDX + push sequence. A decode claiming X=1 at the PHA
			// starts in the middle of that owned sequence.
			return analysis.DispatchFact{}, false
		}
		if returnPC == nil {
			value, _ := shadowImmediateWordContinuation(image, bank, uint16(pha.Instruction.Address))
			returnPC = &value
		}
	}
	usedNestedTable := false
	if len(tableBases) == 0 && dynamicTableLoad != nil {
		if base, recovered, ok := recoverNestedPackedWordTable(image, bank, graph, predecessors, *dynamicTableLoad); ok {
			tableBases = []uint32{decoder.Address24(bank, base)}
			targets = recovered
			targetSetClosed = true
			unknownTable = false
			usedNestedTable = true
		}
	}
	if len(tableBases) == 1 {
		tableBank := byte(tableBases[0] >> 16)
		base := uint16(tableBases[0])
		if recovered, ok := recoverSelfDelimitedWordTable(image, tableBank, base, true); ok {
			targets = recovered
			targetSetClosed = true
		}
	}
	if len(tableBases) == 0 && !targetSetClosed {
		return analysis.DispatchFact{}, false
	}
	fact := analysis.DispatchFact{
		SitePC: pha.Instruction.Address & 0xffffff, FunctionEntries: []uint32{decoder.Address24(bank, functionStart)},
		InstructionBytes: shadowInstructionBytes(image, bank, uint16(pha.Instruction.Address), pha.Instruction.Length),
		Mnemonic:         "PHA", AddressingMode: pha.Instruction.Mode.String(), LiveMX: []analysis.MXState{{M: phaKey.M & 1, X: phaKey.X & 1}},
		Transfer: analysis.TransferTail, TargetEntryKind: analysis.EntryComputed,
		Targets: targets, TargetSetClosed: targetSetClosed,
		IndexRegister: "A", TableBases: tableBases, ReturnPC: returnPC, SEPMask: sepMask,
		Evidence: []analysis.Evidence{{Source: "static.stack_shape.pha_rts", Confidence: analysis.ConfidenceProven, Detail: "16-bit handler value is pushed and consumed by a following RTS"}},
	}
	if !targetSetClosed {
		fact.UnknownFields = append(fact.UnknownFields, "targets")
	} else {
		if usedNestedTable {
			fact.Evidence = append(fact.Evidence, analysis.Evidence{
				Source: "static.rom_table.nested_pointer_lists", Confidence: analysis.ConfidenceProven,
				Detail: "an outer self-delimited pointer table selects packed inner word lists ending at their earliest handler",
			})
		} else {
			fact.Evidence = append(fact.Evidence, analysis.Evidence{
				Source: "static.rom_table.self_delimited", Confidence: analysis.ConfidenceProven,
				Detail: "the packed word table ends exactly where its earliest same-bank handler begins",
			})
		}
	}
	if returnPC != nil {
		fact.Transfer = analysis.TransferCall
	}
	if unknownTable {
		fact.UnknownFields = append(fact.UnknownFields, "table_bases")
	}
	fact.Normalize()
	return fact, true
}

func inferRTSDispatchShape(image romimage.Image, bank byte, functionStart uint16, graph *decoder.Graph, predecessors map[decoder.DecodeKey][]decoder.DecodeKey, rtsKey decoder.DecodeKey) (analysis.DispatchFact, bool) {
	rtsDecoded := graph.Instructions[rtsKey]
	if rtsDecoded == nil || rtsDecoded.Instruction == nil || rtsDecoded.Instruction.Mnemonic != "RTS" {
		return analysis.DispatchFact{}, false
	}
	backward := shadowBackwardKeys(predecessors, rtsKey, 48)
	var handlerPHA *decoder.DecodeKey
	var source *cpu65816.Instruction
	for _, key := range backward {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil || decoded.Instruction.Mnemonic != "PHA" || key.M != 0 {
			continue
		}
		pushPreds := predecessors[key]
		if len(pushPreds) != 1 {
			continue
		}
		load := graph.Instructions[pushPreds[0]]
		if load == nil || load.Instruction == nil || load.Instruction.Mnemonic != "LDA" || load.Instruction.Mode == cpu65816.IMM {
			continue
		}
		candidate := key
		handlerPHA = &candidate
		source = load.Instruction
		break
	}
	if handlerPHA == nil || source == nil {
		return analysis.DispatchFact{}, false
	}
	returnPC, hasReturn := shadowContinuationBeforeHandlerPHA(bank, graph, predecessors, *handlerPHA)
	if !hasReturn {
		return analysis.DispatchFact{}, false
	}

	var targets []uint32
	var evidence analysis.Evidence
	switch source.Mode {
	case cpu65816.ABS:
		var ok bool
		targets, ok = recoverImmediateStoreTargets(image, bank, uint16(source.Operand))
		if !ok {
			return analysis.DispatchFact{}, false
		}
		evidence = analysis.Evidence{
			Source: "static.value_provenance.wram_handler", Confidence: analysis.ConfidenceProven,
			Detail: "all absolute stores to the selected handler word are immediate finite values",
		}
	case cpu65816.LONGX:
		if uint16(source.Operand) >= 0x8000 || byte(source.Operand>>16) != bank {
			return analysis.DispatchFact{}, false
		}
		var ok bool
		targets, ok = recoverBranchSelectedHandlerTables(image, bank, graph, predecessors, backward)
		if !ok {
			return analysis.DispatchFact{}, false
		}
		evidence = analysis.Evidence{
			Source: "static.value_provenance.branch_selected_tables", Confidence: analysis.ConfidenceProven,
			Detail: "finite immediate table pointers converge on a shared indexed handler-table dispatcher",
		}
	default:
		return analysis.DispatchFact{}, false
	}
	_ = returnPC // consumed by continuation propagation in the next analysis phase
	rts := rtsDecoded.Instruction
	fact := analysis.DispatchFact{
		SitePC: rts.Address & 0xffffff, FunctionEntries: []uint32{decoder.Address24(bank, functionStart)},
		InstructionBytes: shadowInstructionBytes(image, bank, uint16(rts.Address), rts.Length),
		Mnemonic:         rts.Mnemonic, AddressingMode: rts.Mode.String(), LiveMX: []analysis.MXState{{M: rtsKey.M & 1, X: rtsKey.X & 1}},
		Transfer: analysis.TransferResume, TargetEntryKind: analysis.EntryContinuation,
		Targets: targets, TargetSetClosed: true,
		Evidence: []analysis.Evidence{
			{Source: "static.stack_shape.pushed_target_rts", Confidence: analysis.ConfidenceProven, Detail: "a finite handler word is pushed immediately below this RTS"},
			evidence,
		},
	}
	fact.Normalize()
	return fact, true
}

func shadowBackwardKeys(predecessors map[decoder.DecodeKey][]decoder.DecodeKey, start decoder.DecodeKey, limit int) []decoder.DecodeKey {
	result := make([]decoder.DecodeKey, 0, limit)
	cursor := start
	for len(result) < limit {
		preds := predecessors[cursor]
		if len(preds) != 1 {
			break
		}
		cursor = preds[0]
		result = append(result, cursor)
	}
	return result
}

func shadowContinuationBeforeHandlerPHA(bank byte, graph *decoder.Graph, predecessors map[decoder.DecodeKey][]decoder.DecodeKey, handlerPHA decoder.DecodeKey) (uint32, bool) {
	pushPreds := predecessors[handlerPHA]
	if len(pushPreds) != 1 {
		return 0, false
	}
	for _, key := range shadowBackwardKeys(predecessors, pushPreds[0], 24) {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		if instruction.Mnemonic != "PHA" || key.M != 0 {
			continue
		}
		preds := predecessors[key]
		if len(preds) != 1 {
			continue
		}
		load := graph.Instructions[preds[0]]
		if load != nil && load.Instruction != nil && load.Instruction.Mnemonic == "LDA" && load.Instruction.Mode == cpu65816.IMM {
			return decoder.Address24(bank, uint16(load.Instruction.Operand+1)), true
		}
	}
	return 0, false
}

func recoverImmediateStoreTargets(image romimage.Image, bank byte, address uint16) ([]uint32, bool) {
	bankStart, err := romimage.LoROMOffset(bank, 0x8000)
	if err != nil || bankStart < 0 || bankStart >= len(image) {
		return nil, false
	}
	bankEnd := min(bankStart+0x8000, len(image))
	data := image[bankStart:bankEnd]
	var targets []uint32
	writes := 0
	for index := 0; index+2 < len(data); index++ {
		if data[index] != 0x8d || uint16(data[index+1])|uint16(data[index+2])<<8 != address {
			continue
		}
		writes++
		if index < 3 || data[index-3] != 0xa9 {
			return nil, false
		}
		raw := uint16(data[index-2]) | uint16(data[index-1])<<8
		target := raw + 1
		if target < 0x8000 {
			return nil, false
		}
		targets = appendUniqueAddresses(targets, decoder.Address24(bank, target))
	}
	return targets, writes > 0 && len(targets) > 0
}

func recoverBranchSelectedHandlerTables(image romimage.Image, bank byte, graph *decoder.Graph, predecessors map[decoder.DecodeKey][]decoder.DecodeKey, backward []decoder.DecodeKey) ([]uint32, bool) {
	commonEntry := uint16(0)
	for _, key := range backward {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil || decoded.Instruction.Mnemonic != "PHY" || key.X != 0 {
			continue
		}
		preds := predecessors[key]
		if len(preds) != 1 {
			continue
		}
		previous := graph.Instructions[preds[0]]
		if previous == nil || previous.Instruction == nil || previous.Instruction.Mnemonic == "LDY" {
			continue
		}
		commonEntry = uint16(previous.Instruction.Address)
		break
	}
	if commonEntry == 0 {
		return nil, false
	}
	bankStart, err := romimage.LoROMOffset(bank, 0x8000)
	if err != nil || bankStart < 0 || bankStart >= len(image) {
		return nil, false
	}
	data := image[bankStart:min(bankStart+0x8000, len(image))]
	var targets []uint32
	tables := 0
	for index := 0; index+5 < len(data); index++ {
		if data[index] != 0xa0 || data[index+3] != 0x82 {
			continue
		}
		pc := uint16(0x8000 + index)
		displacement := int16(uint16(data[index+4]) | uint16(data[index+5])<<8)
		branchTarget := uint16(int32(pc) + 6 + int32(displacement))
		if branchTarget != commonEntry {
			continue
		}
		base := uint16(data[index+1]) | uint16(data[index+2])<<8
		tableTargets, ok := recoverSelfDelimitedWordTable(image, bank, base, true)
		if !ok {
			return nil, false
		}
		tables++
		targets = appendUniqueAddresses(targets, tableTargets...)
	}
	return targets, tables > 0 && len(targets) > 0
}

func shadowHasImmediateWordContinuation(image romimage.Image, bank byte, phaPC uint16) bool {
	_, ok := shadowImmediateWordContinuation(image, bank, phaPC)
	return ok
}

func shadowImmediateWordContinuation(image romimage.Image, bank byte, phaPC uint16) (uint32, bool) {
	if phaPC < 4 {
		return 0, false
	}
	bytes, err := image.Slice(bank, phaPC-4, 4)
	if err != nil {
		return 0, false
	}
	// A0 imm16 / PHY or A2 imm16 / PHX immediately below the handler
	// word. Both instructions require X=0 and push the return-minus-one.
	if !((bytes[0] == 0xa0 && bytes[3] == 0x5a) || (bytes[0] == 0xa2 && bytes[3] == 0xda)) {
		return 0, false
	}
	raw := uint16(bytes[1]) | uint16(bytes[2])<<8
	value := decoder.Address24(bank, raw+1)
	return value, uint16(value) >= 0x8000
}

func recoverNestedPackedWordTable(image romimage.Image, bank byte, graph *decoder.Graph, predecessors map[decoder.DecodeKey][]decoder.DecodeKey, dynamicLoad decoder.DecodeKey) (uint16, []uint32, bool) {
	for _, possibleTAX := range predecessors[dynamicLoad] {
		decodedTAX := graph.Instructions[possibleTAX]
		if decodedTAX == nil || decodedTAX.Instruction == nil || decodedTAX.Instruction.Mnemonic != "TAX" {
			continue
		}
		outerPreds := predecessors[possibleTAX]
		if len(outerPreds) != 1 {
			continue
		}
		outer := graph.Instructions[outerPreds[0]]
		if outer == nil || outer.Instruction == nil || outer.Instruction.Mnemonic != "LDA" {
			continue
		}
		instruction := outer.Instruction
		if instruction.Mode != cpu65816.ABSX && instruction.Mode != cpu65816.ABSY && instruction.Mode != cpu65816.LONGX {
			continue
		}
		outerBank := bank
		if instruction.Mode == cpu65816.LONGX {
			outerBank = byte(instruction.Operand >> 16)
		}
		outerBase := uint16(instruction.Operand)
		if outerBank != bank || outerBase < 0x8000 {
			continue
		}
		pointers, ok := recoverSelfDelimitedWordTable(image, bank, outerBase, false)
		if !ok || len(pointers) == 0 {
			continue
		}
		innerBase := uint16(0xffff)
		for _, pointer := range pointers {
			pc := uint16(pointer)
			if pointer != 0 && byte(pointer>>16) == bank && pc < innerBase {
				innerBase = pc
			}
		}
		if innerBase == 0xffff {
			continue
		}
		targets, ok := recoverSelfDelimitedWordTable(image, bank, innerBase, true)
		if ok {
			return innerBase, targets, true
		}
	}
	return 0, nil, false
}

// recoverSelfDelimitedWordTable reads a packed same-bank word table whose
// first in-ROM target also proves the end of the table. This is a structural
// bound: every word before the boundary is part of the table, and the cursor
// must land exactly on the earliest referenced handler. PHA/RTS tables store
// handler-minus-one values, hence addOne.
func recoverSelfDelimitedWordTable(image romimage.Image, bank byte, base uint16, addOne bool) ([]uint32, bool) {
	tablePC := base
	boundary := uint16(0)
	var targets []uint32
	for len(targets) < 4096 {
		if boundary != 0 && tablePC >= boundary {
			return targets, tablePC == boundary && len(targets) > 0
		}
		if tablePC > 0xfffe {
			return nil, false
		}
		bytes, err := image.Slice(bank, tablePC, 2)
		if err != nil {
			return nil, false
		}
		raw := uint16(bytes[0]) | uint16(bytes[1])<<8
		pc := raw
		if addOne {
			pc++
		}
		if pc == 0 {
			targets = append(targets, 0)
		} else {
			if pc < 0x8000 {
				return nil, false
			}
			offset, offsetErr := romimage.LoROMOffset(bank, pc)
			if offsetErr != nil || offset < 0 || offset >= len(image) {
				return nil, false
			}
			targets = append(targets, decoder.Address24(bank, pc))
			if pc >= base && (boundary == 0 || pc < boundary) {
				boundary = pc
			}
		}
		tablePC += 2
	}
	return nil, false
}

func inferImmediateRTS(image romimage.Image, bank byte, functionStart uint16, graph *decoder.Graph, predecessors map[decoder.DecodeKey][]decoder.DecodeKey, rtsKey decoder.DecodeKey) (analysis.DispatchFact, bool) {
	preds := predecessors[rtsKey]
	if len(preds) != 1 {
		return analysis.DispatchFact{}, false
	}
	push := graph.Instructions[preds[0]]
	if push == nil || push.Instruction == nil {
		return analysis.DispatchFact{}, false
	}
	var rawTarget uint16
	switch push.Instruction.Mnemonic {
	case "PEA":
		rawTarget = uint16(push.Instruction.Operand)
	case "PHA":
		if preds[0].M != 0 {
			return analysis.DispatchFact{}, false
		}
		loadPreds := predecessors[preds[0]]
		if len(loadPreds) != 1 {
			return analysis.DispatchFact{}, false
		}
		load := graph.Instructions[loadPreds[0]]
		if load == nil || load.Instruction == nil || load.Instruction.Mnemonic != "LDA" || load.Instruction.Mode != cpu65816.IMM {
			return analysis.DispatchFact{}, false
		}
		rawTarget = uint16(load.Instruction.Operand)
	default:
		return analysis.DispatchFact{}, false
	}
	target := uint16(rawTarget + 1)
	if target < 0x8000 {
		return analysis.DispatchFact{}, false
	}
	rts := graph.Instructions[rtsKey].Instruction
	fact := analysis.DispatchFact{
		SitePC: rts.Address & 0xffffff, FunctionEntries: []uint32{decoder.Address24(bank, functionStart)},
		InstructionBytes: shadowInstructionBytes(image, bank, uint16(rts.Address), rts.Length),
		Mnemonic:         rts.Mnemonic, AddressingMode: rts.Mode.String(), LiveMX: []analysis.MXState{{M: rtsKey.M & 1, X: rtsKey.X & 1}},
		Transfer: analysis.TransferResume, TargetEntryKind: analysis.EntryContinuation,
		Targets: []uint32{decoder.Address24(bank, target)}, TargetSetClosed: true,
		Evidence: []analysis.Evidence{{Source: "static.stack_shape.immediate_rts", Confidence: analysis.ConfidenceProven, Detail: "the immediately preceding instruction sequence pushes the sole RTS target minus one"}},
	}
	fact.Normalize()
	return fact, true
}

func shadowPredecessors(graph *decoder.Graph) map[decoder.DecodeKey][]decoder.DecodeKey {
	result := make(map[decoder.DecodeKey][]decoder.DecodeKey)
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded == nil {
			continue
		}
		for _, successor := range decoded.Successors {
			result[successor] = append(result[successor], key)
		}
	}
	return result
}

func shadowPHAReachesRTS(graph *decoder.Graph, key decoder.DecodeKey) (byte, bool) {
	var sep byte
	for steps := 0; steps < 8; steps++ {
		decoded := graph.Instructions[key]
		if decoded == nil || len(decoded.Successors) != 1 {
			return 0, false
		}
		key = decoded.Successors[0]
		next := graph.Instructions[key]
		if next == nil || next.Instruction == nil {
			return 0, false
		}
		switch next.Instruction.Mnemonic {
		case "SEP":
			sep |= byte(next.Instruction.Operand)
		case "RTS":
			return sep, true
		case "NOP", "LDX", "LDY":
		default:
			return 0, false
		}
	}
	return 0, false
}

func shadowWritesAccumulator(instruction *cpu65816.Instruction) bool {
	switch instruction.Mnemonic {
	case "LDA", "PLA", "TXA", "TYA", "TSC", "TDC", "ADC", "SBC", "AND", "ORA", "EOR", "ASL", "LSR", "ROL", "ROR", "INC", "DEC", "XBA":
		return true
	}
	return false
}

func mergeShadowFact(facts map[uint32]analysis.DispatchFact, incoming analysis.DispatchFact) {
	incoming.Normalize()
	existing, found := facts[incoming.SitePC]
	if !found {
		facts[incoming.SitePC] = incoming
		return
	}
	existing.FunctionEntries = append(existing.FunctionEntries, incoming.FunctionEntries...)
	existing.LiveMX = append(existing.LiveMX, incoming.LiveMX...)
	existing.Evidence = append(existing.Evidence, incoming.Evidence...)
	existing.UnknownFields = append(existing.UnknownFields, incoming.UnknownFields...)
	if len(existing.Targets) == 0 {
		existing.Targets = append([]uint32(nil), incoming.Targets...)
	} else {
		existing.Targets = appendUniqueAddresses(existing.Targets, incoming.Targets...)
	}
	if len(existing.TargetCandidates) == 0 {
		existing.TargetCandidates = append([]uint32(nil), incoming.TargetCandidates...)
	} else {
		existing.TargetCandidates = appendUniqueAddresses(existing.TargetCandidates, incoming.TargetCandidates...)
	}
	existing.TargetSetClosed = existing.TargetSetClosed && incoming.TargetSetClosed
	if existing.Transfer != incoming.Transfer {
		existing.UnknownFields = append(existing.UnknownFields, "transfer")
	}
	if existing.TargetEntryKind != incoming.TargetEntryKind {
		existing.UnknownFields = append(existing.UnknownFields, "target_entry_kind")
	}
	if existing.IndexRegister != incoming.IndexRegister {
		existing.UnknownFields = append(existing.UnknownFields, "index_register")
	}
	if !equalAddressSlices(existing.TableBases, incoming.TableBases) {
		existing.UnknownFields = append(existing.UnknownFields, "table_bases")
	}
	if !equalShadowOptionalAddress(existing.ReturnPC, incoming.ReturnPC) {
		existing.UnknownFields = append(existing.UnknownFields, "return_pc")
	}
	if existing.SEPMask != incoming.SEPMask {
		existing.UnknownFields = append(existing.UnknownFields, "sep_mask")
	}
	existing.Normalize()
	facts[incoming.SitePC] = existing
}

func appendUniqueAddresses(values []uint32, added ...uint32) []uint32 {
	seen := make(map[uint32]struct{}, len(values)+len(added))
	for _, value := range values {
		seen[value&0xffffff] = struct{}{}
	}
	for _, value := range added {
		value &= 0xffffff
		if _, found := seen[value]; found {
			continue
		}
		seen[value] = struct{}{}
		values = append(values, value)
	}
	return values
}

func equalAddressSlices(left, right []uint32) bool {
	if len(left) != len(right) {
		return false
	}
	for index := range left {
		if left[index]&0xffffff != right[index]&0xffffff {
			return false
		}
	}
	return true
}

func equalShadowOptionalAddress(left, right *uint32) bool {
	if left == nil || right == nil {
		return left == nil && right == nil
	}
	return *left&0xffffff == *right&0xffffff
}

func dedupeShadowCallers(values []ShadowCaller) []ShadowCaller {
	if len(values) < 2 {
		return values
	}
	result := values[:1]
	for _, value := range values[1:] {
		if value != result[len(result)-1] {
			result = append(result, value)
		}
	}
	return result
}

func decodeShadowInstruction(image romimage.Image, bank byte, pc uint16, m, x uint8) (*cpu65816.Instruction, error) {
	offset, err := romimage.LoROMOffset(bank, pc)
	if err != nil || offset < 0 || offset >= len(image) {
		if err == nil {
			err = fmt.Errorf("ROM offset %d outside %d-byte image", offset, len(image))
		}
		return nil, err
	}
	instruction, err := cpu65816.Decode(image, offset, pc, bank, m&1, x&1)
	if err != nil {
		return nil, err
	}
	if instruction == nil {
		return nil, fmt.Errorf("unknown opcode $%02X", image[offset])
	}
	return instruction, nil
}

func shadowInstructionBytes(image romimage.Image, bank byte, pc uint16, length uint8) string {
	bytes, err := image.Slice(bank, pc, int(length))
	if err != nil {
		return ""
	}
	parts := make([]string, len(bytes))
	for index, value := range bytes {
		parts[index] = fmt.Sprintf("%02X", value)
	}
	return strings.Join(parts, " ")
}

func transferForInstruction(instruction *cpu65816.Instruction, hasReturn bool) analysis.TransferKind {
	if instruction.Mnemonic == "JSR" || instruction.Mnemonic == "JSL" {
		return analysis.TransferCall
	}
	if instruction.Mnemonic == "RTS" || instruction.Mnemonic == "RTL" {
		return analysis.TransferResume
	}
	if instruction.Mnemonic == "PHA" && hasReturn {
		return analysis.TransferCall
	}
	return analysis.TransferTail
}

func authoredTransferForInstruction(instruction *cpu65816.Instruction, dispatch config.IndirectDispatch) analysis.TransferKind {
	switch dispatch.Transfer {
	case config.IndirectTransferCall:
		return analysis.TransferCall
	case config.IndirectTransferTail:
		return analysis.TransferTail
	default:
		return transferForInstruction(instruction, dispatch.ReturnPC != nil)
	}
}

func normalizeTableBases(bank byte, bases []uint16) []uint32 {
	result := make([]uint32, len(bases))
	for index, base := range bases {
		result[index] = decoder.Address24(bank, base)
	}
	return result
}

func normalizeShadowTargets(bank byte, targets []uint32, kind string) []uint32 {
	result := make([]uint32, len(targets))
	for index, target := range targets {
		if target == 0 {
			continue
		}
		if target <= 0xffff || kind != "long" {
			target = decoder.Address24(bank, uint16(target))
		}
		result[index] = target & 0xffffff
	}
	return result
}

func WriteShadowReport(output io.Writer, report ShadowReport, format string, verbose bool) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetEscapeHTML(false)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		writeShadowText(output, report, verbose)
		return nil
	default:
		return fmt.Errorf("unknown analysis format %q (want text or json)", format)
	}
}

// FormatShadowConflicts produces the compact, actionable subset used by the
// experimental regeneration gate. The complete report remains available from
// `v2regen analyze --verbose` or JSON output.
func FormatShadowConflicts(report ShadowReport) string {
	var lines []string
	for _, comparison := range report.Comparisons {
		if comparison.Status != analysis.ComparisonConflict {
			continue
		}
		instructionBytes, mnemonic := "", ""
		if comparison.Authored != nil {
			instructionBytes, mnemonic = comparison.Authored.InstructionBytes, comparison.Authored.Mnemonic
		} else if comparison.Inferred != nil {
			instructionBytes, mnemonic = comparison.Inferred.InstructionBytes, comparison.Inferred.Mnemonic
		}
		heading := strings.TrimSpace(strings.Join([]string{shadowAddress(comparison.SitePC), instructionBytes, mnemonic}, " "))
		if len(comparison.Differences) == 0 {
			lines = append(lines, heading+": authored and inferred semantics differ")
			continue
		}
		lines = append(lines, heading+": "+strings.Join(comparison.Differences, "; "))
	}
	return strings.Join(lines, "\n")
}

func writeShadowText(output io.Writer, report ShadowReport, verbose bool) {
	summary := report.Summary
	fmt.Fprintf(output, "shadow analysis v%d: no-write, mapper=%s, ROM sha256=%s\n", report.Version, report.ROM.Mapper, report.ROM.SHA256)
	fmt.Fprintf(output, "dispatch facts: %d authored, %d inferred; exact=%d compatible-guards=%d partial=%d conflicts=%d authored-only=%d automatic=%d garbage-only=%d\n",
		summary.AuthoredFacts, summary.InferredFacts, summary.ExactMatches, summary.Compatible, summary.PartialMatches, summary.Conflicts, summary.AuthoredOnly, summary.Automatic, summary.GarbageOnly)
	fmt.Fprintf(output, "read-only variant discovery: %d configured -> %d analyzed variants in %d passes\n",
		summary.InitialVariants, summary.FinalVariants, summary.VariantPasses)
	fmt.Fprintf(output, "shadow-root unresolved dynamic edges: %d raw emissions -> %d unique source sites; decode issues=%d\n",
		summary.RawUnresolvedEmissions, summary.UniqueUnresolvedSites, summary.DecodeIssues)
	for _, comparison := range report.Comparisons {
		if !verbose && comparison.Status != analysis.ComparisonConflict && comparison.Status != analysis.ComparisonPartial {
			continue
		}
		fmt.Fprintf(output, "[%s] %s", strings.ToUpper(string(comparison.Status)), shadowAddress(comparison.SitePC))
		if comparison.Authored != nil {
			fmt.Fprintf(output, " %s %s", comparison.Authored.InstructionBytes, comparison.Authored.Mnemonic)
		} else if comparison.Inferred != nil {
			fmt.Fprintf(output, " %s %s", comparison.Inferred.InstructionBytes, comparison.Inferred.Mnemonic)
		}
		fmt.Fprintln(output)
		if comparison.Authored != nil {
			fmt.Fprintf(output, "  authored: %s -> %s targets=%d tables=%s return=%s sep=$%02X\n",
				comparison.Authored.Transfer, comparison.Authored.TargetEntryKind, len(comparison.Authored.Targets), shadowAddresses(comparison.Authored.TableBases), shadowOptionalAddress(comparison.Authored.ReturnPC), comparison.Authored.SEPMask)
		}
		if comparison.Inferred != nil {
			fmt.Fprintf(output, "  inferred: %s -> %s targets_closed=%t tables=%s return=%s sep=$%02X live_mx=%s\n",
				comparison.Inferred.Transfer, comparison.Inferred.TargetEntryKind, comparison.Inferred.TargetSetClosed, shadowAddresses(comparison.Inferred.TableBases), shadowOptionalAddress(comparison.Inferred.ReturnPC), comparison.Inferred.SEPMask, shadowMX(comparison.Inferred.LiveMX))
		}
		for _, difference := range comparison.Differences {
			fmt.Fprintf(output, "  reason: %s\n", difference)
		}
	}
	if verbose {
		for _, unresolved := range report.Unresolved {
			fmt.Fprintf(output, "[UNRESOLVED] %s %s %s %s operand=$%X reason=%s callers=%d\n",
				shadowAddress(unresolved.SitePC), unresolved.InstructionBytes, unresolved.Mnemonic, unresolved.AddressingMode, unresolved.Operand, unresolved.Reason, len(unresolved.Callers))
			for _, caller := range unresolved.Callers {
				fmt.Fprintf(output, "  caller=%s M%dX%d reachability=%s\n", shadowAddress(caller.FunctionEntry), caller.LiveMX.M, caller.LiveMX.X, unresolved.Reachability)
			}
		}
		for _, issue := range report.DecodeIssues {
			fmt.Fprintf(output, "[DECODE-ISSUE] %s M%dX%d %s\n", shadowAddress(issue.FunctionEntry), issue.EntryMX.M, issue.EntryMX.X, issue.Error)
		}
	} else {
		fmt.Fprintln(output, "use --verbose for authored-only/automatic facts, deduplicated callers, and decode issues; --format json emits the complete report")
	}
}

func shadowAddress(address uint32) string {
	return fmt.Sprintf("$%02X:%04X", byte(address>>16), uint16(address))
}

func shadowAddresses(addresses []uint32) string {
	if len(addresses) == 0 {
		return "none"
	}
	parts := make([]string, len(addresses))
	for index, address := range addresses {
		parts[index] = shadowAddress(address)
	}
	return strings.Join(parts, ",")
}

func shadowOptionalAddress(address *uint32) string {
	if address == nil {
		return "none"
	}
	return shadowAddress(*address)
}

func shadowMX(states []analysis.MXState) string {
	if len(states) == 0 {
		return "unknown"
	}
	parts := make([]string, len(states))
	for index, state := range states {
		parts[index] = fmt.Sprintf("M%dX%d", state.M, state.X)
	}
	return strings.Join(parts, ",")
}
