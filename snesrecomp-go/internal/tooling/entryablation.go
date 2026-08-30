package tooling

import (
	"fmt"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const (
	shadowEntryAblationVectorCovered = "vector_graph_covered"
	shadowEntryAblationRecoverable   = "batch_recoverable"
	shadowEntryAblationRetained      = "retained_static_root"
)

// ShadowEntryAblationSummary describes a report-only dependency-graph audit.
// Counts are authored declarations unless explicitly named variants or edges.
type ShadowEntryAblationSummary struct {
	AuthoredDeclarations             int `json:"authored_declarations"`
	UniqueAuthoredVariants           int `json:"unique_authored_variants"`
	StaticDependencyEdges            int `json:"static_dependency_edges"`
	VectorCoveredDeclarations        int `json:"vector_covered_declarations"`
	IndividuallyRecoverable          int `json:"individually_recoverable"`
	BatchRecoverable                 int `json:"batch_recoverable"`
	RetainedRootDeclarations         int `json:"retained_root_declarations"`
	RetainedUniqueRootVariants       int `json:"retained_unique_root_variants"`
	BatchRoutineTargets              int `json:"batch_routine_targets"`
	BatchTemplateFreeRoutines        int `json:"batch_template_free_routines"`
	BatchTailTargets                 int `json:"batch_tail_targets"`
	BatchComputedTargets             int `json:"batch_computed_targets"`
	BatchInternalContinuations       int `json:"batch_internal_continuations"`
	BatchSingleOwnerContinuations    int `json:"batch_single_owner_continuations"`
	BatchRegionEligibleContinuations int `json:"batch_region_eligible_continuations"`
	AuthoredHLEObligations           int `json:"authored_hle_obligations"`
	HLEOnlyObligations               int `json:"hle_only_obligations"`
}

type ShadowEntryAblationSource struct {
	PC      uint32               `json:"pc"`
	EntryMX analysis.MXState     `json:"entry_mx"`
	Kinds   []string             `json:"kinds"`
	Edges   []analysis.EntryEdge `json:"edges,omitempty"`
}

type ShadowEntryAblationRecord struct {
	PC                      uint32                      `json:"pc"`
	Name                    string                      `json:"name"`
	AuthoredMX              analysis.MXState            `json:"authored_mx"`
	AuthoredHLE             []string                    `json:"authored_hle,omitempty"`
	TemplateBlockers        []string                    `json:"template_blockers,omitempty"`
	Status                  string                      `json:"status"`
	EntryKindHint           string                      `json:"entry_kind_hint"`
	DecodedInstructions     int                         `json:"decoded_instructions"`
	IndividuallyRecoverable bool                        `json:"individually_recoverable"`
	Incoming                []ShadowEntryAblationSource `json:"incoming,omitempty"`
	Reason                  string                      `json:"reason"`
}

// ShadowEntryHLEObligation inventories semantic replacement policy separately
// from decode roots. Many games author HLE directives without a matching func
// line and rely on static discovery to create the generated entry body.
type ShadowEntryHLEObligation struct {
	PC            uint32 `json:"pc"`
	Directive     string `json:"directive"`
	Function      string `json:"function,omitempty"`
	Predicate     string `json:"predicate,omitempty"`
	AuthoredEntry bool   `json:"authored_entry"`
}

type ShadowEntryAblationReport struct {
	Summary        ShadowEntryAblationSummary  `json:"summary"`
	Entries        []ShadowEntryAblationRecord `json:"entries"`
	HLEObligations []ShadowEntryHLEObligation  `json:"hle_obligations,omitempty"`
	Limitations    []string                    `json:"limitations"`
}

type shadowEntryAblationDeclaration struct {
	node   decoder.Variant
	record ShadowEntryAblationRecord
}

// SelectStaticProvenRoutineEntryFacts returns the narrow subset of the
// ablation report that generation can validate without changing entry or
// continuation semantics. Each selected exact variant is batch-recoverable
// and has at least one ordinary direct JSR/JSL edge. Tail targets and computed
// handlers remain report-only; continuations use the separate exact-region
// selector below.
func SelectStaticProvenRoutineEntryFacts(report ShadowReport) []analysis.EntryFact {
	var facts []analysis.EntryFact
	for _, record := range report.EntryAblation.Entries {
		if record.Status != shadowEntryAblationRecoverable || record.EntryKindHint != "routine" || len(record.AuthoredHLE) != 0 {
			continue
		}
		fact := analysis.EntryFact{
			PC: record.PC, EntryMX: record.AuthoredMX, Kind: analysis.EntryRoutine,
			TemplateFree: len(record.TemplateBlockers) == 0,
		}
		for _, incoming := range record.Incoming {
			for _, kind := range incoming.Kinds {
				if kind != "direct_jsr" && kind != "direct_jsl" {
					continue
				}
				fact.Evidence = append(fact.Evidence, analysis.Evidence{
					Source: "static." + kind, Confidence: analysis.ConfidenceProven,
					Detail: fmt.Sprintf("$%02X:%04X", byte(incoming.PC>>16), uint16(incoming.PC)),
				})
			}
		}
		if len(fact.Evidence) == 0 {
			continue
		}
		fact.Normalize()
		facts = append(facts, fact)
	}
	sort.Slice(facts, func(i, j int) bool {
		if facts[i].PC != facts[j].PC {
			return facts[i].PC < facts[j].PC
		}
		if facts[i].EntryMX.M != facts[j].EntryMX.M {
			return facts[i].EntryMX.M < facts[j].EntryMX.M
		}
		return facts[i].EntryMX.X < facts[j].EntryMX.X
	})
	return facts
}

// SelectStaticProvenContinuationEntryFacts returns exact sibling-only entry
// ownership facts. Unlike routine facts these declarations remain active and
// externally dispatchable; the fact only permits the named parent variants to
// decode a matching continuation as a local block. Special entry metadata and
// HLE policy are excluded because a local edge would bypass their prologues.
// Region size is deliberately not a selection criterion: generation shares a
// single proven body between the owner and external continuation wrapper, then
// independently verifies that their decoded closures match.

func SelectStaticProvenContinuationEntryFacts(report ShadowReport) []analysis.EntryFact {
	var facts []analysis.EntryFact
	for _, record := range report.EntryAblation.Entries {
		if fact, valid := staticProvenContinuationEntryFact(record); valid {
			facts = append(facts, fact)
		}
	}
	sort.Slice(facts, func(i, j int) bool {
		if facts[i].PC != facts[j].PC {
			return facts[i].PC < facts[j].PC
		}
		if facts[i].EntryMX.M != facts[j].EntryMX.M {
			return facts[i].EntryMX.M < facts[j].EntryMX.M
		}
		return facts[i].EntryMX.X < facts[j].EntryMX.X
	})
	return facts
}

func staticProvenContinuationEntryFact(record ShadowEntryAblationRecord) (analysis.EntryFact, bool) {
	if record.Status != shadowEntryAblationRecoverable ||
		record.EntryKindHint != "internal_continuation" ||
		len(record.AuthoredHLE) != 0 || len(record.TemplateBlockers) != 0 ||
		len(record.Incoming) != 1 || record.DecodedInstructions <= 0 {
		return analysis.EntryFact{}, false
	}
	incoming := record.Incoming[0]
	if byte(incoming.PC>>16) != byte(record.PC>>16) || len(incoming.Kinds) == 0 || len(incoming.Edges) == 0 {
		return analysis.EntryFact{}, false
	}
	for _, kind := range incoming.Kinds {
		if kind != "sibling_boundary_edge" {
			return analysis.EntryFact{}, false
		}
	}
	fact := analysis.EntryFact{
		PC: record.PC, EntryMX: record.AuthoredMX,
		Kind: analysis.EntryContinuation, TemplateFree: true,
		RegionOwners: []analysis.EntryVariant{{
			PC: incoming.PC, EntryMX: incoming.EntryMX,
		}},
		Evidence: []analysis.Evidence{{
			Source: "static.sibling_boundary_edge", Confidence: analysis.ConfidenceProven,
			Detail: fmt.Sprintf("$%02X:%04X M%dX%d", byte(incoming.PC>>16), uint16(incoming.PC), incoming.EntryMX.M&1, incoming.EntryMX.X&1),
		}},
		ResumeEdges: append([]analysis.EntryEdge(nil), incoming.Edges...),
	}
	for _, edge := range fact.ResumeEdges {
		if edge.Target.PC&0xffffff != record.PC&0xffffff ||
			edge.Target.EntryMX.M&1 != record.AuthoredMX.M&1 ||
			edge.Target.EntryMX.X&1 != record.AuthoredMX.X&1 {
			return analysis.EntryFact{}, false
		}
	}
	fact.Normalize()
	return fact, true
}

func analyzeShadowEntryAblation(image romimage.Image, banks []shadowBank, results []shadowDecodeResult) ShadowEntryAblationReport {
	bankConfigs := make(map[byte]*config.Config, len(banks))
	var declarations []shadowEntryAblationDeclaration
	for _, bank := range banks {
		bankConfigs[bank.ID] = bank.Config
		for _, entry := range bank.Config.Entries {
			node := decoder.Variant{
				Address: decoder.Address24(bank.ID, entry.Start),
				M:       entry.EntryMX.M & 1, X: entry.EntryMX.X & 1,
			}
			declarations = append(declarations, shadowEntryAblationDeclaration{
				node: node,
				record: ShadowEntryAblationRecord{
					PC: node.Address, Name: entry.Name,
					AuthoredMX:       analysis.MXState{M: node.M, X: node.X},
					AuthoredHLE:      shadowEntryHLEObligations(bank.Config, entry.Start),
					TemplateBlockers: shadowEntryTemplateBlockers(bank.ID, bank.Config, entry),
				},
			})
		}
	}
	sort.Slice(declarations, func(i, j int) bool {
		left, right := declarations[i], declarations[j]
		if left.node.Address != right.node.Address {
			return left.node.Address < right.node.Address
		}
		if left.node.M != right.node.M {
			return left.node.M < right.node.M
		}
		if left.node.X != right.node.X {
			return left.node.X < right.node.X
		}
		return left.record.Name < right.record.Name
	})

	graph := make(map[decoder.Variant]map[decoder.Variant]struct{})
	reverse := make(map[decoder.Variant]map[decoder.Variant][]string)
	reverseEdges := make(map[decoder.Variant]map[decoder.Variant][]analysis.EntryEdge)
	decodedInstructions := make(map[decoder.Variant]int)
	for _, result := range results {
		source, valid := canonicalShadowAblationVariant(result.entry, bankConfigs)
		if !valid {
			continue
		}
		if len(result.instructions) > decodedInstructions[source] {
			decodedInstructions[source] = len(result.instructions)
		}
		if graph[source] == nil {
			graph[source] = make(map[decoder.Variant]struct{})
		}
		for demand := range result.demands {
			target, targetValid := canonicalShadowAblationVariant(demand, bankConfigs)
			if !targetValid {
				continue
			}
			graph[source][target] = struct{}{}
			if reverse[target] == nil {
				reverse[target] = make(map[decoder.Variant][]string)
			}
			kinds := result.demandEvidence[demand]
			if len(kinds) == 0 {
				kinds = []string{"static_demand"}
			}
			for _, kind := range kinds {
				reverse[target][source] = appendUniqueShadowString(reverse[target][source], kind)
			}
			sort.Strings(reverse[target][source])
			if len(result.resumeEdges[demand]) != 0 {
				if reverseEdges[target] == nil {
					reverseEdges[target] = make(map[decoder.Variant][]analysis.EntryEdge)
				}
				for _, edge := range result.resumeEdges[demand] {
					found := false
					for _, existing := range reverseEdges[target][source] {
						if existing == edge {
							found = true
							break
						}
					}
					if !found {
						reverseEdges[target][source] = append(reverseEdges[target][source], edge)
					}
				}
			}
		}
	}

	vectorRoots := make(map[decoder.Variant]int)
	if bankConfigs[0] != nil {
		for _, entry := range appendShadowAutoVectorEntries(image, nil) {
			vectorRoots[decoder.Variant{
				Address: decoder.Address24(0, entry.Start),
				M:       entry.EntryMX.M & 1, X: entry.EntryMX.X & 1,
			}]++
		}
	}
	vectorReachable := shadowEntryAblationReachable(graph, vectorRoots)

	allRoots := make(map[decoder.Variant]int)
	for _, declaration := range declarations {
		allRoots[declaration.node]++
	}
	individuallyRecoverable := make([]bool, len(declarations))
	for index, declaration := range declarations {
		allRoots[declaration.node]--
		if allRoots[declaration.node] == 0 {
			delete(allRoots, declaration.node)
		}
		individualReachable := shadowEntryAblationReachableWithVectors(graph, allRoots, vectorRoots)
		individuallyRecoverable[index] = individualReachable[declaration.node]
		allRoots[declaration.node]++
	}

	retained := make([]bool, len(declarations))
	for index := range retained {
		retained[index] = true
	}
	retainedRoots := cloneShadowEntryAblationRoots(allRoots)
	for index := len(declarations) - 1; index >= 0; index-- {
		declaration := declarations[index]
		retained[index] = false
		retainedRoots[declaration.node]--
		if retainedRoots[declaration.node] == 0 {
			delete(retainedRoots, declaration.node)
		}
		reachable := shadowEntryAblationReachableWithVectors(graph, retainedRoots, vectorRoots)
		if shadowEntryAblationCoversDeclarations(reachable, declarations) {
			continue
		}
		retained[index] = true
		retainedRoots[declaration.node]++
	}

	report := ShadowEntryAblationReport{
		Summary:        ShadowEntryAblationSummary{AuthoredDeclarations: len(declarations)},
		HLEObligations: shadowCollectEntryHLEObligations(banks),
		Limitations: []string{
			"dependency edges are exact finite decode demands collected while the full authored entry boundary set is present",
			"individual recoverability means the declaration's exact (PC,M,X) variant remains graph-reachable when only that declaration is withheld",
			"the retained root set is deterministic and inclusion-minimal in descending declaration order, not guaranteed to have minimum cardinality",
			"graph reachability does not prove an external entry kind, runtime reachability, or generated-region equivalence; no declaration is removed or fed into regeneration",
		},
	}
	report.Summary.AuthoredHLEObligations = len(report.HLEObligations)
	for _, obligation := range report.HLEObligations {
		if !obligation.AuthoredEntry {
			report.Summary.HLEOnlyObligations++
		}
	}
	uniqueAuthored := make(map[decoder.Variant]struct{})
	uniqueRetained := make(map[decoder.Variant]struct{})
	edges := 0
	for _, targets := range graph {
		edges += len(targets)
	}
	report.Summary.StaticDependencyEdges = edges
	for index, declaration := range declarations {
		uniqueAuthored[declaration.node] = struct{}{}
		record := declaration.record
		record.DecodedInstructions = decodedInstructions[declaration.node]
		record.IndividuallyRecoverable = individuallyRecoverable[index]
		record.Incoming = shadowEntryAblationIncoming(reverse[declaration.node], reverseEdges[declaration.node])
		record.EntryKindHint = shadowEntryAblationKindHint(declaration.node, record.Incoming, vectorRoots)
		switch {
		case vectorReachable[declaration.node]:
			record.Status = shadowEntryAblationVectorCovered
			record.Reason = "the exact authored variant is reachable from a hardware vector through static dependency edges"
			report.Summary.VectorCoveredDeclarations++
		case retained[index]:
			record.Status = shadowEntryAblationRetained
			record.Reason = "removing this declaration from the deterministic batch root set leaves at least one authored variant outside the static dependency closure"
			report.Summary.RetainedRootDeclarations++
			uniqueRetained[declaration.node] = struct{}{}
		default:
			record.Status = shadowEntryAblationRecoverable
			record.Reason = "the exact authored variant remains reachable from hardware vectors or the retained static root set"
			report.Summary.BatchRecoverable++
			switch record.EntryKindHint {
			case "routine":
				report.Summary.BatchRoutineTargets++
				if len(record.TemplateBlockers) == 0 {
					report.Summary.BatchTemplateFreeRoutines++
				}
			case "tail_target":
				report.Summary.BatchTailTargets++
			case "computed_handler":
				report.Summary.BatchComputedTargets++
			case "internal_continuation":
				report.Summary.BatchInternalContinuations++
			}
		}
		if record.Status == shadowEntryAblationRecoverable && record.EntryKindHint == "internal_continuation" {
			if len(record.Incoming) == 1 {
				report.Summary.BatchSingleOwnerContinuations++
			}
			if _, eligible := staticProvenContinuationEntryFact(record); eligible {
				report.Summary.BatchRegionEligibleContinuations++
			}
		}
		if record.IndividuallyRecoverable {
			report.Summary.IndividuallyRecoverable++
		}
		report.Entries = append(report.Entries, record)
	}
	report.Summary.UniqueAuthoredVariants = len(uniqueAuthored)
	report.Summary.RetainedUniqueRootVariants = len(uniqueRetained)
	return report
}

func shadowEntryHLEObligations(cfg *config.Config, pc uint16) []string {
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

func shadowEntryTemplateBlockers(bank byte, cfg *config.Config, entry config.Entry) []string {
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
	for _, obligation := range shadowEntryHLEObligations(cfg, entry.Start) {
		kind, _, _ := strings.Cut(obligation, ":")
		result = append(result, kind)
	}
	sort.Strings(result)
	return result
}

func shadowCollectEntryHLEObligations(banks []shadowBank) []ShadowEntryHLEObligation {
	var result []ShadowEntryHLEObligation
	for _, bank := range banks {
		authored := make(map[uint16]struct{}, len(bank.Config.Entries))
		for _, entry := range bank.Config.Entries {
			authored[entry.Start] = struct{}{}
		}
		appendObligation := func(pc uint16, directive, function, predicate string) {
			_, hasEntry := authored[pc]
			result = append(result, ShadowEntryHLEObligation{
				PC: decoder.Address24(bank.ID, pc), Directive: directive,
				Function: function, Predicate: predicate, AuthoredEntry: hasEntry,
			})
		}
		for pc, function := range bank.Config.HLEFunctions {
			appendObligation(pc, "hle_func", function, "")
		}
		for pc, conditional := range bank.Config.HLEFunctionsIf {
			appendObligation(pc, "hle_func_if", conditional.Function, conditional.Predicate)
		}
		for _, pc := range bank.Config.HLESPCUpload {
			appendObligation(pc, "hle_spc_upload", "", "")
		}
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].PC != result[j].PC {
			return result[i].PC < result[j].PC
		}
		if result[i].Directive != result[j].Directive {
			return result[i].Directive < result[j].Directive
		}
		if result[i].Function != result[j].Function {
			return result[i].Function < result[j].Function
		}
		return result[i].Predicate < result[j].Predicate
	})
	return result
}

func shadowEntryAblationKindHint(node decoder.Variant, incoming []ShadowEntryAblationSource, vectorRoots map[decoder.Variant]int) string {
	if vectorRoots[node] > 0 {
		return "reset_interrupt"
	}
	kinds := make(map[string]struct{})
	for _, source := range incoming {
		for _, kind := range source.Kinds {
			kinds[kind] = struct{}{}
		}
	}
	if _, found := kinds["static_computed_dispatch"]; found {
		return "computed_handler"
	}
	if _, found := kinds["direct_jsr"]; found {
		return "routine"
	}
	if _, found := kinds["direct_jsl"]; found {
		return "routine"
	}
	if _, found := kinds["direct_long_jump"]; found {
		return "tail_target"
	}
	if _, found := kinds["direct_tail_jump"]; found {
		return "tail_target"
	}
	if _, found := kinds["sibling_boundary_edge"]; found {
		return "internal_continuation"
	}
	return "external_root_unknown"
}

func canonicalShadowAblationVariant(variant decoder.Variant, bankConfigs map[byte]*config.Config) (decoder.Variant, bool) {
	address := variant.Address & 0xffffff
	bank := shadowCanonicalBank(bankConfigs, byte(address>>16))
	if bankConfigs[bank] == nil {
		return decoder.Variant{}, false
	}
	variant.Address = decoder.Address24(bank, uint16(address))
	variant.M &= 1
	variant.X &= 1
	return variant, true
}

func shadowEntryAblationReachableWithVectors(graph map[decoder.Variant]map[decoder.Variant]struct{}, authored, vectors map[decoder.Variant]int) map[decoder.Variant]bool {
	roots := cloneShadowEntryAblationRoots(authored)
	for root, count := range vectors {
		roots[root] += count
	}
	return shadowEntryAblationReachable(graph, roots)
}

func shadowEntryAblationReachable(graph map[decoder.Variant]map[decoder.Variant]struct{}, roots map[decoder.Variant]int) map[decoder.Variant]bool {
	reachable := make(map[decoder.Variant]bool)
	queue := make([]decoder.Variant, 0, len(roots))
	for root, count := range roots {
		if count <= 0 || reachable[root] {
			continue
		}
		reachable[root] = true
		queue = append(queue, root)
	}
	for len(queue) > 0 {
		source := queue[0]
		queue = queue[1:]
		for target := range graph[source] {
			if reachable[target] {
				continue
			}
			reachable[target] = true
			queue = append(queue, target)
		}
	}
	return reachable
}

func shadowEntryAblationCoversDeclarations(reachable map[decoder.Variant]bool, declarations []shadowEntryAblationDeclaration) bool {
	for _, declaration := range declarations {
		if !reachable[declaration.node] {
			return false
		}
	}
	return true
}

func cloneShadowEntryAblationRoots(values map[decoder.Variant]int) map[decoder.Variant]int {
	result := make(map[decoder.Variant]int, len(values))
	for value, count := range values {
		result[value] = count
	}
	return result
}

func shadowEntryAblationIncoming(values map[decoder.Variant][]string, edges map[decoder.Variant][]analysis.EntryEdge) []ShadowEntryAblationSource {
	result := make([]ShadowEntryAblationSource, 0, len(values))
	for value, kinds := range values {
		result = append(result, ShadowEntryAblationSource{
			PC:      value.Address & 0xffffff,
			EntryMX: analysis.MXState{M: value.M & 1, X: value.X & 1},
			Kinds:   append([]string(nil), kinds...),
			Edges:   append([]analysis.EntryEdge(nil), edges[value]...),
		})
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
