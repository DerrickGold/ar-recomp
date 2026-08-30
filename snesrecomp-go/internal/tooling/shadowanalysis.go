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

const shadowReportVersion = 10

const (
	shadowUnresolvedGeneric              = "generic_dynamic_target"
	shadowUnresolvedTaggedStreamDispatch = "tagged_stream_handler_dispatch"
	shadowPriorityNormal                 = "normal"
	shadowPriorityLikelyBlocker          = "likely_bringup_blocker"
	shadowTableOwnershipConfirmed        = "confirmed_data"
	shadowTableOwnershipCandidate        = "candidate_data"
	shadowRuntimeObservedTrappedMissing  = "observed_trapped_missing_body"
	shadowRuntimeObservedMissing         = "observed_missing_body"
	shadowRuntimeObservedTrapped         = "observed_trapped"
	shadowRuntimeObservedResolved        = "observed_resolved"
	shadowRuntimeUnobserved              = "unobserved"
)

type ShadowAnalysisOptions struct {
	ROMPath              string
	CFGDir               string
	Jobs                 int
	OnlyBank             *byte
	DispatchAnalysisPath string
}

type ShadowROM struct {
	SHA256 string `json:"sha256"`
	Size   int    `json:"size"`
	Mapper string `json:"mapper"`
}

type ShadowSummary struct {
	analysis.ComparisonSummary
	InitialVariants                int `json:"initial_variants"`
	FinalVariants                  int `json:"final_variants"`
	VariantPasses                  int `json:"variant_passes"`
	ConfirmedTableSpans            int `json:"confirmed_table_spans"`
	CandidateTableSpans            int `json:"candidate_table_spans"`
	ObservedUnresolvedSites        int `json:"observed_unresolved_sites"`
	UnobservedUnresolvedSites      int `json:"unobserved_unresolved_sites"`
	RawUnresolvedEmissions         int `json:"raw_unresolved_emissions"`
	UniqueUnresolvedSites          int `json:"unique_unresolved_sites"`
	LikelyBlockingUnresolvedSites  int `json:"likely_blocking_unresolved_sites"`
	DecodeIssues                   int `json:"decode_issues"`
	DispatchCodeIslands            int `json:"dispatch_code_islands"`
	LandingCandidates              int `json:"landing_candidates"`
	ProbableLandingCandidates      int `json:"probable_landing_candidates"`
	SpeculativeLandingCandidates   int `json:"speculative_landing_candidates"`
	LandingCandidateTableConflicts int `json:"landing_candidate_table_conflicts"`
	TableFirstTargets              int `json:"table_first_targets"`
	TableFirstNewLandings          int `json:"table_first_new_landings"`
	TableFirstInternalTargets      int `json:"table_first_internal_targets"`
	ProbableTableFirstTargets      int `json:"probable_table_first_targets"`
	SpeculativeTableFirstTargets   int `json:"speculative_table_first_targets"`
	TableFirstPointerSeeds         int `json:"table_first_pointer_seeds"`
	TableFirstAbsoluteSeeds        int `json:"table_first_absolute_seeds"`
	TableFirstBaseOffsetSeeds      int `json:"table_first_base_offset_seeds"`
	TableFirstBaseEvidenceBases    int `json:"table_first_base_evidence_bases"`
	TableFirstBaseEvidenceSites    int `json:"table_first_base_evidence_sites"`
	TableFirstAbsoluteTargets      int `json:"table_first_absolute_targets"`
	TableFirstBaseOffsetTargets    int `json:"table_first_base_offset_targets"`
	TableFirstPostTerminatorSeeds  int `json:"table_first_post_terminator_seeds"`
	TableFirstPointerWindowSeeds   int `json:"table_first_pointer_window_seeds"`
	TableFirstRejectedSeeds        int `json:"table_first_rejected_seeds"`
}

type ShadowCaller struct {
	FunctionEntry uint32           `json:"function_entry"`
	LiveMX        analysis.MXState `json:"live_mx"`
}

type ShadowUnresolvedSite struct {
	SitePC                      uint32                       `json:"site_pc"`
	InstructionBytes            string                       `json:"instruction_bytes"`
	Mnemonic                    string                       `json:"mnemonic"`
	AddressingMode              string                       `json:"addressing_mode"`
	Operand                     uint32                       `json:"operand"`
	Reason                      string                       `json:"reason"`
	Reachability                string                       `json:"reachability"`
	Classification              string                       `json:"classification"`
	Priority                    string                       `json:"priority"`
	StreamDispatch              *ShadowStreamDispatchPattern `json:"stream_dispatch,omitempty"`
	StructuralHandlerCandidates []uint32                     `json:"structural_handler_candidates,omitempty"`
	Callers                     []ShadowCaller               `json:"callers"`
	RuntimeStatus               string                       `json:"runtime_status,omitempty"`
	RuntimeObservationCount     uint64                       `json:"runtime_observation_count,omitempty"`
	RuntimeObservations         []DispatchObservation        `json:"runtime_observations,omitempty"`
}

// ShadowStreamDispatchPattern records structural evidence for a common script
// interpreter without claiming that the stream-derived target set is closed.
// Every PC is a ROM address in the dispatch site's bank.
type ShadowStreamDispatchPattern struct {
	InterpreterEntryPC  uint32 `json:"interpreter_entry_pc,omitempty"`
	StreamPointerLoadPC uint32 `json:"stream_pointer_load_pc"`
	StreamPointer       uint16 `json:"stream_pointer"`
	StreamWordLoadPC    uint32 `json:"stream_word_load_pc"`
	SignTestPC          uint32 `json:"sign_test_pc"`
	TargetSlotStorePC   uint32 `json:"target_slot_store_pc"`
	TargetSlot          uint8  `json:"target_slot"`
	TrampolineCallPC    uint32 `json:"trampoline_call_pc"`
}

type ShadowDecodeIssue struct {
	FunctionEntry uint32           `json:"function_entry"`
	EntryMX       analysis.MXState `json:"entry_mx"`
	Error         string           `json:"error"`
}

type ShadowTableSpan struct {
	SitePC       uint32              `json:"site_pc"`
	StartPC      uint32              `json:"start_pc"`
	EndExclusive uint32              `json:"end_exclusive"`
	EntryBytes   uint8               `json:"entry_bytes"`
	EntryCount   int                 `json:"entry_count"`
	Ownership    string              `json:"ownership"`
	Confidence   analysis.Confidence `json:"confidence"`
	Provenance   []string            `json:"provenance"`
}

// ShadowDispatchCodeIsland is a review-only candidate handler found in a CFG
// hole between two known targets of the same computed dispatch. The bytes are
// not promoted into generated code: the finding names a place where ordinary
// decode ownership stopped at an unconditional transfer but sequential decode
// reaches a return before the next claimed block.
type ShadowDispatchCodeIsland struct {
	SitePC           uint32              `json:"site_pc"`
	PreviousTargetPC uint32              `json:"previous_target_pc"`
	NextTargetPC     uint32              `json:"next_target_pc"`
	CandidateEntryPC uint32              `json:"candidate_entry_pc"`
	EndExclusive     uint32              `json:"end_exclusive"`
	LiveMX           []analysis.MXState  `json:"live_mx"`
	PrecededBy       string              `json:"preceded_by"`
	Confidence       analysis.Confidence `json:"confidence"`
	Reason           string              `json:"reason"`
}

type ShadowReport struct {
	Version              int                         `json:"version"`
	Mode                 string                      `json:"mode"`
	NoWrite              bool                        `json:"no_write"`
	ROM                  ShadowROM                   `json:"rom"`
	Summary              ShadowSummary               `json:"summary"`
	Comparisons          []analysis.Comparison       `json:"comparisons"`
	TableSpans           []ShadowTableSpan           `json:"table_spans,omitempty"`
	DispatchCodeIslands  []ShadowDispatchCodeIsland  `json:"dispatch_code_islands,omitempty"`
	LandingCandidates    []ShadowLandingCandidate    `json:"landing_candidates,omitempty"`
	TableFirstTargets    []ShadowTableFirstTarget    `json:"table_first_targets,omitempty"`
	TableFirstRejections []ShadowTableFirstRejection `json:"table_first_rejections,omitempty"`
	EntryRecovery        ShadowEntryRecoveryReport   `json:"entry_recovery"`
	EntryAblation        ShadowEntryAblationReport   `json:"entry_ablation"`
	DispatchEvidence     *ShadowDispatchEvidence     `json:"dispatch_evidence,omitempty"`
	Unresolved           []ShadowUnresolvedSite      `json:"unresolved_sites,omitempty"`
	DecodeIssues         []ShadowDecodeIssue         `json:"decode_issues,omitempty"`
	Limitations          []string                    `json:"limitations,omitempty"`
}

type ShadowDispatchEvidence struct {
	Version      int    `json:"version"`
	ROMHash      string `json:"rom_sha256,omitempty"`
	TraceHash    string `json:"trace_sha256"`
	Provenance   string `json:"provenance"`
	Overflow     bool   `json:"overflow"`
	Observations int    `json:"observations"`
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
	entry          decoder.Variant
	facts          []analysis.DispatchFact
	unresolved     []decoder.UnresolvedIndirect
	demands        map[decoder.Variant]struct{}
	demandEvidence map[decoder.Variant][]string
	seedReach      []shadowContinuationReach
	spans          []shadowDecodedSpan
	instructions   []shadowDecodedInstruction
	issue          *ShadowDecodeIssue
}

type shadowDecodedSpan struct {
	PC            uint32
	FunctionEntry uint32
	Length        uint8
}

type shadowDecodedInstruction struct {
	PC            uint32
	FunctionEntry uint32
	M, X          uint8
	Instruction   cpu65816.Instruction
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
	inferred, rawUnresolved, unresolved, issues, inferenceStats, decodeResults, err := inferShadowFacts(image, banks, allRegions, calleeExitMX, options.Jobs)
	if err != nil {
		return ShadowReport{}, err
	}
	comparisons, comparisonSummary := analysis.CompareDispatchFacts(authored, inferred)
	tableSpans := collectShadowTableSpans(comparisons)
	dispatchCodeIslands := collectShadowDispatchCodeIslands(image, comparisons, decodeResults, allRegions, tableSpans)
	landingCandidates := collectShadowLandingCandidates(image, banks, decodeResults, allRegions, tableSpans, dispatchCodeIslands, calleeExitMX)
	probableLandingCandidates, speculativeLandingCandidates, landingCandidateTableConflicts := countShadowLandingConfidence(landingCandidates)
	entryRecovery, err := analyzeShadowEntryRecovery(image, banks, allRegions, calleeExitMX, options.Jobs)
	if err != nil {
		return ShadowReport{}, err
	}
	annotateShadowEntryPointerClusters(image, decodeResults, tableSpans, &entryRecovery)
	entryAblation := analyzeShadowEntryAblation(image, banks, decodeResults)
	tableFirstTargets, tableFirstRejections, tableFirstStats := collectShadowTableFirstTargets(image, banks, entryRecovery.Entries, landingCandidates, decodeResults, allRegions, tableSpans, calleeExitMX)
	probableTableFirstTargets, speculativeTableFirstTargets := countShadowTableFirstConfidence(tableFirstTargets)
	confirmedTableSpans, candidateTableSpans := countShadowTableSpans(tableSpans)
	hash := sha256.Sum256(image)
	report := ShadowReport{
		Version: shadowReportVersion,
		Mode:    "compare_authored",
		NoWrite: true,
		ROM:     ShadowROM{SHA256: hex.EncodeToString(hash[:]), Size: len(image), Mapper: "lorom"},
		Summary: ShadowSummary{
			ComparisonSummary:              comparisonSummary,
			InitialVariants:                inferenceStats.initialVariants,
			FinalVariants:                  inferenceStats.finalVariants,
			VariantPasses:                  inferenceStats.passes,
			ConfirmedTableSpans:            confirmedTableSpans,
			CandidateTableSpans:            candidateTableSpans,
			RawUnresolvedEmissions:         rawUnresolved,
			UniqueUnresolvedSites:          len(unresolved),
			LikelyBlockingUnresolvedSites:  countLikelyBlockingUnresolved(unresolved),
			DecodeIssues:                   len(issues),
			DispatchCodeIslands:            len(dispatchCodeIslands),
			LandingCandidates:              len(landingCandidates),
			ProbableLandingCandidates:      probableLandingCandidates,
			SpeculativeLandingCandidates:   speculativeLandingCandidates,
			LandingCandidateTableConflicts: landingCandidateTableConflicts,
			TableFirstTargets:              len(tableFirstTargets),
			TableFirstNewLandings:          tableFirstStats.postTerminatorMatches + tableFirstStats.pointerWindowMatches,
			TableFirstInternalTargets:      tableFirstStats.internalMatches,
			ProbableTableFirstTargets:      probableTableFirstTargets,
			SpeculativeTableFirstTargets:   speculativeTableFirstTargets,
			TableFirstPointerSeeds:         tableFirstStats.pointerSeeds,
			TableFirstAbsoluteSeeds:        tableFirstStats.absoluteSeeds,
			TableFirstBaseOffsetSeeds:      tableFirstStats.baseOffsetSeeds,
			TableFirstBaseEvidenceBases:    tableFirstStats.baseEvidenceBases,
			TableFirstBaseEvidenceSites:    tableFirstStats.baseEvidenceSites,
			TableFirstAbsoluteTargets:      tableFirstStats.absoluteTargets,
			TableFirstBaseOffsetTargets:    tableFirstStats.baseOffsetTargets,
			TableFirstPostTerminatorSeeds:  tableFirstStats.postTerminatorMatches,
			TableFirstPointerWindowSeeds:   tableFirstStats.pointerWindowMatches,
			TableFirstRejectedSeeds:        tableFirstStats.rejectedSeeds,
		},
		Comparisons:          comparisons,
		TableSpans:           tableSpans,
		DispatchCodeIslands:  dispatchCodeIslands,
		LandingCandidates:    landingCandidates,
		TableFirstTargets:    tableFirstTargets,
		TableFirstRejections: tableFirstRejections,
		EntryRecovery:        entryRecovery,
		EntryAblation:        entryAblation,
		Unresolved:           unresolved,
		DecodeIssues:         issues,
		Limitations: []string{
			"configured func entries, entry M/X states, and exit_mx_at routes seed the read-only call-target variant fixed point",
			"an open table is a partial match until value/bounds provenance proves its complete target set",
			"a compatible guard proves safe coverage of inferred continuations, not that every guarded edge executes",
			"an authored-only result means unproven, not disproven or runtime-reachable",
			"tagged-stream handler classification and structural handler candidates are heuristic triage evidence, not a closed target set",
			"dispatch code-island findings are probable review hints; they never create entry points or alter regeneration",
			"boundary landing-sweep findings are probable or speculative review hints seeded only after confirmed flow terminators; they never create entry points or alter regeneration",
			"table-first targets require a tightly authored-entry-anchored ROM pointer window plus an existing instruction boundary or bounded stack-balanced decode; they remain review-only and do not prove runtime table bounds",
			"vector-root entry recovery is a conservative lower bound; not_recovered does not mean unreachable or invalid code",
			"entry ablation is dependency-graph evidence collected with authored boundaries present; its inclusion-minimal root set is report-only until generated-region equivalence is validated",
			"the current ROM reader is explicitly LoROM; mapper generalization is a later milestone",
		},
	}
	if strings.TrimSpace(options.DispatchAnalysisPath) != "" {
		evidence, loadErr := LoadDispatchCensusFile(options.DispatchAnalysisPath)
		if loadErr != nil {
			return ShadowReport{}, loadErr
		}
		if applyErr := applyShadowDispatchEvidence(&report, evidence); applyErr != nil {
			return ShadowReport{}, applyErr
		}
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
				TableEntryBytes: shadowTableEntryBytes(dispatchKind, len(dispatch.TableBases)),
				SEPMask:         dispatch.SEPMask,
				Evidence:        []analysis.Evidence{{Source: "authored.config.indirect_dispatch", Confidence: analysis.ConfidenceAuthored}},
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

func inferShadowFacts(image romimage.Image, banks []shadowBank, regions []decoder.DataRegion, calleeExitMX map[decoder.Variant]decoder.MX, jobs int) ([]analysis.DispatchFact, int, []ShadowUnresolvedSite, []ShadowDecodeIssue, shadowInferenceStats, []shadowDecodeResult, error) {
	results, stats, err := discoverShadowDecodeResults(image, banks, regions, calleeExitMX, jobs)
	if err != nil {
		return nil, 0, nil, nil, stats, nil, err
	}
	facts, rawUnresolved, unresolved, issues := summarizeShadowResults(image, results)
	facts = inferShadowContinuationFacts(image, banks, regions, calleeExitMX, facts, collectShadowContinuationReaches(results))
	return facts, rawUnresolved, unresolved, issues, stats, results, nil
}

func discoverShadowDecodeResults(image romimage.Image, banks []shadowBank, regions []decoder.DataRegion, calleeExitMX map[decoder.Variant]decoder.MX, jobs int) ([]shadowDecodeResult, shadowInferenceStats, error) {
	entries := make(map[byte][]config.Entry, len(banks))
	bankConfigs := make(map[byte]*config.Config, len(banks))
	stats := shadowInferenceStats{}
	for _, bank := range banks {
		entries[bank.ID] = append([]config.Entry(nil), bank.Config.Entries...)
		bankConfigs[bank.ID] = bank.Config
		stats.initialVariants += len(bank.Config.Entries)
		if bank.ID == 0 && bank.Config.AutoVectors {
			entries[bank.ID] = appendShadowAutoVectorEntries(image, entries[bank.ID])
		}
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
			return nil, stats, fmt.Errorf("shadow variant discovery did not converge in %d passes", passLimit)
		}
	}
	for _, bankEntries := range entries {
		stats.finalVariants += len(bankEntries)
	}

	return results, stats, nil
}

func appendShadowAutoVectorEntries(image romimage.Image, entries []config.Entry) []config.Entry {
	if len(image) < 0x8000 {
		return entries
	}
	starts := make(map[uint16]struct{}, len(entries))
	for _, entry := range entries {
		starts[entry.Start] = struct{}{}
	}
	read := func(offset int) uint16 {
		return uint16(image[offset]) | uint16(image[offset+1])<<8
	}
	seeds := []struct {
		name string
		pc   uint16
	}{
		{name: "I_RESET", pc: read(0x7ffc)},
		{name: "I_NMI", pc: read(0x7fea)},
		{name: "I_IRQ", pc: read(0x7fee)},
	}
	for _, seed := range seeds {
		if seed.pc == 0 || seed.pc == 0xffff {
			continue
		}
		if _, found := starts[seed.pc]; found {
			continue
		}
		entries = append(entries, config.Entry{
			Name: seed.name, Start: seed.pc,
			EntryMX: config.MX{M: 1, X: 1},
		})
		starts[seed.pc] = struct{}{}
	}
	return entries
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
				entryVariant := decoder.Variant{
					Address: decoder.Address24(item.bank.ID, item.entry.Start),
					M:       item.entry.EntryMX.M & 1, X: item.entry.EntryMX.X & 1,
				}
				options := decoder.Options{
					End: item.entry.End, DataRegions: regions,
					HLEDispatch:     item.bank.Config.HLEDispatch,
					CalleeExitMX:    calleeExitMX,
					SiblingEntryPCs: item.siblings,
				}
				graph, err := decoder.DecodeFunction(image, item.bank.ID, item.entry.Start, item.entry.EntryMX.M, item.entry.EntryMX.X, options)
				if err != nil {
					output <- shadowDecodeResult{entry: entryVariant, issue: &ShadowDecodeIssue{
						FunctionEntry: decoder.Address24(item.bank.ID, item.entry.Start),
						EntryMX:       analysis.MXState{M: item.entry.EntryMX.M & 1, X: item.entry.EntryMX.X & 1}, Error: err.Error(),
					}}
					continue
				}
				facts := inferredFactsFromGraph(image, item.bank.ID, item.entry.Start, graph)
				demandEvidence := discoverShadowDemandEvidence(item.bank.ID, graph, item.siblings)
				output <- shadowDecodeResult{
					entry: entryVariant,
					facts: facts, unresolved: graph.UnresolvedIndirects,
					demands:        shadowDemandEvidenceSet(demandEvidence),
					demandEvidence: demandEvidence,
					seedReach:      discoverShadowContinuationReaches(item.bank.ID, graph),
					spans:          shadowDecodedSpans(item.bank.ID, item.entry.Start, graph),
					instructions:   shadowDecodedInstructions(item.bank.ID, item.entry.Start, graph),
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

func shadowDecodedInstructions(bank byte, functionStart uint16, graph *decoder.Graph) []shadowDecodedInstruction {
	result := make([]shadowDecodedInstruction, 0, len(graph.Order))
	entry := decoder.Address24(bank, functionStart)
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		result = append(result, shadowDecodedInstruction{
			PC: decoded.Key.PC & 0xffffff, FunctionEntry: entry,
			M: decoded.Key.M & 1, X: decoded.Key.X & 1,
			Instruction: *decoded.Instruction,
		})
	}
	return result
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
	return shadowDemandEvidenceSet(discoverShadowDemandEvidence(bank, graph, siblingStarts))
}

func shadowDemandEvidenceSet(evidence map[decoder.Variant][]string) map[decoder.Variant]struct{} {
	result := make(map[decoder.Variant]struct{}, len(evidence))
	for variant := range evidence {
		result[variant] = struct{}{}
	}
	return result
}

func discoverShadowDemandEvidence(bank byte, graph *decoder.Graph, siblingStarts map[uint16]struct{}) map[decoder.Variant][]string {
	result := make(map[decoder.Variant][]string)
	one := func(address uint32, m, x uint8, kind string) {
		variant := decoder.Variant{Address: address & 0xffffff, M: m & 1, X: x & 1}
		result[variant] = appendUniqueShadowString(result[variant], kind)
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
					one(target, 1, 1, "static_computed_dispatch")
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
					one(target, m, x, "static_computed_dispatch")
				}
			}
			continue
		}
		switch {
		case instruction.Mnemonic == "JSR" && instruction.Mode == cpu65816.ABS:
			one(decoder.Address24(bank, uint16(instruction.Operand)), instruction.M, instruction.X, "direct_jsr")
		case instruction.Mnemonic == "JSL":
			one(instruction.Operand, instruction.M, instruction.X, "direct_jsl")
		case instruction.Mnemonic == "JMP" && instruction.Mode == cpu65816.LONG:
			one(instruction.Operand, instruction.M, instruction.X, "direct_long_jump")
		}
		for _, successor := range decoded.Successors {
			if byte(successor.PC>>16) != bank {
				continue
			}
			if _, known := siblingStarts[uint16(successor.PC)]; !known {
				continue
			}
			if graph.Instructions[successor] != nil {
				continue
			}
			kind := "sibling_boundary_edge"
			if instruction.Mnemonic == "JMP" && instruction.Mode == cpu65816.ABS {
				kind = "direct_tail_jump"
			}
			one(successor.PC, successor.M, successor.X, kind)
		}
	}
	for variant := range result {
		sort.Strings(result[variant])
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
	instructionOwners := make(map[uint32][]shadowDecodedSpan)
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
			instructionOwners[span.PC&0xffffff] = append(instructionOwners[span.PC&0xffffff], span)
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
					Classification: shadowUnresolvedGeneric, Priority: shadowPriorityNormal,
				}
				if instruction != nil {
					record.InstructionBytes = shadowInstructionBytes(image, byte(site>>16), uint16(site), instruction.Length)
				}
				unresolvedMap[site] = record
			}
			record.Callers = append(record.Callers, ShadowCaller{FunctionEntry: unresolved.FunctionEntry & 0xffffff, LiveMX: analysis.MXState{M: unresolved.EntryM & 1, X: unresolved.EntryX & 1}})
		}
	}
	for _, record := range unresolvedMap {
		classifyShadowUnresolved(image, record)
		if record.StreamDispatch == nil {
			continue
		}
		owners := instructionOwners[record.StreamDispatch.StreamPointerLoadPC&0xffffff]
		if len(owners) == 0 {
			continue
		}
		interpreterEntry := owners[0].FunctionEntry & 0xffffff
		for _, owner := range owners[1:] {
			if candidate := owner.FunctionEntry & 0xffffff; candidate < interpreterEntry {
				interpreterEntry = candidate
			}
		}
		record.StreamDispatch.InterpreterEntryPC = interpreterEntry
		record.StructuralHandlerCandidates = removeShadowAddress(record.StructuralHandlerCandidates, interpreterEntry)
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

func removeShadowAddress(addresses []uint32, remove uint32) []uint32 {
	result := addresses[:0]
	for _, address := range addresses {
		if address&0xffffff != remove&0xffffff {
			result = append(result, address)
		}
	}
	return result
}

func countLikelyBlockingUnresolved(unresolved []ShadowUnresolvedSite) int {
	count := 0
	for _, site := range unresolved {
		if site.Priority == shadowPriorityLikelyBlocker {
			count++
		}
	}
	return count
}

func collectShadowDispatchCodeIslands(image romimage.Image, comparisons []analysis.Comparison, results []shadowDecodeResult, regions []decoder.DataRegion, tableSpans []ShadowTableSpan) []ShadowDispatchCodeIsland {
	spansByEntry := make(map[uint32][]shadowDecodedSpan)
	instructionsByEntry := make(map[uint32][]shadowDecodedInstruction)
	globalOwned := make(map[uint32]struct{})
	for _, result := range results {
		for _, span := range result.spans {
			entry := span.FunctionEntry & 0xffffff
			spansByEntry[entry] = append(spansByEntry[entry], span)
			for offset := uint8(0); offset < span.Length; offset++ {
				globalOwned[(span.PC&0xff0000)|uint32(uint16(span.PC)+uint16(offset))] = struct{}{}
			}
		}
		for _, instruction := range result.instructions {
			entry := instruction.FunctionEntry & 0xffffff
			instructionsByEntry[entry] = append(instructionsByEntry[entry], instruction)
		}
	}
	type islandKey struct {
		site, candidate uint32
	}
	findings := make(map[islandKey]ShadowDispatchCodeIsland)
	for _, comparison := range comparisons {
		facts := []*analysis.DispatchFact{comparison.Authored, comparison.Inferred}
		for _, fact := range facts {
			if fact == nil || !fact.TargetSetClosed || fact.TargetEntryKind != analysis.EntryComputed {
				continue
			}
			targets := shadowUniqueSameBankTargets(fact.SitePC, fact.Targets)
			for index := 0; index+1 < len(targets); index++ {
				previous, next := targets[index], targets[index+1]
				if previous >= next || len(spansByEntry[previous]) == 0 {
					continue
				}
				for _, finding := range shadowCodeIslandsBetweenTargets(image, fact.SitePC, previous, next, spansByEntry, instructionsByEntry, globalOwned, regions, tableSpans) {
					key := islandKey{site: finding.SitePC, candidate: finding.CandidateEntryPC}
					existing, found := findings[key]
					if !found {
						findings[key] = finding
						continue
					}
					existing.LiveMX = appendUniqueShadowMX(existing.LiveMX, finding.LiveMX...)
					findings[key] = existing
				}
			}
		}
	}
	result := make([]ShadowDispatchCodeIsland, 0, len(findings))
	for _, finding := range findings {
		sort.Slice(finding.LiveMX, func(i, j int) bool {
			if finding.LiveMX[i].M != finding.LiveMX[j].M {
				return finding.LiveMX[i].M < finding.LiveMX[j].M
			}
			return finding.LiveMX[i].X < finding.LiveMX[j].X
		})
		result = append(result, finding)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].SitePC != result[j].SitePC {
			return result[i].SitePC < result[j].SitePC
		}
		return result[i].CandidateEntryPC < result[j].CandidateEntryPC
	})
	return result
}

func shadowUniqueSameBankTargets(site uint32, targets []uint32) []uint32 {
	bank := byte(site >> 16)
	seen := make(map[uint32]struct{})
	var result []uint32
	for _, target := range targets {
		target &= 0xffffff
		if target == 0 || byte(target>>16) != bank {
			continue
		}
		if _, found := seen[target]; found {
			continue
		}
		seen[target] = struct{}{}
		result = append(result, target)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
}

func shadowCodeIslandsBetweenTargets(image romimage.Image, site, previous, next uint32, spansByEntry map[uint32][]shadowDecodedSpan, instructionsByEntry map[uint32][]shadowDecodedInstruction, globalOwned map[uint32]struct{}, regions []decoder.DataRegion, tableSpans []ShadowTableSpan) []ShadowDispatchCodeIsland {
	if byte(previous>>16) != byte(next>>16) || uint16(previous) >= uint16(next) {
		return nil
	}
	owned := make(map[uint16]struct{})
	for _, span := range spansByEntry[previous] {
		if span.PC < previous || span.PC >= next {
			continue
		}
		for offset := uint8(0); offset < span.Length; offset++ {
			pc := uint16(span.PC) + uint16(offset)
			if uint32(pc) < uint32(uint16(next)) {
				owned[pc] = struct{}{}
			}
		}
	}
	if _, found := owned[uint16(previous)]; !found {
		return nil
	}
	type flowEnd struct {
		Mnemonic string
		MX       analysis.MXState
	}
	flowEnds := make(map[uint16][]flowEnd)
	for _, decoded := range instructionsByEntry[previous] {
		instruction := decoded.Instruction
		if !disassemblyFlowEnd(disassemblyMnemonic(&instruction)) {
			continue
		}
		end := uint16(decoded.PC) + uint16(instruction.Length)
		flowEnds[end] = append(flowEnds[end], flowEnd{
			Mnemonic: instruction.Mnemonic,
			MX:       analysis.MXState{M: decoded.M & 1, X: decoded.X & 1},
		})
	}
	var findings []ShadowDispatchCodeIsland
	cursor := uint16(previous)
	limit := uint16(next)
	for cursor < limit {
		if _, found := owned[cursor]; found {
			cursor++
			continue
		}
		gapStart := cursor
		for cursor < limit {
			if _, found := owned[cursor]; found {
				break
			}
			cursor++
		}
		gapEnd := cursor
		predecessors := flowEnds[gapStart]
		if len(predecessors) == 0 {
			continue
		}
		candidate := gapStart
		for candidate < gapEnd {
			candidatePC := uint32(byte(site>>16))<<16 | uint32(candidate)
			if len(spansByEntry[candidatePC]) != 0 || shadowInDataRegion(regions, byte(site>>16), candidate) || shadowAddressInTableSpan(candidatePC, tableSpans) {
				break
			}
			var liveMX []analysis.MXState
			end := uint16(0)
			for _, predecessor := range predecessors {
				candidateEnd, ok := shadowStraightLineReturn(image, byte(site>>16), candidate, gapEnd, predecessor.MX.M, predecessor.MX.X, regions)
				if !ok {
					continue
				}
				if end == 0 || candidateEnd < end {
					end = candidateEnd
				}
				liveMX = appendUniqueShadowMX(liveMX, predecessor.MX)
			}
			if end == 0 {
				break
			}
			if shadowRangeHasOtherOwnership(candidatePC, uint32(byte(site>>16))<<16|uint32(end), globalOwned, tableSpans) {
				break
			}
			findings = append(findings, ShadowDispatchCodeIsland{
				SitePC: site & 0xffffff, PreviousTargetPC: previous, NextTargetPC: next,
				CandidateEntryPC: candidatePC,
				EndExclusive:     uint32(byte(site>>16))<<16 | uint32(end),
				LiveMX:           liveMX, PrecededBy: predecessors[0].Mnemonic,
				Confidence: analysis.ConfidenceProbable,
				Reason:     "unclaimed bytes after a decoded flow terminator reach a return before the next known dispatch target",
			})
			predecessors = []flowEnd{{Mnemonic: "return", MX: liveMX[0]}}
			candidate = end
		}
	}
	return findings
}

func shadowAddressInTableSpan(address uint32, spans []ShadowTableSpan) bool {
	address &= 0xffffff
	for _, span := range spans {
		if address >= span.StartPC && address < span.EndExclusive {
			return true
		}
	}
	return false
}

func shadowRangeHasOtherOwnership(start, end uint32, owned map[uint32]struct{}, tableSpans []ShadowTableSpan) bool {
	for address := start; address < end; address++ {
		if _, found := owned[address&0xffffff]; found || shadowAddressInTableSpan(address, tableSpans) {
			return true
		}
	}
	return false
}

func shadowStraightLineReturn(image romimage.Image, bank byte, start, limit uint16, m, x uint8, regions []decoder.DataRegion) (uint16, bool) {
	pc := start
	nonReturn := 0
	for instructions := 0; instructions < 256 && pc < limit; instructions++ {
		if shadowInDataRegion(regions, bank, pc) {
			return 0, false
		}
		offset, err := romimage.LoROMOffset(bank, pc)
		if err != nil || offset < 0 || offset >= len(image) {
			return 0, false
		}
		instruction, err := cpu65816.Decode(image, offset, pc, bank, m&1, x&1)
		if err != nil || instruction.Length == 0 || uint32(pc)+uint32(instruction.Length) > uint32(limit) {
			return 0, false
		}
		next := pc + uint16(instruction.Length)
		switch instruction.Mnemonic {
		case "RTS", "RTL", "RTI":
			if nonReturn == 0 {
				return 0, false
			}
			return next, true
		case "REP":
			if instruction.Operand&0x20 != 0 {
				m = 0
			}
			if instruction.Operand&0x10 != 0 {
				x = 0
			}
		case "SEP":
			if instruction.Operand&0x20 != 0 {
				m = 1
			}
			if instruction.Operand&0x10 != 0 {
				x = 1
			}
		case "PLP", "XCE", "BRK", "COP":
			return 0, false
		}
		if disassemblyFlowEnd(disassemblyMnemonic(instruction)) {
			return 0, false
		}
		if pollConditionalBranch(instruction) {
			target := uint16(instruction.Operand)
			if target < start || target >= limit {
				return 0, false
			}
		}
		nonReturn++
		pc = next
	}
	return 0, false
}

func appendUniqueShadowMX(values []analysis.MXState, incoming ...analysis.MXState) []analysis.MXState {
	for _, value := range incoming {
		found := false
		for _, existing := range values {
			if existing == value {
				found = true
				break
			}
		}
		if !found {
			values = append(values, value)
		}
	}
	return values
}

func collectShadowTableSpans(comparisons []analysis.Comparison) []ShadowTableSpan {
	type spanKey struct {
		site, start, end uint32
		entryBytes       uint8
	}
	spans := make(map[spanKey]ShadowTableSpan)
	for _, comparison := range comparisons {
		facts := []*analysis.DispatchFact{comparison.Authored, comparison.Inferred}
		for _, fact := range facts {
			if fact == nil || len(fact.TableBases) == 0 {
				continue
			}
			entryCount := len(fact.TargetCandidates)
			if fact.TargetSetClosed && !fact.FieldUnknown("targets") {
				entryCount = len(fact.Targets)
			}
			if entryCount == 0 {
				continue
			}
			entryBytes := fact.TableEntryBytes
			if entryBytes == 0 {
				entryBytes = shadowTableEntryBytes("short", len(fact.TableBases))
			}
			ownership, confidence := shadowTableOwnership(*fact)
			provenance := make([]string, 0, len(fact.Evidence))
			for _, evidence := range fact.Evidence {
				provenance = append(provenance, evidence.Source)
			}
			sort.Strings(provenance)
			provenance = dedupeShadowStrings(provenance)
			for _, base := range fact.TableBases {
				start := base & 0xffffff
				length := uint32(entryCount) * uint32(entryBytes)
				if length == 0 || uint32(uint16(start))+length > 0xffff {
					continue
				}
				end := start + length
				key := spanKey{site: fact.SitePC & 0xffffff, start: start, end: end, entryBytes: entryBytes}
				incoming := ShadowTableSpan{
					SitePC: key.site, StartPC: start, EndExclusive: end,
					EntryBytes: entryBytes, EntryCount: entryCount,
					Ownership: ownership, Confidence: confidence, Provenance: provenance,
				}
				existing, found := spans[key]
				if !found {
					spans[key] = incoming
					continue
				}
				existing.Provenance = append(existing.Provenance, incoming.Provenance...)
				sort.Strings(existing.Provenance)
				existing.Provenance = dedupeShadowStrings(existing.Provenance)
				if incoming.Ownership == shadowTableOwnershipConfirmed {
					existing.Ownership, existing.Confidence = incoming.Ownership, incoming.Confidence
				}
				spans[key] = existing
			}
		}
	}
	result := make([]ShadowTableSpan, 0, len(spans))
	for _, span := range spans {
		result = append(result, span)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].StartPC != result[j].StartPC {
			return result[i].StartPC < result[j].StartPC
		}
		if result[i].EndExclusive != result[j].EndExclusive {
			return result[i].EndExclusive < result[j].EndExclusive
		}
		return result[i].SitePC < result[j].SitePC
	})
	return result
}

func shadowTableOwnership(fact analysis.DispatchFact) (string, analysis.Confidence) {
	confidence := analysis.ConfidenceProbable
	confirmed := fact.TargetSetClosed && !fact.FieldUnknown("targets") && !fact.FieldUnknown("table_bases")
	for _, evidence := range fact.Evidence {
		switch evidence.Confidence {
		case analysis.ConfidenceProven:
			confidence = analysis.ConfidenceProven
		case analysis.ConfidenceAuthored:
			if confidence != analysis.ConfidenceProven {
				confidence = analysis.ConfidenceAuthored
			}
		case analysis.ConfidenceObserved:
			if confidence == analysis.ConfidenceProbable {
				confidence = analysis.ConfidenceObserved
			}
		}
	}
	if confirmed && (confidence == analysis.ConfidenceProven || confidence == analysis.ConfidenceAuthored) {
		return shadowTableOwnershipConfirmed, confidence
	}
	return shadowTableOwnershipCandidate, confidence
}

func countShadowTableSpans(spans []ShadowTableSpan) (confirmed, candidates int) {
	for _, span := range spans {
		if span.Ownership == shadowTableOwnershipConfirmed {
			confirmed++
		} else {
			candidates++
		}
	}
	return confirmed, candidates
}

func applyShadowDispatchEvidence(report *ShadowReport, evidence DispatchCensusReport) error {
	if report == nil {
		return fmt.Errorf("cannot apply dispatch evidence to a nil report")
	}
	if evidence.ROMHash != "" && !strings.EqualFold(evidence.ROMHash, report.ROM.SHA256) {
		return fmt.Errorf("dispatch census ROM hash %s does not match analyzed ROM %s", evidence.ROMHash, report.ROM.SHA256)
	}
	bySite := make(map[uint32][]DispatchObservation)
	for _, observation := range evidence.Observations {
		site := observation.SitePC & 0xffffff
		bySite[site] = append(bySite[site], observation)
	}
	for index := range report.Unresolved {
		site := &report.Unresolved[index]
		observations := append([]DispatchObservation(nil), bySite[site.SitePC&0xffffff]...)
		if len(observations) == 0 {
			site.RuntimeStatus = shadowRuntimeUnobserved
			report.Summary.UnobservedUnresolvedSites++
			continue
		}
		report.Summary.ObservedUnresolvedSites++
		site.RuntimeObservations = observations
		status := shadowRuntimeObservedResolved
		for _, observation := range observations {
			site.RuntimeObservationCount += observation.ObservationCount
			missing := !observation.Found && !observation.Continuation
			switch {
			case observation.Trapped && missing:
				status = shadowRuntimeObservedTrappedMissing
			case missing && status != shadowRuntimeObservedTrappedMissing:
				status = shadowRuntimeObservedMissing
			case observation.Trapped && status != shadowRuntimeObservedTrappedMissing && status != shadowRuntimeObservedMissing:
				status = shadowRuntimeObservedTrapped
			}
		}
		site.RuntimeStatus = status
	}
	sort.SliceStable(report.Unresolved, func(i, j int) bool {
		left, right := report.Unresolved[i], report.Unresolved[j]
		leftRank, rightRank := shadowUnresolvedRuntimeRank(left), shadowUnresolvedRuntimeRank(right)
		if leftRank != rightRank {
			return leftRank < rightRank
		}
		if left.RuntimeObservationCount != right.RuntimeObservationCount {
			return left.RuntimeObservationCount > right.RuntimeObservationCount
		}
		return left.SitePC < right.SitePC
	})
	report.DispatchEvidence = &ShadowDispatchEvidence{
		Version: evidence.Version, ROMHash: evidence.ROMHash, TraceHash: evidence.TraceHash,
		Provenance: evidence.Provenance, Overflow: evidence.Overflow,
		Observations: len(evidence.Observations),
	}
	return nil
}

func shadowUnresolvedRuntimeRank(site ShadowUnresolvedSite) int {
	switch site.RuntimeStatus {
	case shadowRuntimeObservedTrappedMissing:
		return 0
	case shadowRuntimeObservedMissing:
		return 1
	case shadowRuntimeObservedTrapped:
		return 2
	case shadowRuntimeObservedResolved:
		return 3
	case shadowRuntimeUnobserved:
		if site.Priority == shadowPriorityLikelyBlocker {
			return 4
		}
		return 5
	default:
		return 6
	}
}

func dedupeShadowStrings(values []string) []string {
	result := values[:0]
	for _, value := range values {
		if len(result) == 0 || result[len(result)-1] != value {
			result = append(result, value)
		}
	}
	return result
}

// classifyShadowUnresolved recognizes report-only shapes that deserve earlier
// bring-up attention. It deliberately does not create a DispatchFact: handler
// words loaded from mutable script data do not prove a finite target set.
func classifyShadowUnresolved(image romimage.Image, site *ShadowUnresolvedSite) {
	if site.Mnemonic != "JMP" || site.AddressingMode != cpu65816.INDIR.String() || site.Operand > 0xff {
		return
	}
	pattern, ok := detectTaggedStreamDispatch(image, byte(site.SitePC>>16), uint16(site.SitePC), uint8(site.Operand))
	if !ok {
		return
	}
	site.Classification = shadowUnresolvedTaggedStreamDispatch
	site.Priority = shadowPriorityLikelyBlocker
	site.Reason = "sign-tagged stream word is staged in a direct-page slot and reached through a JSR/JMP trampoline; an unresolved handler can prevent stream advancement"
	site.StreamDispatch = &pattern
	site.StructuralHandlerCandidates = enumerateStructuralStreamHandlers(image, byte(site.SitePC>>16), pattern.StreamPointer)
}

// detectTaggedStreamDispatch searches the short straight-line window before a
// JMP (dp) trampoline for this value-provenance skeleton:
//
//	LDY memory; LDA abs,Y; BPL data; ...; STA dp; JSR trampoline
//
// The intervening comparison/terminator checks are intentionally allowed. The
// result is triage evidence only, so false negatives are preferable to turning
// arbitrary data into generated code.
func detectTaggedStreamDispatch(image romimage.Image, bank byte, site uint16, slot uint8) (ShadowStreamDispatchPattern, bool) {
	bankOffset := int(bank&0x7f) * 0x8000
	if bankOffset >= len(image) {
		return ShadowStreamDispatchPattern{}, false
	}
	bankBytes, err := image.Slice(bank, 0x8000, min(0x8000, len(image)-bankOffset))
	if err != nil || site < 0x8000 {
		return ShadowStreamDispatchPattern{}, false
	}
	siteOffset := int(site - 0x8000)
	if siteOffset < 0 || siteOffset+2 >= len(bankBytes) || bankBytes[siteOffset] != 0x6c || bankBytes[siteOffset+1] != slot || bankBytes[siteOffset+2] != 0x00 {
		return ShadowStreamDispatchPattern{}, false
	}
	windowStart := max(0, siteOffset-64)
	for callOffset := siteOffset - 3; callOffset >= windowStart; callOffset-- {
		if callOffset+2 >= len(bankBytes) || bankBytes[callOffset] != 0x20 || uint16(bankBytes[callOffset+1])|uint16(bankBytes[callOffset+2])<<8 != site {
			continue
		}
		storeOffset := findBackwardBytes(bankBytes, callOffset, max(windowStart, callOffset-24), []byte{0x85, slot})
		if storeOffset < 0 {
			continue
		}
		signOffset := findBackwardOpcode(bankBytes, storeOffset, max(windowStart, storeOffset-24), 0x10)
		if signOffset < 0 || signOffset+1 >= len(bankBytes) {
			continue
		}
		branchTarget := signOffset + 2 + int(int8(bankBytes[signOffset+1]))
		if branchTarget <= callOffset {
			continue
		}
		wordLoadOffset := findBackwardOpcode(bankBytes, signOffset, max(windowStart, signOffset-16), 0xb9)
		if wordLoadOffset < 0 || wordLoadOffset+2 >= len(bankBytes) {
			continue
		}
		pointerLoadOffset, pointer, ok := findVariableYLoad(bankBytes, wordLoadOffset, max(windowStart, wordLoadOffset-16))
		if !ok {
			continue
		}
		return ShadowStreamDispatchPattern{
			StreamPointerLoadPC: decoder.Address24(bank, uint16(pointerLoadOffset+0x8000)),
			StreamPointer:       pointer,
			StreamWordLoadPC:    decoder.Address24(bank, uint16(wordLoadOffset+0x8000)),
			SignTestPC:          decoder.Address24(bank, uint16(signOffset+0x8000)),
			TargetSlotStorePC:   decoder.Address24(bank, uint16(storeOffset+0x8000)),
			TargetSlot:          slot,
			TrampolineCallPC:    decoder.Address24(bank, uint16(callOffset+0x8000)),
		}, true
	}
	return ShadowStreamDispatchPattern{}, false
}

func findBackwardBytes(data []byte, before, lower int, pattern []byte) int {
	for offset := before - len(pattern); offset >= lower; offset-- {
		if offset+len(pattern) > len(data) {
			continue
		}
		match := true
		for index, value := range pattern {
			if data[offset+index] != value {
				match = false
				break
			}
		}
		if match {
			return offset
		}
	}
	return -1
}

func findBackwardOpcode(data []byte, before, lower int, opcode byte) int {
	for offset := before - 1; offset >= lower; offset-- {
		if data[offset] == opcode {
			return offset
		}
	}
	return -1
}

func findVariableYLoad(data []byte, before, lower int) (int, uint16, bool) {
	for offset := before - 3; offset >= lower; offset-- {
		if offset+2 >= len(data) {
			continue
		}
		switch data[offset] {
		case 0xac, 0xbc: // LDY abs / LDY abs,X; immediate LDY is deliberately excluded.
			return offset, uint16(data[offset+1]) | uint16(data[offset+2])<<8, true
		}
	}
	return 0, 0, false
}

// enumerateStructuralStreamHandlers implements the cheap first half of the
// recovery workflow: stores to the interpreter's stream-pointer field imply a
// handler boundary after the preceding RTS/RTL. Shared advance code and shared
// stores necessarily under-count, so these addresses remain suggestions.
func enumerateStructuralStreamHandlers(image romimage.Image, bank byte, streamPointer uint16) []uint32 {
	bankOffset := int(bank&0x7f) * 0x8000
	if bankOffset >= len(image) {
		return nil
	}
	bankBytes := image[bankOffset:min(len(image), bankOffset+0x8000)]
	candidates := make(map[uint32]struct{})
	for offset := 0; offset+2 < len(bankBytes); offset++ {
		opcode := bankBytes[offset]
		if opcode != 0x8d && opcode != 0x9d && opcode != 0x99 { // STA abs / abs,X / abs,Y
			continue
		}
		operand := uint16(bankBytes[offset+1]) | uint16(bankBytes[offset+2])<<8
		if operand != streamPointer {
			continue
		}
		for previous := offset - 1; previous >= 0; previous-- {
			if bankBytes[previous] != 0x60 && bankBytes[previous] != 0x6b {
				continue
			}
			candidate := previous + 1
			if candidate < len(bankBytes) && bankBytes[candidate] != 0x00 && bankBytes[candidate] != 0xff {
				candidates[decoder.Address24(bank, uint16(candidate+0x8000))] = struct{}{}
			}
			break
		}
	}
	result := make([]uint32, 0, len(candidates))
	for candidate := range candidates {
		result = append(result, candidate)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
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
	candidates := targets
	evidenceDetail := "target entries recovered without authored dispatch directives; table bound remains heuristic"
	if len(instruction.DispatchCandidateEntries) > 0 {
		candidates = normalizeShadowTargets(bank, instruction.DispatchCandidateEntries, instruction.DispatchKind)
		if instruction.DispatchBound == "structural_candidate" {
			evidenceDetail = fmt.Sprintf("conservative count=%d; post-zero candidate count=%d lands on the earliest same-bank handler but remains open", len(targets), len(candidates))
		} else {
			evidenceDetail = fmt.Sprintf("conservative count=%d; post-zero plausible candidate count=%d has no closed structural bound", len(targets), len(candidates))
		}
	}
	tableBases := normalizeTableBases(bank, instruction.DispatchTableBase)
	if len(tableBases) == 0 && (instruction.Mode == cpu65816.INDIRX || instruction.Mode == cpu65816.INDIR) && instruction.Operand >= 0x8000 {
		tableBases = []uint32{decoder.Address24(bank, uint16(instruction.Operand))}
	}
	fact := analysis.DispatchFact{
		SitePC: instruction.Address & 0xffffff, FunctionEntries: []uint32{decoder.Address24(bank, functionStart)},
		InstructionBytes: shadowInstructionBytes(image, bank, uint16(instruction.Address), instruction.Length),
		Mnemonic:         instruction.Mnemonic, AddressingMode: instruction.Mode.String(), LiveMX: []analysis.MXState{{M: key.M & 1, X: key.X & 1}},
		Transfer: transferForInstruction(instruction, instruction.DispatchReturn != nil), TargetEntryKind: analysis.EntryComputed,
		TargetCandidates: candidates, TargetSetClosed: false, IndexRegister: instruction.DispatchIndexReg,
		TableBases: tableBases, TableEntryBytes: shadowTableEntryBytes(instruction.DispatchKind, len(tableBases)), SEPMask: instruction.DispatchSEP,
		UnknownFields: []string{"targets"},
		Evidence:      []analysis.Evidence{{Source: "static.decoder.auto_dispatch", Confidence: analysis.ConfidenceProbable, Detail: evidenceDetail}},
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
		IndexRegister: "A", TableBases: tableBases, TableEntryBytes: 2, ReturnPC: returnPC, SEPMask: sepMask,
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
	if existing.TableEntryBytes != incoming.TableEntryBytes {
		existing.UnknownFields = append(existing.UnknownFields, "table_entry_bytes")
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

func shadowTableEntryBytes(kind string, baseCount int) uint8 {
	if baseCount > 1 {
		return 1
	}
	if kind == "long" {
		return 3
	}
	return 2
}

func normalizeShadowTargets(bank byte, targets []uint32, kind string) []uint32 {
	result := make([]uint32, len(targets))
	for index, target := range targets {
		// A zero word is a sparse-table hole. resolveDispatch represents a
		// short-table zero as bank:$0000, while auto-recovery retains literal
		// zero; canonicalize both so authored and inferred evidence compares.
		if target == 0 || kind != "long" && uint16(target) == 0 {
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
	fmt.Fprintf(output, "table ownership: %d confirmed data span(s), %d candidate span(s)\n",
		summary.ConfirmedTableSpans, summary.CandidateTableSpans)
	fmt.Fprintf(output, "dispatch gap sweep: %d probable unclaimed code island(s) (review-only)\n",
		summary.DispatchCodeIslands)
	fmt.Fprintf(output, "boundary landing sweep: %d unclaimed entry candidate(s): %d probable, %d speculative (%d candidate-table conflicts; review-only)\n",
		summary.LandingCandidates, summary.ProbableLandingCandidates, summary.SpeculativeLandingCandidates, summary.LandingCandidateTableConflicts)
	fmt.Fprintf(output, "table-first target discovery: %d pointer seed(s) (absolute=%d base+offset=%d; base arithmetic=%d base(s)/%d site(s)); %d accepted target(s) (absolute=%d base+offset=%d): %d new landing(s), %d address-taken internal; %d probable, %d speculative (post-terminator=%d pointer-window=%d rejected=%d; review-only)\n",
		summary.TableFirstPointerSeeds, summary.TableFirstAbsoluteSeeds, summary.TableFirstBaseOffsetSeeds,
		summary.TableFirstBaseEvidenceBases, summary.TableFirstBaseEvidenceSites,
		summary.TableFirstTargets, summary.TableFirstAbsoluteTargets, summary.TableFirstBaseOffsetTargets,
		summary.TableFirstNewLandings, summary.TableFirstInternalTargets,
		summary.ProbableTableFirstTargets, summary.SpeculativeTableFirstTargets,
		summary.TableFirstPostTerminatorSeeds, summary.TableFirstPointerWindowSeeds, summary.TableFirstRejectedSeeds)
	entrySummary := report.EntryRecovery.Summary
	fmt.Fprintf(output, "vector-root entry recovery: %d authored declaration(s); exact=%d address-other-M/X=%d continuation=%d internal-owned=%d not-recovered=%d; %d roots -> %d variants in %d passes; decode issues=%d\n",
		entrySummary.AuthoredEntries, entrySummary.ExactVariants, entrySummary.AddressOtherMX,
		entrySummary.ProvenContinuations, entrySummary.InternalOwned, entrySummary.NotRecovered,
		entrySummary.InitialRootVariants, entrySummary.FinalRootVariants, entrySummary.VariantPasses, entrySummary.DecodeIssues)
	fmt.Fprintf(output, "authored-entry ROM pointer clusters: %d declaration(s), %d probable; %d are otherwise not recovered (corroboration-only)\n",
		entrySummary.EntriesWithPointerClusters, entrySummary.ProbablePointerClusterEntries, entrySummary.NotRecoveredWithPointerClusters)
	ablationSummary := report.EntryAblation.Summary
	fmt.Fprintf(output, "static entry ablation: %d declaration(s)/%d unique variant(s), %d dependency edge(s); individually recoverable=%d; inclusion-minimal roots=%d declaration(s)/%d variant(s), batch-recoverable=%d (routine=%d tail=%d computed=%d internal-continuation=%d), vector-covered=%d (report-only)\n",
		ablationSummary.AuthoredDeclarations, ablationSummary.UniqueAuthoredVariants, ablationSummary.StaticDependencyEdges,
		ablationSummary.IndividuallyRecoverable, ablationSummary.RetainedRootDeclarations, ablationSummary.RetainedUniqueRootVariants,
		ablationSummary.BatchRecoverable, ablationSummary.BatchRoutineTargets, ablationSummary.BatchTailTargets,
		ablationSummary.BatchComputedTargets, ablationSummary.BatchInternalContinuations, ablationSummary.VectorCoveredDeclarations)
	fmt.Fprintf(output, "authored HLE obligations: %d declaration(s), %d without an explicit func entry (preserved independently of root ablation)\n",
		ablationSummary.AuthoredHLEObligations, ablationSummary.HLEOnlyObligations)
	fmt.Fprintf(output, "shadow-root unresolved dynamic edges: %d raw emissions -> %d unique source sites; likely bring-up blockers=%d; decode issues=%d\n",
		summary.RawUnresolvedEmissions, summary.UniqueUnresolvedSites, summary.LikelyBlockingUnresolvedSites, summary.DecodeIssues)
	if evidence := report.DispatchEvidence; evidence != nil {
		fmt.Fprintf(output, "runtime triage: %d unresolved site(s) observed, %d unobserved; evidence observations=%d overflow=%t trace_sha256=%s\n",
			summary.ObservedUnresolvedSites, summary.UnobservedUnresolvedSites,
			evidence.Observations, evidence.Overflow, evidence.TraceHash)
		for _, unresolved := range report.Unresolved {
			if unresolved.RuntimeStatus == shadowRuntimeUnobserved {
				continue
			}
			fmt.Fprintf(output, "[RUNTIME-UNRESOLVED] %s status=%s hits=%d class=%s priority=%s\n",
				shadowAddress(unresolved.SitePC), unresolved.RuntimeStatus,
				unresolved.RuntimeObservationCount, unresolved.Classification, unresolved.Priority)
		}
	}
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
			fmt.Fprintf(output, "  authored: %s -> %s targets=%d tables=%s entry_bytes=%d return=%s sep=$%02X\n",
				comparison.Authored.Transfer, comparison.Authored.TargetEntryKind, len(comparison.Authored.Targets), shadowAddresses(comparison.Authored.TableBases), comparison.Authored.TableEntryBytes, shadowOptionalAddress(comparison.Authored.ReturnPC), comparison.Authored.SEPMask)
		}
		if comparison.Inferred != nil {
			fmt.Fprintf(output, "  inferred: %s -> %s targets_closed=%t tables=%s entry_bytes=%d return=%s sep=$%02X live_mx=%s\n",
				comparison.Inferred.Transfer, comparison.Inferred.TargetEntryKind, comparison.Inferred.TargetSetClosed, shadowAddresses(comparison.Inferred.TableBases), comparison.Inferred.TableEntryBytes, shadowOptionalAddress(comparison.Inferred.ReturnPC), comparison.Inferred.SEPMask, shadowMX(comparison.Inferred.LiveMX))
		}
		for _, difference := range comparison.Differences {
			fmt.Fprintf(output, "  reason: %s\n", difference)
		}
	}
	if verbose {
		for _, unresolved := range report.Unresolved {
			fmt.Fprintf(output, "[UNRESOLVED] %s %s %s %s operand=$%X class=%s priority=%s runtime=%s hits=%d reason=%s callers=%d\n",
				shadowAddress(unresolved.SitePC), unresolved.InstructionBytes, unresolved.Mnemonic, unresolved.AddressingMode, unresolved.Operand, unresolved.Classification, unresolved.Priority, unresolved.RuntimeStatus, unresolved.RuntimeObservationCount, unresolved.Reason, len(unresolved.Callers))
			if pattern := unresolved.StreamDispatch; pattern != nil {
				fmt.Fprintf(output, "  stream-pattern: interpreter=%s pointer-load=%s pointer=$%04X word-load=%s sign-test=%s slot-store=%s slot=$%02X trampoline-call=%s\n",
					shadowOptionalPatternAddress(pattern.InterpreterEntryPC), shadowAddress(pattern.StreamPointerLoadPC), pattern.StreamPointer, shadowAddress(pattern.StreamWordLoadPC), shadowAddress(pattern.SignTestPC), shadowAddress(pattern.TargetSlotStorePC), pattern.TargetSlot, shadowAddress(pattern.TrampolineCallPC))
				fmt.Fprintf(output, "  structural-handler-candidates=%s (incomplete heuristic; confirm with dispatch census)\n", shadowAddresses(unresolved.StructuralHandlerCandidates))
			}
			for _, caller := range unresolved.Callers {
				fmt.Fprintf(output, "  caller=%s M%dX%d reachability=%s\n", shadowAddress(caller.FunctionEntry), caller.LiveMX.M, caller.LiveMX.X, unresolved.Reachability)
			}
			for _, observation := range unresolved.RuntimeObservations {
				fmt.Fprintf(output, "  observed-target=%s M%dX%d hits=%d generated=%t continuation=%t trapped=%t\n",
					shadowAddress(observation.TargetPC), observation.M, observation.X,
					observation.ObservationCount, observation.Found, observation.Continuation, observation.Trapped)
			}
		}
		for _, issue := range report.DecodeIssues {
			fmt.Fprintf(output, "[DECODE-ISSUE] %s M%dX%d %s\n", shadowAddress(issue.FunctionEntry), issue.EntryMX.M, issue.EntryMX.X, issue.Error)
		}
		for _, span := range report.TableSpans {
			fmt.Fprintf(output, "[TABLE-%s] [%s,%s) site=%s entries=%d entry_bytes=%d confidence=%s provenance=%s\n",
				strings.ToUpper(span.Ownership), shadowAddress(span.StartPC), shadowAddress(span.EndExclusive),
				shadowAddress(span.SitePC), span.EntryCount, span.EntryBytes, span.Confidence, strings.Join(span.Provenance, ","))
		}
		for _, island := range report.DispatchCodeIslands {
			fmt.Fprintf(output, "[DISPATCH-CODE-ISLAND] %s..%s site=%s between=%s,%s preceded_by=%s live_mx=%s confidence=%s reason=%s\n",
				shadowAddress(island.CandidateEntryPC), shadowAddress(island.EndExclusive),
				shadowAddress(island.SitePC), shadowAddress(island.PreviousTargetPC),
				shadowAddress(island.NextTargetPC), island.PrecededBy,
				shadowMX(island.LiveMX), island.Confidence, island.Reason)
		}
		for _, candidate := range report.LandingCandidates {
			fmt.Fprintf(output, "[LANDING-CANDIDATE] %s anchor=%s preceded_by=%s ownership=%s confidence=%s shapes=%d reason=%s\n",
				shadowAddress(candidate.CandidateEntryPC), shadowAddress(candidate.AnchorPC),
				candidate.PrecededBy, candidate.Ownership, candidate.Confidence, len(candidate.Variants), candidate.Reason)
			for _, variant := range candidate.Variants {
				fmt.Fprintf(output, "  shape=%s..%s entry_mx=%s termination=%s instructions=%d ownership=%s confidence=%s\n",
					shadowAddress(candidate.CandidateEntryPC), shadowAddress(variant.EndExclusive),
					shadowMX(variant.EntryMX), variant.Termination, variant.InstructionCount, variant.Ownership, variant.Confidence)
			}
		}
		for _, target := range report.TableFirstTargets {
			fmt.Fprintf(output, "[TABLE-FIRST-TARGET] %s class=%s boundary=%s anchor=%s entry_mx=%s landing_seed=%s landing_ownership=%s landing_confidence=%s confidence=%s sources=%d reason=%s\n",
				shadowAddress(target.TargetPC), target.Classification, target.LandingBoundary,
				shadowOptionalPatternAddress(target.AnchorPC), shadowMX(target.EntryMX), target.LandingSeed,
				target.LandingOwnership, target.LandingConfidence, target.Confidence, len(target.Sources), target.Reason)
			for _, source := range target.Sources {
				fmt.Fprintf(output, "  source=%s encoding=%s base=%s base_evidence=%s word=%s window=%s..%s known=%d distinct=%d ownership=%s confidence=%s\n",
					source.Kind, source.Encoding, shadowOptionalPatternAddress(source.BasePC), shadowAddresses(source.BaseEvidencePCs), shadowAddress(source.WordPC), shadowAddress(source.StartPC), shadowAddress(source.EndExclusive),
					source.KnownEntries, source.DistinctKnownTargets, source.Ownership, source.Confidence)
			}
		}
		for _, rejection := range report.TableFirstRejections {
			fmt.Fprintf(output, "[TABLE-FIRST-REJECTED] %s reason=%s sources=%d\n",
				shadowAddress(rejection.TargetPC), rejection.Reason, len(rejection.Sources))
			for _, source := range rejection.Sources {
				fmt.Fprintf(output, "  source=%s encoding=%s base=%s base_evidence=%s word=%s window=%s..%s known=%d distinct=%d ownership=%s confidence=%s\n",
					source.Kind, source.Encoding, shadowOptionalPatternAddress(source.BasePC), shadowAddresses(source.BaseEvidencePCs), shadowAddress(source.WordPC), shadowAddress(source.StartPC), shadowAddress(source.EndExclusive),
					source.KnownEntries, source.DistinctKnownTargets, source.Ownership, source.Confidence)
			}
		}
		for _, entry := range report.EntryRecovery.Entries {
			if entry.Status == shadowEntryRecoveryExact {
				continue
			}
			fmt.Fprintf(output, "[ENTRY-%s] %s name=%s authored=M%dX%d recovered=%s reason=%s\n",
				strings.ToUpper(strings.ReplaceAll(entry.Status, "_", "-")), shadowAddress(entry.PC), entry.Name,
				entry.AuthoredMX.M, entry.AuthoredMX.X, shadowMX(entry.RecoveredMX), entry.Reason)
			for _, cluster := range entry.PointerClusters {
				fmt.Fprintf(output, "  pointer-cluster=%s..%s entries=%d distinct=%d occurrences=%d ownership=%s confidence=%s\n",
					shadowAddress(cluster.StartPC), shadowAddress(cluster.EndExclusive), cluster.EntryCount,
					cluster.DistinctTargets, cluster.TargetOccurrences, cluster.Ownership, cluster.Confidence)
			}
		}
		for _, entry := range report.EntryAblation.Entries {
			if entry.Status == shadowEntryAblationRecoverable && entry.IndividuallyRecoverable {
				continue
			}
			fmt.Fprintf(output, "[ABLATION-%s] %s name=%s authored=M%dX%d entry_kind=%s individually_recoverable=%t incoming=%d hle=%s reason=%s\n",
				strings.ToUpper(strings.ReplaceAll(entry.Status, "_", "-")), shadowAddress(entry.PC), entry.Name,
				entry.AuthoredMX.M, entry.AuthoredMX.X, entry.EntryKindHint, entry.IndividuallyRecoverable, len(entry.Incoming), shadowStringsOrNone(entry.AuthoredHLE), entry.Reason)
			for _, incoming := range entry.Incoming {
				fmt.Fprintf(output, "  incoming=%s M%dX%d kinds=%s\n", shadowAddress(incoming.PC), incoming.EntryMX.M, incoming.EntryMX.X, strings.Join(incoming.Kinds, ","))
			}
		}
		for _, obligation := range report.EntryAblation.HLEObligations {
			fmt.Fprintf(output, "[HLE-OBLIGATION] %s directive=%s function=%s predicate=%s explicit_func=%t\n",
				shadowAddress(obligation.PC), obligation.Directive, shadowStringOrNone(obligation.Function), shadowStringOrNone(obligation.Predicate), obligation.AuthoredEntry)
		}
	} else {
		fmt.Fprintln(output, "use --verbose for authored-only/automatic facts, deduplicated callers, and decode issues; --format json emits the complete report")
	}
}

func shadowStringsOrNone(values []string) string {
	if len(values) == 0 {
		return "none"
	}
	return strings.Join(values, ",")
}

func shadowStringOrNone(value string) string {
	if value == "" {
		return "none"
	}
	return value
}

func shadowAddress(address uint32) string {
	return fmt.Sprintf("$%02X:%04X", byte(address>>16), uint16(address))
}

func shadowOptionalPatternAddress(address uint32) string {
	if address == 0 {
		return "unknown"
	}
	return shadowAddress(address)
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
