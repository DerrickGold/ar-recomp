package tooling

import (
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const (
	shadowLandingWindow          = 0x100
	shadowLandingMaxInstructions = 128
	shadowLandingMaxStackBytes   = 32
	shadowLandingUnclaimed       = "unclaimed"
	shadowLandingTableConflict   = "candidate_table_conflict"
)

// ShadowLandingCandidate is a review-only entry boundary recovered by
// seeking backwards from a confirmed entry boundary and decoding forwards.
// The sweep never promotes the candidate into the generated entry registry.
type ShadowLandingCandidate struct {
	AnchorPC         uint32                 `json:"anchor_pc"`
	CandidateEntryPC uint32                 `json:"candidate_entry_pc"`
	PrecededBy       string                 `json:"preceded_by"`
	Variants         []ShadowLandingVariant `json:"variants"`
	Ownership        string                 `json:"ownership"`
	Confidence       analysis.Confidence    `json:"confidence"`
	Reason           string                 `json:"reason"`
}

// ShadowLandingVariant keeps width-dependent decode shapes separate while the
// parent candidate remains deduplicated by source entry PC.
type ShadowLandingVariant struct {
	EndExclusive     uint32              `json:"end_exclusive"`
	EntryMX          []analysis.MXState  `json:"entry_mx"`
	Termination      string              `json:"termination"`
	InstructionCount int                 `json:"instruction_count"`
	Ownership        string              `json:"ownership"`
	Confidence       analysis.Confidence `json:"confidence"`
}

type shadowLandingBoundary struct {
	PC        uint32
	Mnemonics []string
}

type shadowLandingShape struct {
	EndExclusive     uint32
	Termination      string
	InstructionCount int
	Ownership        string
	Confidence       analysis.Confidence
}

// collectShadowLandingCandidates implements the first deliberately
// conservative landing sweep. Confirmed flow terminators seed possible starts;
// the next confirmed function entry is the forward landing boundary. Later
// passes may add pointer/table seeds without weakening these acceptance rules.
func collectShadowLandingCandidates(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, regions []decoder.DataRegion, tableSpans []ShadowTableSpan, dispatchIslands []ShadowDispatchCodeIsland, calleeExitMX map[decoder.Variant]decoder.MX) []ShadowLandingCandidate {
	owned := make(map[uint32]struct{})
	alreadyReported := make(map[uint32]struct{}, len(dispatchIslands))
	for _, island := range dispatchIslands {
		alreadyReported[island.CandidateEntryPC&0xffffff] = struct{}{}
	}
	anchorsByBank := make(map[byte]map[uint16]struct{})
	boundariesByBank := make(map[byte]map[uint16][]string)
	configByBank := make(map[byte]*config.Config)
	for _, bank := range banks {
		configByBank[bank.ID] = bank.Config
	}
	for _, result := range results {
		for _, span := range result.spans {
			bank := byte(span.PC >> 16)
			if anchorsByBank[bank] == nil {
				anchorsByBank[bank] = make(map[uint16]struct{})
			}
			anchorsByBank[bank][uint16(span.FunctionEntry)] = struct{}{}
			for offset := uint8(0); offset < span.Length; offset++ {
				owned[(span.PC&0xff0000)|uint32(uint16(span.PC)+uint16(offset))] = struct{}{}
			}
		}
		for _, decoded := range result.instructions {
			mnemonic := disassemblyMnemonic(&decoded.Instruction)
			if !disassemblyFlowEnd(mnemonic) {
				continue
			}
			end := uint32(uint16(decoded.PC)) + uint32(decoded.Instruction.Length)
			if end > 0xffff {
				continue
			}
			bank := byte(decoded.PC >> 16)
			if boundariesByBank[bank] == nil {
				boundariesByBank[bank] = make(map[uint16][]string)
			}
			pc := uint16(end)
			boundariesByBank[bank][pc] = appendUniqueShadowString(boundariesByBank[bank][pc], mnemonic)
		}
	}

	var boundaries []shadowLandingBoundary
	for bank, starts := range boundariesByBank {
		anchors := sortedShadowLandingAddresses(anchorsByBank[bank])
		for start, mnemonics := range starts {
			if _, alreadyEntry := anchorsByBank[bank][start]; alreadyEntry {
				continue
			}
			anchor, found := nextShadowLandingAnchor(anchors, start)
			if !found || int(anchor)-int(start) > shadowLandingWindow {
				continue
			}
			sort.Strings(mnemonics)
			boundaries = append(boundaries, shadowLandingBoundary{
				PC: decoder.Address24(bank, start), Mnemonics: mnemonics,
			})
		}
	}
	sort.Slice(boundaries, func(i, j int) bool { return boundaries[i].PC < boundaries[j].PC })

	findings := make(map[uint32]ShadowLandingCandidate)
	for _, boundary := range boundaries {
		if _, duplicate := alreadyReported[boundary.PC&0xffffff]; duplicate {
			continue
		}
		bank := byte(boundary.PC >> 16)
		start := uint16(boundary.PC)
		anchors := sortedShadowLandingAddresses(anchorsByBank[bank])
		anchor, found := nextShadowLandingAnchor(anchors, start)
		if !found || shadowLandingAddressClaimed(boundary.PC, owned, regions, tableSpans) {
			continue
		}
		cfg := configByBank[bank]
		if cfg == nil {
			continue
		}
		siblings := make(map[uint16]struct{}, len(anchors))
		for _, sibling := range anchors {
			siblings[sibling] = struct{}{}
		}
		anchorPC := decoder.Address24(bank, anchor)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				end := anchor
				graph, err := decoder.DecodeFunction(image, bank, start, m, x, decoder.Options{
					End: &end, MaxInstructions: shadowLandingMaxInstructions,
					DataRegions: regions, HLEDispatch: cfg.HLEDispatch,
					CalleeExitMX: calleeExitMX, SiblingEntryPCs: siblings,
				})
				if err != nil {
					continue
				}
				shape, ok := validateShadowLandingGraph(graph, boundary.PC, anchorPC, owned, regions, tableSpans)
				if !ok {
					continue
				}
				key := boundary.PC & 0xffffff
				finding, exists := findings[key]
				if !exists {
					finding = ShadowLandingCandidate{
						AnchorPC: anchorPC, CandidateEntryPC: boundary.PC,
						PrecededBy: strings.Join(boundary.Mnemonics, "/"),
						Reason:     "forward decode from an unclaimed post-terminator boundary is stack-balanced and either joins the next confirmed entry or terminates cleanly",
					}
				}
				finding.Variants = appendShadowLandingVariant(finding.Variants, shape, analysis.MXState{M: m, X: x})
				findings[key] = finding
			}
		}
	}

	result := make([]ShadowLandingCandidate, 0, len(findings))
	for _, finding := range findings {
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
		result = append(result, finding)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].CandidateEntryPC != result[j].CandidateEntryPC {
			return result[i].CandidateEntryPC < result[j].CandidateEntryPC
		}
		return result[i].AnchorPC < result[j].AnchorPC
	})
	return result
}

func validateShadowLandingGraph(graph *decoder.Graph, start, anchor uint32, owned map[uint32]struct{}, regions []decoder.DataRegion, tableSpans []ShadowTableSpan) (shadowLandingShape, bool) {
	if graph == nil || len(graph.Instructions) < 2 || len(graph.UnresolvedIndirects) != 0 {
		return shadowLandingShape{}, false
	}
	bank := byte(start >> 16)
	lengthByPC := make(map[uint32]uint8)
	byteOwner := make(map[uint32]uint32)
	uniqueInstructions := make(map[uint32]struct{})
	endExclusive := start
	containsCall := false
	candidateTableConflict := false
	for key, decoded := range graph.Instructions {
		if decoded == nil || decoded.Instruction == nil || byte(key.PC>>16) != bank || key.PC < start || key.PC >= anchor {
			return shadowLandingShape{}, false
		}
		instruction := decoded.Instruction
		if instruction.Mnemonic == "JSR" || instruction.Mnemonic == "JSL" {
			containsCall = true
		}
		if instruction.Length == 0 || instruction.Mnemonic == "BRK" || instruction.Mnemonic == "COP" || instruction.Mnemonic == "WAI" || instruction.Mnemonic == "STP" || instruction.Mnemonic == "XCE" {
			return shadowLandingShape{}, false
		}
		instructionEnd := key.PC + uint32(instruction.Length)
		if instructionEnd > anchor || byte((instructionEnd-1)>>16) != bank {
			return shadowLandingShape{}, false
		}
		if previous, found := lengthByPC[key.PC]; found && previous != instruction.Length {
			return shadowLandingShape{}, false
		}
		lengthByPC[key.PC] = instruction.Length
		uniqueInstructions[key.PC] = struct{}{}
		for address := key.PC; address < instructionEnd; address++ {
			if shadowLandingAddressClaimed(address, owned, regions, tableSpans) {
				return shadowLandingShape{}, false
			}
			if shadowLandingAddressInTableOwnership(address, tableSpans, shadowTableOwnershipCandidate) {
				candidateTableConflict = true
			}
			if previous, found := byteOwner[address]; found && previous != key.PC {
				return shadowLandingShape{}, false
			}
			byteOwner[address] = key.PC
		}
		if instructionEnd > endExclusive {
			endExclusive = instructionEnd
		}
	}
	if len(uniqueInstructions) < 2 {
		return shadowLandingShape{}, false
	}

	stackAt := map[decoder.DecodeKey]int{graph.Entry: 0}
	queue := []decoder.DecodeKey{graph.Entry}
	joinedAnchor, explicitAnchorEdge, cleanReturn := false, false, false
	for len(queue) > 0 {
		key := queue[0]
		queue = queue[1:]
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil {
			return shadowLandingShape{}, false
		}
		delta, valid := shadowStackDelta(decoded.Instruction, key)
		if !valid {
			return shadowLandingShape{}, false
		}
		after := stackAt[key] + delta
		if after < 0 || after > shadowLandingMaxStackBytes {
			return shadowLandingShape{}, false
		}
		if len(decoded.Successors) == 0 {
			switch decoded.Instruction.Mnemonic {
			case "RTS", "RTL", "RTI":
				if after != 0 {
					return shadowLandingShape{}, false
				}
				cleanReturn = true
			default:
				return shadowLandingShape{}, false
			}
			continue
		}
		for _, successor := range decoded.Successors {
			if successor.PC == anchor {
				if after != 0 {
					return shadowLandingShape{}, false
				}
				joinedAnchor = true
				if shadowLandingExplicitControl(decoded.Instruction) {
					explicitAnchorEdge = true
				}
				continue
			}
			if successor.PC < start || successor.PC >= anchor {
				return shadowLandingShape{}, false
			}
			if _, exists := graph.Instructions[successor]; !exists {
				return shadowLandingShape{}, false
			}
			if previous, seen := stackAt[successor]; seen {
				if previous != after {
					return shadowLandingShape{}, false
				}
				continue
			}
			stackAt[successor] = after
			queue = append(queue, successor)
		}
	}
	if !joinedAnchor && !cleanReturn {
		return shadowLandingShape{}, false
	}
	termination := "clean_return"
	if joinedAnchor && cleanReturn {
		termination = "return_or_join_anchor"
	} else if joinedAnchor {
		termination = "join_anchor"
	}
	confidence := analysis.ConfidenceProbable
	ownership := shadowLandingUnclaimed
	if candidateTableConflict {
		confidence = analysis.ConfidenceSpeculative
		ownership = shadowLandingTableConflict
	} else if len(uniqueInstructions) <= 2 && !containsCall && !explicitAnchorEdge {
		confidence = analysis.ConfidenceSpeculative
	}
	return shadowLandingShape{
		EndExclusive: endExclusive, Termination: termination,
		InstructionCount: len(uniqueInstructions), Ownership: ownership, Confidence: confidence,
	}, true
}

func shadowLandingExplicitControl(instruction *cpu65816.Instruction) bool {
	if xrefBranch(instruction) {
		return true
	}
	return instruction.Mnemonic == "JMP" || instruction.Mnemonic == "JML"
}

func shadowLandingAddressClaimed(address uint32, owned map[uint32]struct{}, regions []decoder.DataRegion, tableSpans []ShadowTableSpan) bool {
	address &= 0xffffff
	if _, found := owned[address]; found || shadowLandingAddressInTableOwnership(address, tableSpans, shadowTableOwnershipConfirmed) {
		return true
	}
	return shadowInDataRegion(regions, byte(address>>16), uint16(address))
}

func shadowLandingAddressInTableOwnership(address uint32, spans []ShadowTableSpan, ownership string) bool {
	address &= 0xffffff
	for _, span := range spans {
		if span.Ownership == ownership && address >= span.StartPC && address < span.EndExclusive {
			return true
		}
	}
	return false
}

func sortedShadowLandingAddresses(values map[uint16]struct{}) []uint16 {
	result := make([]uint16, 0, len(values))
	for value := range values {
		result = append(result, value)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
}

func nextShadowLandingAnchor(anchors []uint16, start uint16) (uint16, bool) {
	index := sort.Search(len(anchors), func(index int) bool { return anchors[index] > start })
	if index >= len(anchors) {
		return 0, false
	}
	return anchors[index], true
}

func appendUniqueShadowString(values []string, incoming string) []string {
	for _, value := range values {
		if value == incoming {
			return values
		}
	}
	return append(values, incoming)
}

func countShadowLandingConfidence(candidates []ShadowLandingCandidate) (probable, speculative, tableConflicts int) {
	for _, candidate := range candidates {
		if candidate.Ownership == shadowLandingTableConflict {
			tableConflicts++
		}
		if candidate.Confidence == analysis.ConfidenceSpeculative {
			speculative++
		} else {
			probable++
		}
	}
	return probable, speculative, tableConflicts
}

func appendShadowLandingVariant(variants []ShadowLandingVariant, shape shadowLandingShape, mx analysis.MXState) []ShadowLandingVariant {
	for index := range variants {
		variant := &variants[index]
		if variant.EndExclusive == shape.EndExclusive && variant.Termination == shape.Termination &&
			variant.InstructionCount == shape.InstructionCount && variant.Ownership == shape.Ownership && variant.Confidence == shape.Confidence {
			variant.EntryMX = appendUniqueShadowMX(variant.EntryMX, mx)
			return variants
		}
	}
	return append(variants, ShadowLandingVariant{
		EndExclusive: shape.EndExclusive, EntryMX: []analysis.MXState{mx},
		Termination: shape.Termination, InstructionCount: shape.InstructionCount,
		Ownership: shape.Ownership, Confidence: shape.Confidence,
	})
}

func sortShadowLandingVariants(variants []ShadowLandingVariant) {
	for index := range variants {
		sort.Slice(variants[index].EntryMX, func(i, j int) bool {
			if variants[index].EntryMX[i].M != variants[index].EntryMX[j].M {
				return variants[index].EntryMX[i].M < variants[index].EntryMX[j].M
			}
			return variants[index].EntryMX[i].X < variants[index].EntryMX[j].X
		})
	}
	sort.Slice(variants, func(i, j int) bool {
		if variants[i].EndExclusive != variants[j].EndExclusive {
			return variants[i].EndExclusive < variants[j].EndExclusive
		}
		if variants[i].Termination != variants[j].Termination {
			return variants[i].Termination < variants[j].Termination
		}
		if variants[i].InstructionCount != variants[j].InstructionCount {
			return variants[i].InstructionCount < variants[j].InstructionCount
		}
		if variants[i].Ownership != variants[j].Ownership {
			return variants[i].Ownership < variants[j].Ownership
		}
		return variants[i].Confidence < variants[j].Confidence
	})
}
