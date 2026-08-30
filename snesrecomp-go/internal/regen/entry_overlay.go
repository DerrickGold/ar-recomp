package regen

import (
	"fmt"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

type entryOverlayCounts struct {
	routineRoots  int
	continuations int
}

// applyProvenEntryFacts applies two deliberately separate entry semantics.
// Routine facts withhold exact configured roots and require static call
// discovery to recreate them. Continuation facts retain their external entry
// and only open the sibling boundary in the exact named parent region.
func (repo *repository) applyProvenEntryFacts(facts []analysis.EntryFact) (entryOverlayCounts, error) {
	var counts entryOverlayCounts
	seen := make(map[decoder.Variant]analysis.EntryKind)
	for _, original := range facts {
		fact := original
		fact.Normalize()
		if len(fact.Evidence) == 0 {
			return counts, fmt.Errorf("experimental entry fact $%06X has no static proof evidence", fact.PC)
		}
		bankID := canonicalBank(repo.byBank, byte(fact.PC>>16))
		bank := repo.byBank[bankID]
		if bank == nil {
			return counts, fmt.Errorf("experimental entry fact $%06X has no configured bank", fact.PC)
		}
		variant := decoder.Variant{
			Address: decoder.Address24(bankID, uint16(fact.PC)),
			M:       fact.EntryMX.M & 1,
			X:       fact.EntryMX.X & 1,
		}
		if previous, duplicate := seen[variant]; duplicate {
			return counts, fmt.Errorf("duplicate experimental entry fact $%06X M%dX%d (%s and %s)", variant.Address, variant.M, variant.X, previous, fact.Kind)
		}
		seen[variant] = fact.Kind

		switch fact.Kind {
		case analysis.EntryRoutine:
			if err := repo.applyProvenRoutineEntryFact(bankID, bank, variant, fact); err != nil {
				return counts, err
			}
			counts.routineRoots++
		case analysis.EntryContinuation:
			if err := repo.applyProvenContinuationEntryFact(bankID, bank, variant, fact); err != nil {
				return counts, err
			}
			counts.continuations++
		default:
			return counts, fmt.Errorf("experimental entry fact $%06X has unsupported kind %q", fact.PC, fact.Kind)
		}
	}
	if repo.templateFreeEntryFacts > 0 {
		repo.rebuildNames()
	}
	return counts, nil
}

func (repo *repository) applyProvenRoutineEntryFact(bankID byte, bank *bankState, variant decoder.Variant, fact analysis.EntryFact) error {
	for _, evidence := range fact.Evidence {
		if evidence.Confidence != analysis.ConfidenceProven || !strings.HasPrefix(evidence.Source, "static.direct_js") {
			return fmt.Errorf("experimental routine entry fact $%06X has non-call proof evidence %q (%s)", fact.PC, evidence.Source, evidence.Confidence)
		}
	}
	if len(fact.RegionOwners) != 0 {
		return fmt.Errorf("experimental routine entry fact $%06X unexpectedly names region owners", fact.PC)
	}
	found := false
	for index := range bank.Config.Entries {
		entry := &bank.Config.Entries[index]
		if entryVariant(bankID, *entry) == variant {
			if obligations := entryHLEObligations(bank.Config, entry.Start); len(obligations) != 0 {
				return fmt.Errorf("experimental entry fact $%06X M%dX%d has authored HLE obligations (%s); refusing to withhold it", variant.Address, variant.M, variant.X, strings.Join(obligations, ", "))
			}
			if fact.TemplateFree {
				if blockers := entryTemplateBlockers(bankID, bank.Config, *entry); len(blockers) != 0 {
					return fmt.Errorf("experimental template-free entry fact $%06X M%dX%d has authored metadata blockers (%s)", variant.Address, variant.M, variant.X, strings.Join(blockers, ", "))
				}
				*entry = config.Entry{Start: entry.Start, EntryMX: entry.EntryMX}
				repo.removeCanonicalEntryVariant(variant)
				repo.templateFreeEntryRoots[variant] = struct{}{}
				repo.templateFreeEntryFacts++
			}
			found = true
			break
		}
	}
	if !found {
		return fmt.Errorf("experimental entry fact $%06X M%dX%d has no exact authored declaration", variant.Address, variant.M, variant.X)
	}
	if _, duplicate := repo.dormantEntryRoots[variant]; duplicate {
		return fmt.Errorf("duplicate experimental entry fact $%06X M%dX%d", variant.Address, variant.M, variant.X)
	}
	repo.dormantEntryRoots[variant] = struct{}{}
	return nil
}

func (repo *repository) applyProvenContinuationEntryFact(bankID byte, bank *bankState, target decoder.Variant, fact analysis.EntryFact) error {
	for _, evidence := range fact.Evidence {
		if evidence.Confidence != analysis.ConfidenceProven || evidence.Source != "static.sibling_boundary_edge" {
			return fmt.Errorf("experimental continuation fact $%06X has incompatible proof evidence %q (%s)", fact.PC, evidence.Source, evidence.Confidence)
		}
	}
	if !fact.TemplateFree {
		return fmt.Errorf("experimental continuation fact $%06X M%dX%d is not metadata-free", target.Address, target.M, target.X)
	}
	var targetEntry *config.Entry
	for index := range bank.Config.Entries {
		entry := &bank.Config.Entries[index]
		if entryVariant(bankID, *entry) == target {
			targetEntry = entry
			break
		}
	}
	if targetEntry == nil {
		return fmt.Errorf("experimental continuation fact $%06X M%dX%d has no exact authored declaration", target.Address, target.M, target.X)
	}
	if blockers := entryTemplateBlockers(bankID, bank.Config, *targetEntry); len(blockers) != 0 {
		return fmt.Errorf("experimental continuation fact $%06X M%dX%d has authored metadata blockers (%s)", target.Address, target.M, target.X, strings.Join(blockers, ", "))
	}
	if len(fact.RegionOwners) != 1 {
		return fmt.Errorf("experimental continuation fact $%06X M%dX%d has %d owning regions; exact region merging currently requires one", target.Address, target.M, target.X, len(fact.RegionOwners))
	}
	if len(fact.ResumeEdges) == 0 {
		return fmt.Errorf("experimental continuation fact $%06X M%dX%d has no exact resume edge", target.Address, target.M, target.X)
	}
	for _, ownerFact := range fact.RegionOwners {
		ownerBank := canonicalBank(repo.byBank, byte(ownerFact.PC>>16))
		if ownerBank != bankID {
			return fmt.Errorf("experimental continuation fact $%06X owner $%06X is not in the same canonical bank", target.Address, ownerFact.PC)
		}
		owner := decoder.Variant{
			Address: decoder.Address24(ownerBank, uint16(ownerFact.PC)),
			M:       ownerFact.EntryMX.M & 1, X: ownerFact.EntryMX.X & 1,
		}
		ownerFound := false
		var ownerEntry config.Entry
		for _, entry := range bank.Config.Entries {
			if entryVariant(bankID, entry) == owner && !repo.entryRootDormant(bankID, entry) {
				ownerFound = true
				ownerEntry = entry
				break
			}
		}
		if !ownerFound {
			return fmt.Errorf("experimental continuation fact $%06X owner $%06X M%dX%d has no active authored entry", target.Address, owner.Address, owner.M, owner.X)
		}
		if repo.provenResumeEdges[owner] == nil {
			repo.provenResumeEdges[owner] = make(map[decoder.ResumeEdge]struct{})
		}
		for _, factEdge := range fact.ResumeEdges {
			sourceBank := canonicalBank(repo.byBank, byte(factEdge.Source.PC>>16))
			targetBank := canonicalBank(repo.byBank, byte(factEdge.Target.PC>>16))
			edge := decoder.ResumeEdge{
				Source: decoder.Variant{
					Address: decoder.Address24(sourceBank, uint16(factEdge.Source.PC)),
					M:       factEdge.Source.EntryMX.M & 1, X: factEdge.Source.EntryMX.X & 1,
				},
				Target: decoder.Variant{
					Address: decoder.Address24(targetBank, uint16(factEdge.Target.PC)),
					M:       factEdge.Target.EntryMX.M & 1, X: factEdge.Target.EntryMX.X & 1,
				},
			}
			if sourceBank != bankID || targetBank != bankID || edge.Target != target {
				return fmt.Errorf("experimental continuation fact $%06X has incompatible resume edge $%06X M%dX%d -> $%06X M%dX%d", target.Address, edge.Source.Address, edge.Source.M, edge.Source.X, edge.Target.Address, edge.Target.M, edge.Target.X)
			}
			if err := repo.validateProvenResumeEdge(bank, ownerEntry, edge); err != nil {
				return fmt.Errorf("experimental continuation fact $%06X owner $%06X M%dX%d: %w", target.Address, owner.Address, owner.M, owner.X, err)
			}
			repo.provenResumeEdges[owner][edge] = struct{}{}
		}
	}
	return nil
}

func (repo *repository) validateProvenResumeEdge(bank *bankState, owner config.Entry, edge decoder.ResumeEdge) error {
	options := repo.decodeOptions(bank, owner)
	graph, err := decoder.DecodeFunction(repo.image, bank.ID, owner.Start, owner.EntryMX.M, owner.EntryMX.X, options)
	if err != nil {
		return fmt.Errorf("decode owning region: %w", err)
	}
	for key, decoded := range graph.Instructions {
		if key.PC&0xffffff != edge.Source.Address || key.M&1 != edge.Source.M || key.X&1 != edge.Source.X {
			continue
		}
		for _, successor := range decoded.Successors {
			if successor.PC&0xffffff == edge.Target.Address && successor.M&1 == edge.Target.M && successor.X&1 == edge.Target.X {
				return nil
			}
		}
	}
	return fmt.Errorf("resume edge $%06X M%dX%d -> $%06X M%dX%d is not present in the closed owner graph", edge.Source.Address, edge.Source.M, edge.Source.X, edge.Target.Address, edge.Target.M, edge.Target.X)
}

func entryHLEObligations(cfg *config.Config, pc uint16) []string {
	var result []string
	if name := cfg.HLEFunctions[pc]; name != "" {
		result = append(result, "hle_func:"+name)
	}
	if conditional := cfg.HLEFunctionsIf[pc]; conditional.Function != "" {
		result = append(result, "hle_func_if:"+conditional.Function+":"+conditional.Predicate)
	}
	for _, candidate := range cfg.HLESPCUpload {
		if candidate == pc {
			result = append(result, "hle_spc_upload")
			break
		}
	}
	sort.Strings(result)
	return result
}

func entryTemplateBlockers(bank byte, cfg *config.Config, entry config.Entry) []string {
	var result []string
	canonicalName := fmt.Sprintf("bank_%02X_%04X", bank, entry.Start)
	if entry.Name != "" && entry.Name != canonicalName {
		result = append(result, "custom_name")
	}
	if entry.End != nil {
		result = append(result, "end")
	}
	if entry.ExitMX != nil {
		result = append(result, "exit_mx")
	}
	if entry.TailCallPC != nil {
		result = append(result, "tail_call")
	}
	if entry.EntrySOffset != 0 {
		result = append(result, "entry_s_offset")
	}
	for _, obligation := range entryHLEObligations(cfg, entry.Start) {
		kind, _, _ := strings.Cut(obligation, ":")
		result = append(result, kind)
	}
	sort.Strings(result)
	return result
}

func (repo *repository) removeCanonicalEntryVariant(variant decoder.Variant) {
	variants := repo.canonical[variant.Address]
	if variants == nil {
		return
	}
	delete(variants, [2]uint8{variant.M & 1, variant.X & 1})
	if len(variants) == 0 {
		delete(repo.canonical, variant.Address)
	}
}

func (repo *repository) addCanonicalEntryVariant(variant decoder.Variant) {
	if repo.canonical[variant.Address] == nil {
		repo.canonical[variant.Address] = make(map[[2]uint8]struct{})
	}
	repo.canonical[variant.Address][[2]uint8{variant.M & 1, variant.X & 1}] = struct{}{}
}

func (repo *repository) recordStaticEntryDiscovery(variant decoder.Variant, demand variantDemandEvidence) {
	fact := repo.staticEntryDiscoveries[variant]
	if fact.PC == 0 {
		fact = analysis.EntryFact{
			PC: variant.Address, EntryMX: analysis.MXState{M: variant.M, X: variant.X}, Kind: demand.Kind,
			TemplateFree: true,
		}
	}
	if fact.Kind == "" {
		fact.Kind = demand.Kind
	} else if demand.Kind != "" && fact.Kind != demand.Kind {
		fact.Kind = ""
	}
	for _, source := range demand.Sources {
		fact.Evidence = append(fact.Evidence, analysis.Evidence{
			Source: source, Confidence: analysis.ConfidenceProven,
			Detail: "exact variant discovered without an authored declaration",
		})
	}
	fact.Normalize()
	repo.staticEntryDiscoveries[variant] = fact
}

// finalizeStaticEntryDiscoveries promotes only singleton addresses. When more
// than one generated M/X variant exists at a PC, choosing a new canonical set
// could change how pruned live states are routed; those facts remain outside
// the config-free contract until interprocedural width analysis proves the
// complete state set.
func (repo *repository) finalizeStaticEntryDiscoveries() {
	variantCounts := make(map[uint32]map[[2]uint8]struct{})
	for _, bank := range repo.banks {
		for _, entry := range bank.Config.Entries {
			address := decoder.Address24(bank.ID, entry.Start)
			if variantCounts[address] == nil {
				variantCounts[address] = make(map[[2]uint8]struct{})
			}
			variantCounts[address][[2]uint8{entry.EntryMX.M & 1, entry.EntryMX.X & 1}] = struct{}{}
		}
	}
	for variant, fact := range repo.staticEntryDiscoveries {
		fact.CanonicalPromoted = len(variantCounts[variant.Address]) == 1
		if fact.CanonicalPromoted {
			repo.addCanonicalEntryVariant(variant)
		}
		repo.staticEntryDiscoveries[variant] = fact
	}
}

func (repo *repository) sortedStaticEntryDiscoveries() []analysis.EntryFact {
	result := make([]analysis.EntryFact, 0, len(repo.staticEntryDiscoveries))
	for _, fact := range repo.staticEntryDiscoveries {
		result = append(result, fact)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].PC != result[j].PC {
			return result[i].PC < result[j].PC
		}
		if result[i].EntryMX.M != result[j].EntryMX.M {
			return result[i].EntryMX.M < result[j].EntryMX.M
		}
		return result[i].EntryMX.X < result[j].EntryMX.X
	})
	return result
}

func entryVariant(bank byte, entry config.Entry) decoder.Variant {
	return decoder.Variant{
		Address: decoder.Address24(bank, entry.Start),
		M:       entry.EntryMX.M & 1,
		X:       entry.EntryMX.X & 1,
	}
}

func (repo *repository) entryRootDormant(bank byte, entry config.Entry) bool {
	_, dormant := repo.dormantEntryRoots[entryVariant(bank, entry)]
	return dormant
}

func (repo *repository) activeSiblingAddress(bank byte, pc uint16) bool {
	state := repo.byBank[bank]
	if state == nil {
		return false
	}
	for _, entry := range state.Config.Entries {
		if entry.Start == pc && !repo.entryRootDormant(bank, entry) {
			return true
		}
	}
	return false
}

func (repo *repository) activateDemandedEntryRoot(bank byte, entry *config.Entry) bool {
	variant := entryVariant(bank, *entry)
	if _, dormant := repo.dormantEntryRoots[variant]; !dormant {
		return false
	}
	if _, templateFree := repo.templateFreeEntryRoots[variant]; templateFree {
		*entry = config.Entry{
			Name:  fmt.Sprintf("bank_%02X_%04X", bank, entry.Start),
			Start: entry.Start, EntryMX: entry.EntryMX,
		}
		repo.addCanonicalEntryVariant(variant)
		delete(repo.templateFreeEntryRoots, variant)
		repo.templateFreeSynthesized++
	}
	delete(repo.dormantEntryRoots, variant)
	return true
}

func (repo *repository) unrecoveredProvenEntryFactsError() error {
	variants := make([]decoder.Variant, 0, len(repo.dormantEntryRoots))
	for variant := range repo.dormantEntryRoots {
		variants = append(variants, variant)
	}
	sort.Slice(variants, func(i, j int) bool {
		if variants[i].Address != variants[j].Address {
			return variants[i].Address < variants[j].Address
		}
		if variants[i].M != variants[j].M {
			return variants[i].M < variants[j].M
		}
		return variants[i].X < variants[j].X
	})
	lines := make([]string, 0, len(variants))
	for _, variant := range variants {
		lines = append(lines, fmt.Sprintf("  $%02X:%04X M%dX%d", byte(variant.Address>>16), uint16(variant.Address), variant.M, variant.X))
	}
	return fmt.Errorf("experimental static entry analysis failed closed: %d withheld routine root(s) were not rediscovered:\n%s", len(variants), strings.Join(lines, "\n"))
}
