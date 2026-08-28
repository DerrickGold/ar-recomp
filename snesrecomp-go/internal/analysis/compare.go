package analysis

import (
	"fmt"
	"slices"
	"sort"
)

type ComparisonStatus string

const (
	ComparisonExact        ComparisonStatus = "exact_match"
	ComparisonCompatible   ComparisonStatus = "compatible_guard"
	ComparisonPartial      ComparisonStatus = "partial_match"
	ComparisonConflict     ComparisonStatus = "conflict"
	ComparisonAuthoredOnly ComparisonStatus = "authored_only"
	ComparisonAutomatic    ComparisonStatus = "automatic"
	ComparisonGarbageOnly  ComparisonStatus = "garbage_only"
)

type Comparison struct {
	SitePC      uint32           `json:"site_pc"`
	Status      ComparisonStatus `json:"status"`
	Authored    *DispatchFact    `json:"authored,omitempty"`
	Inferred    *DispatchFact    `json:"inferred,omitempty"`
	Differences []string         `json:"differences,omitempty"`
}

type ComparisonSummary struct {
	AuthoredFacts  int `json:"authored_facts"`
	InferredFacts  int `json:"inferred_facts"`
	ExactMatches   int `json:"exact_matches"`
	Compatible     int `json:"compatible_guards"`
	PartialMatches int `json:"partial_matches"`
	Conflicts      int `json:"conflicts"`
	AuthoredOnly   int `json:"authored_only"`
	Automatic      int `json:"automatic"`
	GarbageOnly    int `json:"garbage_only"`
}

// CompareDispatchFacts compares independent inference with authored policy.
// An unknown inferred field produces a partial match, never an exact match or
// a speculative conflict.
func CompareDispatchFacts(authored, inferred []DispatchFact) ([]Comparison, ComparisonSummary) {
	authoredBySite := factsBySite(authored)
	inferredBySite := factsBySite(inferred)
	sites := make([]uint32, 0, len(authoredBySite)+len(inferredBySite))
	seen := make(map[uint32]struct{}, cap(sites))
	for site := range authoredBySite {
		seen[site] = struct{}{}
		sites = append(sites, site)
	}
	for site := range inferredBySite {
		if _, found := seen[site]; !found {
			sites = append(sites, site)
		}
	}
	sort.Slice(sites, func(i, j int) bool { return sites[i] < sites[j] })

	summary := ComparisonSummary{AuthoredFacts: len(authoredBySite), InferredFacts: len(inferredBySite)}
	continuationUniverse := inferredContinuationUniverse(inferred)
	comparisons := make([]Comparison, 0, len(sites))
	for _, site := range sites {
		auth, hasAuthored := authoredBySite[site]
		inf, hasInferred := inferredBySite[site]
		comparison := Comparison{SitePC: site}
		if hasAuthored {
			copy := auth
			comparison.Authored = &copy
		}
		if hasInferred {
			copy := inf
			comparison.Inferred = &copy
		}
		switch {
		case hasAuthored && !hasInferred:
			if authoredGuardCovered(auth, continuationUniverse) {
				comparison.Status = ComparisonCompatible
				comparison.Differences = []string{"authored safety guard contains independently proven same-bank continuations; no site-specific edge is asserted"}
				summary.Compatible++
			} else {
				comparison.Status = ComparisonAuthoredOnly
				comparison.Differences = []string{"no independent site-to-target proof"}
				summary.AuthoredOnly++
			}
		case !hasAuthored && hasInferred:
			if inf.CodeOwnership == OwnershipGarbageOnly {
				comparison.Status = ComparisonGarbageOnly
				comparison.Differences = []string{"the control-flow instruction exists only in a conflicting overlapping decode"}
				summary.GarbageOnly++
			} else {
				comparison.Status = ComparisonAutomatic
				summary.Automatic++
			}
		default:
			comparison.Status, comparison.Differences = compareFact(auth, inf)
			switch comparison.Status {
			case ComparisonExact:
				summary.ExactMatches++
			case ComparisonCompatible:
				summary.Compatible++
			case ComparisonPartial:
				summary.PartialMatches++
			case ComparisonConflict:
				summary.Conflicts++
			}
		}
		comparisons = append(comparisons, comparison)
	}
	return comparisons, summary
}

func compareFact(authored, inferred DispatchFact) (ComparisonStatus, []string) {
	var conflicts, unknowns, compatible []string
	compareKnown := func(field string, equal bool, description string) {
		if inferred.FieldUnknown(field) {
			unknowns = append(unknowns, field+" is not yet proven")
			return
		}
		if !equal {
			conflicts = append(conflicts, description)
		}
	}
	compareKnown("transfer", authored.Transfer == inferred.Transfer,
		fmt.Sprintf("transfer differs: authored=%s inferred=%s", authored.Transfer, inferred.Transfer))
	compareKnown("target_entry_kind", authored.TargetEntryKind == inferred.TargetEntryKind,
		fmt.Sprintf("target entry kind differs: authored=%s inferred=%s", authored.TargetEntryKind, inferred.TargetEntryKind))
	compareKnown("index_register", authored.IndexRegister == inferred.IndexRegister,
		fmt.Sprintf("index register differs: authored=%s inferred=%s", authored.IndexRegister, inferred.IndexRegister))
	compareKnown("table_bases", slices.Equal(authored.TableBases, inferred.TableBases),
		fmt.Sprintf("table bases differ: authored=%v inferred=%v", authored.TableBases, inferred.TableBases))
	compareKnown("return_pc", equalOptionalAddress(authored.ReturnPC, inferred.ReturnPC),
		fmt.Sprintf("return PC differs: authored=%s inferred=%s", optionalAddress(authored.ReturnPC), optionalAddress(inferred.ReturnPC)))
	compareKnown("sep_mask", authored.SEPMask == inferred.SEPMask,
		fmt.Sprintf("SEP mask differs: authored=$%02X inferred=$%02X", authored.SEPMask, inferred.SEPMask))

	if inferred.TargetSetClosed && !inferred.FieldUnknown("targets") {
		if !slices.Equal(authored.Targets, inferred.Targets) {
			if authored.TargetEntryKind == EntryContinuation && addressSubset(inferred.Targets, authored.Targets) {
				compatible = append(compatible, "independently proven site-specific continuations are contained by the authored safety guard")
			} else {
				conflicts = append(conflicts, fmt.Sprintf("closed target set differs: authored=%v inferred=%v", authored.Targets, inferred.Targets))
			}
		}
	} else {
		unknowns = append(unknowns, "target-set bound is not yet proven")
		if len(inferred.TargetCandidates) > 0 && !hasPrefix(inferred.TargetCandidates, authored.Targets) {
			unknowns = append(unknowns, "recovered target candidates do not have the authored set as a prefix")
		}
	}
	unknowns = append(unknowns, inferred.UnknownFields...)
	unknowns = uniqueStrings(unknowns)
	if len(conflicts) > 0 {
		return ComparisonConflict, append(conflicts, unknowns...)
	}
	if len(unknowns) > 0 {
		return ComparisonPartial, unknowns
	}
	if len(compatible) > 0 {
		return ComparisonCompatible, compatible
	}
	return ComparisonExact, nil
}

func inferredContinuationUniverse(facts []DispatchFact) map[byte]map[uint32]struct{} {
	result := make(map[byte]map[uint32]struct{})
	for _, fact := range facts {
		fact.Normalize()
		if fact.TargetEntryKind != EntryContinuation || !fact.TargetSetClosed || fact.FieldUnknown("targets") {
			continue
		}
		bank := byte(fact.SitePC >> 16)
		values := result[bank]
		if values == nil {
			values = make(map[uint32]struct{})
			result[bank] = values
		}
		for _, target := range fact.Targets {
			if byte(target>>16) == bank {
				values[target&0xffffff] = struct{}{}
			}
		}
	}
	return result
}

func authoredGuardCovered(fact DispatchFact, universe map[byte]map[uint32]struct{}) bool {
	if fact.TargetEntryKind != EntryContinuation || fact.Mnemonic != "RTS" || len(fact.Targets) == 0 {
		return false
	}
	bank := byte(fact.SitePC >> 16)
	values := universe[bank]
	for _, target := range fact.Targets {
		if _, found := values[target&0xffffff]; !found {
			return false
		}
	}
	return true
}

func addressSubset(subset, superset []uint32) bool {
	if len(subset) == 0 || len(subset) > len(superset) {
		return false
	}
	values := make(map[uint32]struct{}, len(superset))
	for _, value := range superset {
		values[value&0xffffff] = struct{}{}
	}
	for _, value := range subset {
		if _, found := values[value&0xffffff]; !found {
			return false
		}
	}
	return true
}

func factsBySite(facts []DispatchFact) map[uint32]DispatchFact {
	result := make(map[uint32]DispatchFact, len(facts))
	for _, fact := range facts {
		fact.Normalize()
		result[fact.SitePC] = fact
	}
	return result
}

func equalOptionalAddress(left, right *uint32) bool {
	if left == nil || right == nil {
		return left == nil && right == nil
	}
	return *left&0xffffff == *right&0xffffff
}

func optionalAddress(value *uint32) string {
	if value == nil {
		return "none"
	}
	return fmt.Sprintf("$%06X", *value&0xffffff)
}

func hasPrefix(candidates, wanted []uint32) bool {
	if len(candidates) < len(wanted) {
		return false
	}
	return slices.Equal(candidates[:len(wanted)], wanted)
}

func uniqueStrings(values []string) []string {
	sort.Strings(values)
	result := values[:0]
	for _, value := range values {
		if len(result) == 0 || value != result[len(result)-1] {
			result = append(result, value)
		}
	}
	return result
}
