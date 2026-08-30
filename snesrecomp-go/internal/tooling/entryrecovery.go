package tooling

import (
	"fmt"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const (
	shadowEntryRecoveryExact        = "exact_variant"
	shadowEntryRecoveryAddressOnly  = "address_other_mx"
	shadowEntryRecoveryContinuation = "proven_continuation"
	shadowEntryRecoveryInternal     = "internal_owned"
	shadowEntryRecoveryMissing      = "not_recovered"
)

// ShadowEntryRecoverySummary compares authored func declarations with a
// second fixed point seeded only by hardware vectors. Counts are declarations,
// not unique addresses or generated M/X variants.
type ShadowEntryRecoverySummary struct {
	AuthoredEntries                 int `json:"authored_entries"`
	ExactVariants                   int `json:"exact_variants"`
	AddressOtherMX                  int `json:"address_other_mx"`
	ProvenContinuations             int `json:"proven_continuations"`
	InternalOwned                   int `json:"internal_owned"`
	NotRecovered                    int `json:"not_recovered"`
	InitialRootVariants             int `json:"initial_root_variants"`
	FinalRootVariants               int `json:"final_root_variants"`
	VariantPasses                   int `json:"variant_passes"`
	DecodeIssues                    int `json:"decode_issues"`
	EntriesWithPointerClusters      int `json:"entries_with_pointer_clusters"`
	NotRecoveredWithPointerClusters int `json:"not_recovered_with_pointer_clusters"`
	ProbablePointerClusterEntries   int `json:"probable_pointer_cluster_entries"`
}

type ShadowEntryRecoveryRecord struct {
	PC              uint32                      `json:"pc"`
	Name            string                      `json:"name"`
	AuthoredMX      analysis.MXState            `json:"authored_mx"`
	RecoveredMX     []analysis.MXState          `json:"recovered_mx,omitempty"`
	PointerClusters []ShadowEntryPointerCluster `json:"pointer_clusters,omitempty"`
	Status          string                      `json:"status"`
	Reason          string                      `json:"reason"`
}

// ShadowEntryPointerCluster records a contiguous run of same-bank ROM words
// that name authored entries. It corroborates a configured address without
// claiming the table's runtime index, bounds, or reachability.
type ShadowEntryPointerCluster struct {
	StartPC           uint32              `json:"start_pc"`
	EndExclusive      uint32              `json:"end_exclusive"`
	EntryCount        int                 `json:"entry_count"`
	DistinctTargets   int                 `json:"distinct_targets"`
	TargetOccurrences int                 `json:"target_occurrences"`
	Ownership         string              `json:"ownership"`
	Confidence        analysis.Confidence `json:"confidence"`
}

type ShadowEntryRecoveryReport struct {
	Summary      ShadowEntryRecoverySummary  `json:"summary"`
	Entries      []ShadowEntryRecoveryRecord `json:"entries"`
	DecodeIssues []ShadowDecodeIssue         `json:"decode_issues,omitempty"`
	Limitations  []string                    `json:"limitations"`
}

func analyzeShadowEntryRecovery(image romimage.Image, banks []shadowBank, regions []decoder.DataRegion, calleeExitMX map[decoder.Variant]decoder.MX, jobs int) (ShadowEntryRecoveryReport, error) {
	entries := make(map[byte][]config.Entry, len(banks))
	bankConfigs := make(map[byte]*config.Config, len(banks))
	for _, bank := range banks {
		entries[bank.ID] = nil
		bankConfigs[bank.ID] = bank.Config
	}
	if bankConfigs[0] != nil {
		entries[0] = appendShadowAutoVectorEntries(image, nil)
	}
	stats := shadowInferenceStats{initialVariants: countShadowEntryVariants(entries)}
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
			return ShadowEntryRecoveryReport{}, fmt.Errorf("root-only entry recovery did not converge in %d passes", passLimit)
		}
	}
	stats.finalVariants = countShadowEntryVariants(entries)

	recoveredMX := make(map[uint32][]analysis.MXState)
	for bank, bankEntries := range entries {
		for _, entry := range bankEntries {
			pc := decoder.Address24(bank, entry.Start)
			recoveredMX[pc] = appendUniqueShadowMX(recoveredMX[pc], analysis.MXState{M: entry.EntryMX.M & 1, X: entry.EntryMX.X & 1})
		}
	}
	owned := make(map[uint32]struct{})
	var issues []ShadowDecodeIssue
	for _, result := range results {
		if result.issue != nil {
			issues = append(issues, *result.issue)
			continue
		}
		for _, instruction := range result.instructions {
			owned[instruction.PC&0xffffff] = struct{}{}
		}
	}
	facts, _, _, summaryIssues := summarizeShadowResults(image, results)
	facts = inferShadowContinuationFacts(image, banks, regions, calleeExitMX, facts, collectShadowContinuationReaches(results))
	continuations := make(map[uint32]struct{})
	for _, fact := range facts {
		if fact.TargetEntryKind != analysis.EntryContinuation || !fact.TargetSetClosed {
			continue
		}
		for _, target := range fact.Targets {
			continuations[target&0xffffff] = struct{}{}
		}
	}
	issues = append(issues, summaryIssues...)
	issues = dedupeShadowEntryRecoveryIssues(issues)

	report := ShadowEntryRecoveryReport{
		Summary: ShadowEntryRecoverySummary{
			InitialRootVariants: stats.initialVariants, FinalRootVariants: stats.finalVariants,
			VariantPasses: stats.passes, DecodeIssues: len(issues),
		},
		DecodeIssues: issues,
		Limitations: []string{
			"the recovery fixed point starts only at reset, native NMI, and native IRQ vector targets in bank zero",
			"not_recovered means absent from this conservative root closure, not unreachable or invalid code",
			"authored data regions, HLE dispatch declarations, and exit-M/X routes remain available while func entries and authored dynamic-dispatch declarations are withheld",
			"internal_owned is decoded at an instruction boundary but is not proven to require an externally callable entry",
		},
	}
	for _, bank := range banks {
		for _, entry := range bank.Config.Entries {
			pc := decoder.Address24(bank.ID, entry.Start)
			authoredMX := analysis.MXState{M: entry.EntryMX.M & 1, X: entry.EntryMX.X & 1}
			record := ShadowEntryRecoveryRecord{
				PC: pc, Name: entry.Name, AuthoredMX: authoredMX,
				RecoveredMX: append([]analysis.MXState(nil), recoveredMX[pc]...),
			}
			sortShadowEntryRecoveryMX(record.RecoveredMX)
			switch {
			case shadowEntryRecoveryHasMX(record.RecoveredMX, authoredMX):
				record.Status = shadowEntryRecoveryExact
				record.Reason = "the vector-rooted fixed point recovered the authored address and M/X variant"
				report.Summary.ExactVariants++
			case len(record.RecoveredMX) != 0:
				record.Status = shadowEntryRecoveryAddressOnly
				record.Reason = "the address was recovered, but not with the authored entry M/X state"
				report.Summary.AddressOtherMX++
			case shadowEntryRecoveryContains(continuations, pc):
				record.Status = shadowEntryRecoveryContinuation
				record.Reason = "static stack provenance recovered the address as a continuation rather than a normal routine entry"
				report.Summary.ProvenContinuations++
			case shadowEntryRecoveryContains(owned, pc):
				record.Status = shadowEntryRecoveryInternal
				record.Reason = "the address is an owned instruction boundary inside a vector-rooted generated region"
				report.Summary.InternalOwned++
			default:
				record.Status = shadowEntryRecoveryMissing
				record.Reason = "no vector-rooted direct call, computed target, continuation proof, or internal instruction boundary recovered this declaration"
				report.Summary.NotRecovered++
			}
			report.Entries = append(report.Entries, record)
		}
	}
	report.Summary.AuthoredEntries = len(report.Entries)
	sort.Slice(report.Entries, func(i, j int) bool {
		if report.Entries[i].PC != report.Entries[j].PC {
			return report.Entries[i].PC < report.Entries[j].PC
		}
		if report.Entries[i].AuthoredMX.M != report.Entries[j].AuthoredMX.M {
			return report.Entries[i].AuthoredMX.M < report.Entries[j].AuthoredMX.M
		}
		if report.Entries[i].AuthoredMX.X != report.Entries[j].AuthoredMX.X {
			return report.Entries[i].AuthoredMX.X < report.Entries[j].AuthoredMX.X
		}
		return report.Entries[i].Name < report.Entries[j].Name
	})
	return report, nil
}

func countShadowEntryVariants(entries map[byte][]config.Entry) int {
	count := 0
	for _, bankEntries := range entries {
		count += len(bankEntries)
	}
	return count
}

func shadowEntryRecoveryHasMX(values []analysis.MXState, want analysis.MXState) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}

func sortShadowEntryRecoveryMX(values []analysis.MXState) {
	sort.Slice(values, func(i, j int) bool {
		if values[i].M != values[j].M {
			return values[i].M < values[j].M
		}
		return values[i].X < values[j].X
	})
}

func shadowEntryRecoveryContains(values map[uint32]struct{}, pc uint32) bool {
	_, found := values[pc&0xffffff]
	return found
}

func dedupeShadowEntryRecoveryIssues(issues []ShadowDecodeIssue) []ShadowDecodeIssue {
	seen := make(map[ShadowDecodeIssue]struct{})
	result := make([]ShadowDecodeIssue, 0, len(issues))
	for _, issue := range issues {
		if _, duplicate := seen[issue]; duplicate {
			continue
		}
		seen[issue] = struct{}{}
		result = append(result, issue)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].FunctionEntry != result[j].FunctionEntry {
			return result[i].FunctionEntry < result[j].FunctionEntry
		}
		if result[i].EntryMX.M != result[j].EntryMX.M {
			return result[i].EntryMX.M < result[j].EntryMX.M
		}
		if result[i].EntryMX.X != result[j].EntryMX.X {
			return result[i].EntryMX.X < result[j].EntryMX.X
		}
		return result[i].Error < result[j].Error
	})
	return result
}

type shadowEntryPointerHit struct {
	offset int
	target uint32
}

func annotateShadowEntryPointerClusters(image romimage.Image, results []shadowDecodeResult, tableSpans []ShadowTableSpan, report *ShadowEntryRecoveryReport) {
	if report == nil || len(report.Entries) == 0 {
		return
	}
	recordsByPC := make(map[uint32][]int)
	targetsByBank := make(map[byte]map[uint16]uint32)
	for index, entry := range report.Entries {
		pc := entry.PC & 0xffffff
		recordsByPC[pc] = append(recordsByPC[pc], index)
		bank := byte(pc >> 16)
		if targetsByBank[bank] == nil {
			targetsByBank[bank] = make(map[uint16]uint32)
		}
		targetsByBank[bank][uint16(pc)] = pc
	}
	owned := make(map[uint32]struct{})
	for _, result := range results {
		for _, span := range result.spans {
			for offset := uint8(0); offset < span.Length; offset++ {
				owned[(span.PC&0xff0000)|uint32(uint16(span.PC)+uint16(offset))] = struct{}{}
			}
		}
	}
	for bank, targets := range targetsByBank {
		bankOffset := int(bank&0x7f) * 0x8000
		if bankOffset >= len(image) {
			continue
		}
		bankLength := min(0x8000, len(image)-bankOffset)
		for parity := 0; parity < 2; parity++ {
			var run []shadowEntryPointerHit
			flush := func() {
				if len(run) >= 2 {
					annotateShadowEntryPointerRun(bank, run, recordsByPC, owned, tableSpans, report)
				}
				run = run[:0]
			}
			for relative := parity; relative+1 < bankLength; relative += 2 {
				value := uint16(image[bankOffset+relative]) | uint16(image[bankOffset+relative+1])<<8
				target, found := targets[value]
				if !found {
					flush()
					continue
				}
				run = append(run, shadowEntryPointerHit{offset: relative, target: target})
			}
			flush()
		}
	}
	for index := range report.Entries {
		entry := &report.Entries[index]
		if len(entry.PointerClusters) == 0 {
			continue
		}
		report.Summary.EntriesWithPointerClusters++
		if entry.Status == shadowEntryRecoveryMissing {
			report.Summary.NotRecoveredWithPointerClusters++
		}
		probable := false
		for _, cluster := range entry.PointerClusters {
			if cluster.Confidence != analysis.ConfidenceSpeculative {
				probable = true
				break
			}
		}
		if probable {
			report.Summary.ProbablePointerClusterEntries++
		}
		sort.Slice(entry.PointerClusters, func(i, j int) bool {
			left, right := entry.PointerClusters[i], entry.PointerClusters[j]
			if left.StartPC != right.StartPC {
				return left.StartPC < right.StartPC
			}
			return left.EndExclusive < right.EndExclusive
		})
	}
}

func annotateShadowEntryPointerRun(bank byte, run []shadowEntryPointerHit, recordsByPC map[uint32][]int, owned map[uint32]struct{}, tableSpans []ShadowTableSpan, report *ShadowEntryRecoveryReport) {
	distinct := make(map[uint32]int)
	for _, hit := range run {
		distinct[hit.target&0xffffff]++
	}
	if len(distinct) < 2 {
		return
	}
	bankBase := uint32(bank) << 16
	start := bankBase + uint32(0x8000+run[0].offset)
	end := bankBase + uint32(0x8000+run[len(run)-1].offset+2)
	ownership, confidence := shadowEntryPointerOwnership(start, end, len(run), owned, tableSpans)
	for target, occurrences := range distinct {
		cluster := ShadowEntryPointerCluster{
			StartPC: start, EndExclusive: end, EntryCount: len(run), DistinctTargets: len(distinct),
			TargetOccurrences: occurrences, Ownership: ownership, Confidence: confidence,
		}
		for _, index := range recordsByPC[target&0xffffff] {
			report.Entries[index].PointerClusters = append(report.Entries[index].PointerClusters, cluster)
		}
	}
}

func shadowEntryPointerOwnership(start, end uint32, entryCount int, owned map[uint32]struct{}, tableSpans []ShadowTableSpan) (string, analysis.Confidence) {
	if shadowEntryPointerRangeInTable(start, end, tableSpans, shadowTableOwnershipConfirmed) {
		return shadowTableOwnershipConfirmed, analysis.ConfidenceProven
	}
	if shadowEntryPointerRangeInTable(start, end, tableSpans, shadowTableOwnershipCandidate) {
		return shadowTableOwnershipCandidate, analysis.ConfidenceProbable
	}
	for address := start; address < end; address++ {
		if _, found := owned[address&0xffffff]; found {
			return "decoded_code_overlap", analysis.ConfidenceSpeculative
		}
	}
	if entryCount >= 3 {
		return "unclaimed_rom", analysis.ConfidenceProbable
	}
	return "unclaimed_rom", analysis.ConfidenceSpeculative
}

func shadowEntryPointerRangeInTable(start, end uint32, spans []ShadowTableSpan, ownership string) bool {
	for _, span := range spans {
		if span.Ownership == ownership && start >= span.StartPC && end <= span.EndExclusive {
			return true
		}
	}
	return false
}
