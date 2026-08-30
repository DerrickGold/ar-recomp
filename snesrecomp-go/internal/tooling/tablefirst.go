package tooling

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const (
	shadowTableFirstMaxInteriorWords = 2
	shadowTableFirstMissingInterior  = "missing_interior"
	shadowTableFirstMissingBefore    = "missing_before"
	shadowTableFirstMissingAfter     = "missing_after"
	shadowTableFirstAbsolute16       = "absolute16"
	shadowTableFirstBasePlusU16      = "base_plus_u16"
)

// ShadowTableFirstTarget is an unknown same-bank pointer value found next to
// authored entry pointers and independently corroborated by the landing sweep.
// It is review-only: neither the pointer cluster nor the landing shape proves a
// runtime index, table bound, or live M/X state.
type ShadowTableFirstTarget struct {
	TargetPC          uint32                   `json:"target_pc"`
	AnchorPC          uint32                   `json:"anchor_pc"`
	EntryMX           []analysis.MXState       `json:"entry_mx"`
	Classification    string                   `json:"classification"`
	LandingSeed       string                   `json:"landing_seed"`
	LandingBoundary   string                   `json:"landing_boundary"`
	LandingOwnership  string                   `json:"landing_ownership"`
	LandingConfidence analysis.Confidence      `json:"landing_confidence"`
	Confidence        analysis.Confidence      `json:"confidence"`
	Sources           []ShadowTableFirstSource `json:"sources"`
	Reason            string                   `json:"reason"`
}

// ShadowTableFirstSource describes the smallest anchored ROM word window that
// exposed an otherwise unknown target value.
type ShadowTableFirstSource struct {
	Kind                 string              `json:"kind"`
	Encoding             string              `json:"encoding"`
	BasePC               uint32              `json:"base_pc,omitempty"`
	BaseEvidencePCs      []uint32            `json:"base_evidence_pcs,omitempty"`
	WordPC               uint32              `json:"word_pc"`
	StartPC              uint32              `json:"start_pc"`
	EndExclusive         uint32              `json:"end_exclusive"`
	KnownEntries         int                 `json:"known_entries"`
	DistinctKnownTargets int                 `json:"distinct_known_targets"`
	Ownership            string              `json:"ownership"`
	Confidence           analysis.Confidence `json:"confidence"`
}

// ShadowTableFirstRejection keeps an anchored pointer seed visible when the
// semantic landing checks reject it. This prevents a quiet zero-result report
// from conflating absent pointer evidence with failed code-shape evidence.
type ShadowTableFirstRejection struct {
	TargetPC uint32                   `json:"target_pc"`
	Reason   string                   `json:"reason"`
	Sources  []ShadowTableFirstSource `json:"sources"`
}

type shadowTableFirstKnownHit struct {
	offset int
	target uint16
}

type shadowTableFirstEncoding struct {
	kind     string
	base     uint16
	evidence []uint32
}

type shadowTableFirstStats struct {
	pointerSeeds          int
	absoluteSeeds         int
	baseOffsetSeeds       int
	baseEvidenceBases     int
	baseEvidenceSites     int
	postTerminatorMatches int
	pointerWindowMatches  int
	internalMatches       int
	absoluteTargets       int
	baseOffsetTargets     int
	rejectedSeeds         int
}

type shadowTableFirstProbeContext struct {
	image        romimage.Image
	regions      []decoder.DataRegion
	calleeExitMX map[decoder.Variant]decoder.MX
	configByBank map[byte]*config.Config
	anchors      map[byte][]uint16
	owned        map[uint32]struct{}
	starts       map[uint32][]analysis.MXState
	tableSpans   []ShadowTableSpan
}

func collectShadowTableFirstTargets(image romimage.Image, banks []shadowBank, entries []ShadowEntryRecoveryRecord, landings []ShadowLandingCandidate, results []shadowDecodeResult, regions []decoder.DataRegion, tableSpans []ShadowTableSpan, calleeExitMX map[decoder.Variant]decoder.MX) ([]ShadowTableFirstTarget, []ShadowTableFirstRejection, shadowTableFirstStats) {
	knownByBank := make(map[byte]map[uint16]struct{})
	for _, entry := range entries {
		pc := entry.PC & 0xffffff
		bank := byte(pc >> 16)
		if knownByBank[bank] == nil {
			knownByBank[bank] = make(map[uint16]struct{})
		}
		knownByBank[bank][uint16(pc)] = struct{}{}
	}
	landingByPC := make(map[uint32]ShadowLandingCandidate, len(landings))
	for _, landing := range landings {
		landingByPC[landing.CandidateEntryPC&0xffffff] = landing
	}
	owned := make(map[uint32]struct{})
	for _, result := range results {
		for _, span := range result.spans {
			for offset := uint8(0); offset < span.Length; offset++ {
				owned[(span.PC&0xff0000)|uint32(uint16(span.PC)+uint16(offset))] = struct{}{}
			}
		}
	}
	baseEvidence := collectShadowTableBaseEvidence(results)
	baseEvidenceBases, baseEvidenceSites := countShadowTableBaseEvidence(baseEvidence)

	sourcesByTarget := make(map[uint32][]ShadowTableFirstSource)
	for bank, known := range knownByBank {
		bankOffset := int(bank&0x7f) * 0x8000
		if bankOffset >= len(image) {
			continue
		}
		bankLength := min(0x8000, len(image)-bankOffset)
		collectShadowTableFirstEncodedSources(image, bank, bankOffset, bankLength, known,
			shadowTableFirstEncoding{kind: shadowTableFirstAbsolute16}, owned, tableSpans, sourcesByTarget)
		bases := make([]uint16, 0, len(baseEvidence[bank]))
		for base := range baseEvidence[bank] {
			if _, authoredBase := known[base]; !authoredBase {
				continue
			}
			bases = append(bases, base)
		}
		sort.Slice(bases, func(i, j int) bool { return bases[i] < bases[j] })
		for _, base := range bases {
			collectShadowTableFirstEncodedSources(image, bank, bankOffset, bankLength, known,
				shadowTableFirstEncoding{kind: shadowTableFirstBasePlusU16, base: base, evidence: baseEvidence[bank][base]}, owned, tableSpans, sourcesByTarget)
		}
	}

	probe := newShadowTableFirstProbeContext(image, banks, results, regions, tableSpans, calleeExitMX, owned)
	findings := make(map[uint32]ShadowTableFirstTarget)
	var rejections []ShadowTableFirstRejection
	stats := shadowTableFirstStats{
		pointerSeeds: len(sourcesByTarget), baseEvidenceBases: baseEvidenceBases, baseEvidenceSites: baseEvidenceSites,
	}
	for targetPC, sources := range sourcesByTarget {
		absolute, baseOffset := shadowTableFirstSourceEncodings(sources)
		if absolute {
			stats.absoluteSeeds++
		}
		if baseOffset {
			stats.baseOffsetSeeds++
		}
		if entryMX, internal := probe.starts[targetPC]; internal {
			findings[targetPC] = shadowTableFirstInternalFinding(targetPC, entryMX, sources)
			stats.internalMatches++
			if absolute {
				stats.absoluteTargets++
			}
			if baseOffset {
				stats.baseOffsetTargets++
			}
			continue
		}
		landing, found := landingByPC[targetPC]
		landingSeed := "post_terminator"
		landingBoundary := "confirmed_entry"
		if found {
			stats.postTerminatorMatches++
		} else {
			var rejectionReason string
			landing, landingBoundary, rejectionReason, found = probe.probe(targetPC)
			landingSeed = "pointer_window"
			if found {
				stats.pointerWindowMatches++
			} else {
				rejections = append(rejections, ShadowTableFirstRejection{TargetPC: targetPC, Reason: rejectionReason, Sources: sources})
			}
		}
		if !found {
			stats.rejectedSeeds++
			continue
		}
		finding := shadowTableFirstFinding(targetPC, landingSeed, landingBoundary, landing, sources)
		findings[targetPC] = finding
		if absolute {
			stats.absoluteTargets++
		}
		if baseOffset {
			stats.baseOffsetTargets++
		}
	}

	result := make([]ShadowTableFirstTarget, 0, len(findings))
	for _, finding := range findings {
		sort.Slice(finding.Sources, func(i, j int) bool {
			left, right := finding.Sources[i], finding.Sources[j]
			if left.WordPC != right.WordPC {
				return left.WordPC < right.WordPC
			}
			if left.StartPC != right.StartPC {
				return left.StartPC < right.StartPC
			}
			if left.EndExclusive != right.EndExclusive {
				return left.EndExclusive < right.EndExclusive
			}
			if left.Kind != right.Kind {
				return left.Kind < right.Kind
			}
			if left.Encoding != right.Encoding {
				return left.Encoding < right.Encoding
			}
			return left.BasePC < right.BasePC
		})
		result = append(result, finding)
	}
	sort.Slice(result, func(i, j int) bool { return result[i].TargetPC < result[j].TargetPC })
	sort.Slice(rejections, func(i, j int) bool { return rejections[i].TargetPC < rejections[j].TargetPC })
	return result, rejections, stats
}

func shadowTableFirstSourceEncodings(sources []ShadowTableFirstSource) (absolute, baseOffset bool) {
	for _, source := range sources {
		switch source.Encoding {
		case shadowTableFirstAbsolute16:
			absolute = true
		case shadowTableFirstBasePlusU16:
			baseOffset = true
		}
	}
	return absolute, baseOffset
}

func collectShadowTableBaseEvidence(results []shadowDecodeResult) map[byte]map[uint16][]uint32 {
	evidence := make(map[byte]map[uint16][]uint32)
	for _, result := range results {
		predecessors := make(map[uint32][]shadowDecodedInstruction)
		for _, instruction := range result.instructions {
			end := instruction.PC + uint32(instruction.Instruction.Length)
			predecessors[end&0xffffff] = append(predecessors[end&0xffffff], instruction)
		}
		for _, instruction := range result.instructions {
			decoded := instruction.Instruction
			if decoded.Mnemonic != "ADC" || decoded.Mode != cpu65816.IMM || decoded.Length != 3 || instruction.M != 0 || decoded.Operand < 0x8000 {
				continue
			}
			provenCarryClear := false
			for _, predecessor := range predecessors[instruction.PC&0xffffff] {
				if predecessor.FunctionEntry == instruction.FunctionEntry && predecessor.M == instruction.M && predecessor.X == instruction.X &&
					predecessor.Instruction.Mnemonic == "CLC" {
					provenCarryClear = true
					break
				}
			}
			if !provenCarryClear {
				continue
			}
			bank := byte(instruction.PC >> 16)
			base := uint16(decoded.Operand)
			if evidence[bank] == nil {
				evidence[bank] = make(map[uint16][]uint32)
			}
			evidence[bank][base] = appendUniqueShadowAddress(evidence[bank][base], instruction.PC&0xffffff)
		}
	}
	for bank := range evidence {
		for base := range evidence[bank] {
			sort.Slice(evidence[bank][base], func(i, j int) bool { return evidence[bank][base][i] < evidence[bank][base][j] })
		}
	}
	return evidence
}

func countShadowTableBaseEvidence(evidence map[byte]map[uint16][]uint32) (bases, sites int) {
	for _, byBase := range evidence {
		bases += len(byBase)
		for _, values := range byBase {
			sites += len(values)
		}
	}
	return bases, sites
}

func appendUniqueShadowAddress(values []uint32, incoming uint32) []uint32 {
	for _, value := range values {
		if value == incoming {
			return values
		}
	}
	return append(values, incoming)
}

func collectShadowTableFirstEncodedSources(image romimage.Image, bank byte, bankOffset, bankLength int, known map[uint16]struct{}, encoding shadowTableFirstEncoding, owned map[uint32]struct{}, tableSpans []ShadowTableSpan, sources map[uint32][]ShadowTableFirstSource) {
	for parity := 0; parity < 2; parity++ {
		var hits []shadowTableFirstKnownHit
		for relative := parity; relative+1 < bankLength; relative += 2 {
			value := shadowTableFirstWord(image, bankOffset+relative)
			target, valid := encoding.resolve(value)
			if !valid {
				continue
			}
			if _, found := known[target]; found {
				hits = append(hits, shadowTableFirstKnownHit{offset: relative, target: target})
			}
		}
		if encoding.kind == shadowTableFirstBasePlusU16 {
			collectShadowBaseOffsetInterior(image, bank, bankOffset, known, hits, encoding, owned, tableSpans, sources)
			continue
		}
		collectShadowTableFirstInterior(image, bank, bankOffset, known, hits, encoding, owned, tableSpans, sources)
		collectShadowTableFirstEdges(image, bank, bankOffset, bankLength, known, hits, encoding, owned, tableSpans, sources)
	}
}

func (encoding shadowTableFirstEncoding) resolve(value uint16) (uint16, bool) {
	switch encoding.kind {
	case shadowTableFirstAbsolute16:
		return value, value >= 0x8000 && value != 0xffff
	case shadowTableFirstBasePlusU16:
		target := uint32(encoding.base) + uint32(value)
		if target > 0xffff {
			return 0, false
		}
		return uint16(target), target >= 0x8000
	default:
		return 0, false
	}
}

func collectShadowBaseOffsetInterior(image romimage.Image, bank byte, bankOffset int, known map[uint16]struct{}, hits []shadowTableFirstKnownHit, encoding shadowTableFirstEncoding, owned map[uint32]struct{}, tableSpans []ShadowTableSpan, sources map[uint32][]ShadowTableFirstSource) {
	for index := 0; index+1 < len(hits); index++ {
		left, right := hits[index], hits[index+1]
		missing := (right.offset-left.offset)/2 - 1
		if missing < 1 || missing > shadowTableFirstMaxInteriorWords || left.target == right.target {
			continue
		}
		contextStart, contextEnd := index, index+1
		if contextStart > 0 && hits[contextStart].offset-hits[contextStart-1].offset == 2 {
			contextStart--
		}
		if contextEnd+1 < len(hits) && hits[contextEnd+1].offset-hits[contextEnd].offset == 2 {
			contextEnd++
		}
		distinct := make(map[uint16]struct{})
		containsBase := false
		for contextIndex := contextStart; contextIndex <= contextEnd; contextIndex++ {
			distinct[hits[contextIndex].target] = struct{}{}
			if hits[contextIndex].target == encoding.base {
				containsBase = true
			}
		}
		if !containsBase || len(distinct) < 3 {
			continue
		}
		for relative := left.offset + 2; relative < right.offset; relative += 2 {
			addShadowTableFirstSource(image, bank, bankOffset, relative, hits[contextStart].offset, hits[contextEnd].offset+2,
				shadowTableFirstMissingInterior, contextEnd-contextStart+1, len(distinct), encoding, known, owned, tableSpans, sources)
		}
	}
}

func collectShadowTableFirstInterior(image romimage.Image, bank byte, bankOffset int, known map[uint16]struct{}, hits []shadowTableFirstKnownHit, encoding shadowTableFirstEncoding, owned map[uint32]struct{}, tableSpans []ShadowTableSpan, sources map[uint32][]ShadowTableFirstSource) {
	for index := 0; index+1 < len(hits); index++ {
		left, right := hits[index], hits[index+1]
		missing := (right.offset-left.offset)/2 - 1
		if missing < 1 || missing > shadowTableFirstMaxInteriorWords || left.target == right.target {
			continue
		}
		for relative := left.offset + 2; relative < right.offset; relative += 2 {
			addShadowTableFirstSource(image, bank, bankOffset, relative, left.offset, right.offset+2,
				shadowTableFirstMissingInterior, 2, 2, encoding, known, owned, tableSpans, sources)
		}
	}
}

func collectShadowTableFirstEdges(image romimage.Image, bank byte, bankOffset, bankLength int, known map[uint16]struct{}, hits []shadowTableFirstKnownHit, encoding shadowTableFirstEncoding, owned map[uint32]struct{}, tableSpans []ShadowTableSpan, sources map[uint32][]ShadowTableFirstSource) {
	for start := 0; start < len(hits); {
		end := start + 1
		for end < len(hits) && hits[end].offset == hits[end-1].offset+2 {
			end++
		}
		run := hits[start:end]
		prefixEnd, prefixOK := shadowTableFirstDistinctPrefix(run)
		if prefixOK && run[0].offset >= 2 {
			addShadowTableFirstSource(image, bank, bankOffset, run[0].offset-2, run[0].offset-2, run[prefixEnd].offset+2,
				shadowTableFirstMissingBefore, prefixEnd+1, 2, encoding, known, owned, tableSpans, sources)
		}
		suffixStart, suffixOK := shadowTableFirstDistinctSuffix(run)
		if suffixOK && run[len(run)-1].offset+3 < bankLength {
			addShadowTableFirstSource(image, bank, bankOffset, run[len(run)-1].offset+2, run[suffixStart].offset, run[len(run)-1].offset+4,
				shadowTableFirstMissingAfter, len(run)-suffixStart, 2, encoding, known, owned, tableSpans, sources)
		}
		start = end
	}
}

func shadowTableFirstDistinctPrefix(run []shadowTableFirstKnownHit) (int, bool) {
	if len(run) < 2 {
		return 0, false
	}
	first := run[0].target
	for index := 1; index < len(run); index++ {
		if run[index].target != first {
			return index, true
		}
	}
	return 0, false
}

func shadowTableFirstDistinctSuffix(run []shadowTableFirstKnownHit) (int, bool) {
	if len(run) < 2 {
		return 0, false
	}
	last := run[len(run)-1].target
	for index := len(run) - 2; index >= 0; index-- {
		if run[index].target != last {
			return index, true
		}
	}
	return 0, false
}

func addShadowTableFirstSource(image romimage.Image, bank byte, bankOffset, relative, rangeStart, rangeEnd int, kind string, knownEntries, distinctKnownTargets int, encoding shadowTableFirstEncoding, known map[uint16]struct{}, owned map[uint32]struct{}, tableSpans []ShadowTableSpan, sources map[uint32][]ShadowTableFirstSource) {
	value := shadowTableFirstWord(image, bankOffset+relative)
	target, valid := encoding.resolve(value)
	if !valid {
		return
	}
	if _, alreadyKnown := known[target]; alreadyKnown {
		return
	}
	targetPC := (uint32(bank) << 16) | uint32(target)
	bankBase := uint32(bank) << 16
	source := ShadowTableFirstSource{
		Kind: kind, Encoding: encoding.kind, WordPC: bankBase + uint32(0x8000+relative),
		StartPC: bankBase + uint32(0x8000+rangeStart), EndExclusive: bankBase + uint32(0x8000+rangeEnd),
		KnownEntries: knownEntries, DistinctKnownTargets: distinctKnownTargets,
	}
	if encoding.kind == shadowTableFirstBasePlusU16 {
		source.BasePC = bankBase | uint32(encoding.base)
		source.BaseEvidencePCs = append([]uint32(nil), encoding.evidence...)
	}
	source.Ownership, source.Confidence = shadowEntryPointerOwnership(source.StartPC, source.EndExclusive, knownEntries+1, owned, tableSpans)
	if encoding.kind == shadowTableFirstBasePlusU16 && source.Confidence == analysis.ConfidenceSpeculative {
		return
	}
	for _, existing := range sources[targetPC] {
		if existing.Kind == source.Kind && existing.Encoding == source.Encoding && existing.BasePC == source.BasePC &&
			existing.WordPC == source.WordPC && existing.StartPC == source.StartPC && existing.EndExclusive == source.EndExclusive {
			return
		}
	}
	sources[targetPC] = append(sources[targetPC], source)
}

func newShadowTableFirstProbeContext(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, regions []decoder.DataRegion, tableSpans []ShadowTableSpan, calleeExitMX map[decoder.Variant]decoder.MX, owned map[uint32]struct{}) shadowTableFirstProbeContext {
	anchorsByBank := make(map[byte]map[uint16]struct{})
	starts := make(map[uint32][]analysis.MXState)
	for _, result := range results {
		for _, instruction := range result.instructions {
			pc := instruction.PC & 0xffffff
			starts[pc] = appendUniqueShadowMX(starts[pc], analysis.MXState{M: instruction.M & 1, X: instruction.X & 1})
		}
		for _, span := range result.spans {
			bank := byte(span.FunctionEntry >> 16)
			if anchorsByBank[bank] == nil {
				anchorsByBank[bank] = make(map[uint16]struct{})
			}
			anchorsByBank[bank][uint16(span.FunctionEntry)] = struct{}{}
		}
	}
	anchors := make(map[byte][]uint16, len(anchorsByBank))
	for bank, values := range anchorsByBank {
		anchors[bank] = sortedShadowLandingAddresses(values)
	}
	for pc := range starts {
		sort.Slice(starts[pc], func(i, j int) bool {
			if starts[pc][i].M != starts[pc][j].M {
				return starts[pc][i].M < starts[pc][j].M
			}
			return starts[pc][i].X < starts[pc][j].X
		})
	}
	configs := make(map[byte]*config.Config, len(banks))
	for _, bank := range banks {
		configs[bank.ID] = bank.Config
	}
	return shadowTableFirstProbeContext{
		image: image, regions: regions, calleeExitMX: calleeExitMX,
		configByBank: configs, anchors: anchors, owned: owned, starts: starts, tableSpans: tableSpans,
	}
}

func (context shadowTableFirstProbeContext) probe(start uint32) (ShadowLandingCandidate, string, string, bool) {
	start &= 0xffffff
	bank := byte(start >> 16)
	startPC := uint16(start)
	anchors := context.anchors[bank]
	anchor, found := nextShadowLandingAnchor(anchors, startPC)
	boundary := "confirmed_entry"
	if !found || int(anchor)-int(startPC) > shadowLandingWindow {
		limit := int(startPC) + shadowLandingWindow
		if limit > 0xffff {
			limit = 0xffff
		}
		if limit <= int(startPC)+1 {
			return ShadowLandingCandidate{}, "", "no_bounded_decode_window", false
		}
		anchor = uint16(limit)
		boundary = "scan_limit"
	}
	if _, claimed := context.owned[start]; claimed {
		return ShadowLandingCandidate{}, "", "inside_decoded_instruction", false
	}
	if shadowInDataRegion(context.regions, bank, startPC) {
		return ShadowLandingCandidate{}, "", "declared_data", false
	}
	if shadowLandingAddressInTableOwnership(start, context.tableSpans, shadowTableOwnershipConfirmed) {
		return ShadowLandingCandidate{}, "", "confirmed_table_data", false
	}
	cfg := context.configByBank[bank]
	if cfg == nil {
		return ShadowLandingCandidate{}, "", "bank_without_configuration", false
	}
	siblings := make(map[uint16]struct{}, len(anchors))
	for _, sibling := range anchors {
		siblings[sibling] = struct{}{}
	}
	anchorPC := decoder.Address24(bank, anchor)
	finding := ShadowLandingCandidate{
		AnchorPC: anchorPC, CandidateEntryPC: start, PrecededBy: "ROM_POINTER_WINDOW",
		Reason: "an authored-entry-anchored pointer seeds a bounded forward decode that is stack-balanced and either joins the next confirmed entry or terminates cleanly",
	}
	rejectedRTI := false
	for m := uint8(0); m < 2; m++ {
		for x := uint8(0); x < 2; x++ {
			end := anchor
			graph, err := decoder.DecodeFunction(context.image, bank, startPC, m, x, decoder.Options{
				End: &end, MaxInstructions: shadowLandingMaxInstructions,
				DataRegions: context.regions, HLEDispatch: cfg.HLEDispatch,
				CalleeExitMX: context.calleeExitMX, SiblingEntryPCs: siblings,
			})
			if err != nil {
				continue
			}
			shape, ok := validateShadowLandingGraph(graph, start, anchorPC, context.owned, context.regions, context.tableSpans)
			if !ok {
				continue
			}
			if shadowTableFirstContainsRTI(graph) {
				rejectedRTI = true
				continue
			}
			if boundary == "scan_limit" && shape.Termination != "clean_return" {
				continue
			}
			finding.Variants = appendShadowLandingVariant(finding.Variants, shape, analysis.MXState{M: m, X: x})
		}
	}
	if len(finding.Variants) == 0 {
		if rejectedRTI {
			return ShadowLandingCandidate{}, "", "rti_not_valid_for_table_pointer", false
		}
		return ShadowLandingCandidate{}, "", "no_stack_balanced_bounded_decode", false
	}
	sortShadowLandingVariants(finding.Variants)
	finding.Confidence = analysis.ConfidenceSpeculative
	finding.Ownership = shadowLandingUnclaimed
	for _, variant := range finding.Variants {
		if variant.Ownership == shadowLandingTableConflict {
			finding.Ownership = shadowLandingTableConflict
		}
		if variant.Confidence == analysis.ConfidenceProbable {
			finding.Confidence = analysis.ConfidenceProbable
		}
	}
	if finding.Ownership == shadowLandingTableConflict {
		finding.Confidence = analysis.ConfidenceSpeculative
	}
	return finding, boundary, "", true
}

func shadowTableFirstContainsRTI(graph *decoder.Graph) bool {
	if graph == nil {
		return false
	}
	for _, decoded := range graph.Instructions {
		if decoded != nil && decoded.Instruction != nil && decoded.Instruction.Mnemonic == "RTI" {
			return true
		}
	}
	return false
}

func shadowTableFirstFinding(targetPC uint32, landingSeed, landingBoundary string, landing ShadowLandingCandidate, sources []ShadowTableFirstSource) ShadowTableFirstTarget {
	finding := ShadowTableFirstTarget{
		TargetPC: targetPC, AnchorPC: landing.AnchorPC, Classification: "new_landing",
		LandingSeed: landingSeed, LandingBoundary: landingBoundary,
		LandingOwnership: landing.Ownership, LandingConfidence: landing.Confidence,
		Confidence: analysis.ConfidenceSpeculative, Sources: sources,
		Reason: "an unknown same-bank pointer is tightly anchored by authored entry pointers and passes the bounded stack-balanced landing checks",
	}
	for _, variant := range landing.Variants {
		for _, mx := range variant.EntryMX {
			finding.EntryMX = appendUniqueShadowMX(finding.EntryMX, mx)
		}
	}
	sort.Slice(finding.EntryMX, func(i, j int) bool {
		if finding.EntryMX[i].M != finding.EntryMX[j].M {
			return finding.EntryMX[i].M < finding.EntryMX[j].M
		}
		return finding.EntryMX[i].X < finding.EntryMX[j].X
	})
	for _, source := range sources {
		if landing.Confidence == analysis.ConfidenceProbable && landing.Ownership != shadowLandingTableConflict && source.Confidence != analysis.ConfidenceSpeculative {
			finding.Confidence = analysis.ConfidenceProbable
			break
		}
	}
	return finding
}

func shadowTableFirstInternalFinding(targetPC uint32, entryMX []analysis.MXState, sources []ShadowTableFirstSource) ShadowTableFirstTarget {
	finding := ShadowTableFirstTarget{
		TargetPC: targetPC, EntryMX: append([]analysis.MXState(nil), entryMX...),
		Classification: "address_taken_internal", LandingSeed: "decoded_instruction",
		LandingBoundary: "existing_region", LandingOwnership: "decoded_code",
		LandingConfidence: analysis.ConfidenceProven, Confidence: analysis.ConfidenceSpeculative,
		Sources: sources,
		Reason:  "an unknown same-bank pointer is tightly anchored by authored entry pointers and names an existing decoded instruction boundary",
	}
	for _, source := range sources {
		if source.Confidence != analysis.ConfidenceSpeculative {
			finding.Confidence = analysis.ConfidenceProbable
			break
		}
	}
	return finding
}

func shadowTableFirstWord(image romimage.Image, offset int) uint16 {
	return uint16(image[offset]) | uint16(image[offset+1])<<8
}

func countShadowTableFirstConfidence(targets []ShadowTableFirstTarget) (probable, speculative int) {
	for _, target := range targets {
		if target.Confidence == analysis.ConfidenceProbable {
			probable++
		} else {
			speculative++
		}
	}
	return probable, speculative
}
