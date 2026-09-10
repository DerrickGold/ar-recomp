package tooling

import (
	"encoding/json"
	"fmt"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// ShadowStoredField identifies an address expression, NOT a memory allocation.
// Matching expressions in different functions need not address the same object:
// D, DB, the index value, lifetime, and intervening aliases remain unproven.
type ShadowStoredField struct {
	Mode    string `json:"mode"`
	Operand uint32 `json:"operand"`
}

type ShadowStoredTargetFlow struct {
	Contexts         []analysis.EntryVariant       `json:"contexts"`
	LiveMX           analysis.MXState              `json:"live_mx"`
	LoadPC           uint32                        `json:"load_pc"`
	PushPC           uint32                        `json:"push_pc"`
	Field            ShadowStoredField             `json:"field"`
	EvidenceScope    string                        `json:"evidence_scope"`
	WriterMatch      string                        `json:"writer_match"`
	TargetAddend     int                           `json:"target_addend"`
	Writers          []ShadowStoredTargetWriter    `json:"writer_candidates,omitempty"`
	Targets          []ShadowStoredTargetCandidate `json:"target_candidates,omitempty"`
	SourceFields     []ShadowStoredFieldSource     `json:"source_fields,omitempty"`
	SourcesTruncated bool                          `json:"source_fields_truncated,omitempty"`
	ProofObligations []string                      `json:"proof_obligations"`
}

type ShadowStoredTargetWriter struct {
	Contexts      []analysis.EntryVariant `json:"contexts"`
	StorePC       uint32                  `json:"store_pc"`
	StoreMX       analysis.MXState        `json:"store_mx"`
	Width         uint8                   `json:"width"`
	Kind          string                  `json:"kind"`
	SourcePC      uint32                  `json:"source_pc,omitempty"`
	SourceMode    string                  `json:"source_mode,omitempty"`
	SourceOperand uint32                  `json:"source_operand,omitempty"`
	ValueAddend   int                     `json:"value_addend"`
	StoredValue   *uint16                 `json:"stored_value,omitempty"`
	SourceField   *ShadowStoredField      `json:"source_field,omitempty"`
	Expression    *ShadowStoredExpression `json:"value_expression,omitempty"`
}

// A bounded expression-dependency index, not a reaching-definition proof.
// No value is substituted across these conditional memory aliases.
type ShadowStoredFieldSource struct {
	Field            ShadowStoredField          `json:"field"`
	Depth            int                        `json:"depth"`
	IncomingStorePCs []uint32                   `json:"incoming_store_pcs"`
	Writers          []ShadowStoredTargetWriter `json:"writer_candidates,omitempty"`
}

type shadowStoredFieldKey struct {
	bank  byte
	field ShadowStoredField
}

const shadowStoredSourceDepth = 2

type ShadowStoredTargetCandidate struct {
	PC            uint32 `json:"pc"`
	StorePC       uint32 `json:"store_pc"`
	CallPC        uint32 `json:"call_pc,omitempty"`
	EntryKind     string `json:"entry_kind"`
	Ownership     string `json:"ownership"`
	AuthoredEntry bool   `json:"authored_entry"`
}

type shadowStoredRead struct {
	site uint32
	flow ShadowStoredTargetFlow
}

type shadowStoredWrite struct {
	field       ShadowStoredField
	writer      ShadowStoredTargetWriter
	initializer *ShadowTableInitializer
}

// This first query covers indexed fields only. Scalar dp trampolines have the
// separate caller-to-trampoline query. No field offset or record layout is fixed.
func shadowStoredField(instruction *cpu65816.Instruction) (ShadowStoredField, bool) {
	switch instruction.Mode {
	case cpu65816.DPX, cpu65816.DPY, cpu65816.ABSX, cpu65816.ABSY, cpu65816.LONGX:
		return ShadowStoredField{Mode: instruction.Mode.String(), Operand: instruction.Operand}, true
	}
	return ShadowStoredField{}, false
}

// Scalar pointer/index slots are also useful to the indirect-jump audit. Keep
// this distinct from the indexed-only RTS sink/field-dependency query above.
func shadowMemoryField(instruction *cpu65816.Instruction) (ShadowStoredField, bool) {
	if field, ok := shadowStoredField(instruction); ok {
		return field, true
	}
	switch instruction.Mode {
	case cpu65816.DP, cpu65816.ABS, cpu65816.LONG:
		return ShadowStoredField{Mode: instruction.Mode.String(), Operand: instruction.Operand}, true
	}
	return ShadowStoredField{}, false
}

// storedValue follows a word value through register transfers and INC/DEC.
// Unlike ADC/SBC, those adjustments do not depend on carry or decimal mode.
// Arithmetic with unmodeled flags, joins, calls, and memory writes stop the walk.
func (walk shadowPointerWalk) storedValue(key decoder.DecodeKey, reg string) (*decoder.DecodedInstruction, int) {
	addend := 0
	for range shadowPointerWalkLimit {
		decoded := walk.previous(key)
		if decoded == nil || decoded.Instruction == nil || !shadowPointerWord(decoded.Key, reg) {
			return nil, 0
		}
		key = decoded.Key
		instruction := decoded.Instruction
		if instruction.Mnemonic == "LD"+reg || instruction.Mnemonic == "PL"+reg {
			return decoded, addend
		}
		if (reg == "A" && instruction.Mode == cpu65816.ACC && (instruction.Mnemonic == "INC" || instruction.Mnemonic == "DEC")) ||
			(reg == "X" && (instruction.Mnemonic == "INX" || instruction.Mnemonic == "DEX")) ||
			(reg == "Y" && (instruction.Mnemonic == "INY" || instruction.Mnemonic == "DEY")) {
			if instruction.Mnemonic[0] == 'I' {
				addend++
			} else {
				addend--
			}
			continue
		}
		if src, dst := shadowPointerTransfer(instruction.Mnemonic); dst != "" {
			if dst == reg {
				if !shadowPointerWord(key, src) {
					return nil, 0
				}
				reg = src
			}
			continue
		}
		if instruction.Mnemonic == "LDA" || instruction.Mnemonic == "LDX" || instruction.Mnemonic == "LDY" || shadowPointerTransparent(instruction) {
			continue
		}
		// An accumulator-only operation does not clobber an X/Y value being
		// carried across a branch or used later by TXA/TYA. In particular its
		// carry/decimal uncertainty must not contaminate an unrelated register.
		if reg != "A" {
			switch instruction.Mnemonic {
			case "ADC", "SBC", "AND", "ORA", "EOR":
				continue
			case "ASL", "LSR", "ROL", "ROR", "INC", "DEC":
				if instruction.Mode == cpu65816.ACC {
					continue
				}
			}
		}
		return nil, 0
	}
	return nil, 0
}

func collectShadowStoredTargets(graph *decoder.Graph) ([]shadowStoredRead, []shadowStoredWrite) {
	var reads []shadowStoredRead
	var writes []shadowStoredWrite
	var walk shadowPointerWalk
	getWalk := func() shadowPointerWalk {
		if walk.graph == nil {
			walk = shadowPointerWalk{graph: graph, preds: shadowPredecessors(graph)}
		}
		return walk
	}
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		instruction := decoded.Instruction
		if instruction.Mnemonic == "RTS" {
			cursor := key
			for range 8 {
				push := getWalk().previous(cursor)
				if push == nil || push.Instruction == nil {
					break
				}
				cursor = push.Key
				reg := ""
				switch push.Instruction.Mnemonic {
				case "PHA":
					reg = "A"
				case "PHX":
					reg = "X"
				case "PHY":
					reg = "Y"
				case "NOP", "REP", "SEP":
					continue
				}
				if reg == "" || !shadowPointerWord(push.Key, reg) {
					break
				}
				load, addend := getWalk().storedValue(push.Key, reg)
				if load == nil || shadowPointerRegister(load.Instruction.Mnemonic) == "" {
					break
				}
				field, valid := shadowStoredField(load.Instruction)
				if !valid {
					break
				}
				reads = append(reads, shadowStoredRead{site: key.PC, flow: ShadowStoredTargetFlow{
					Contexts: []analysis.EntryVariant{{PC: graph.Entry.PC, EntryMX: analysis.MXState{M: graph.Entry.M, X: graph.Entry.X}}}, LiveMX: analysis.MXState{M: key.M, X: key.X},
					LoadPC: load.Key.PC, PushPC: push.Key.PC, Field: field, TargetAddend: addend + 1,
					EvidenceScope: "decoded_cfg_and_static_call_closure", WriterMatch: "same_bank_address_expression_only",
					ProofObligations: []string{"same_memory_allocation_and_index", "D_DB_and_bank_aliases", "all_writers_and_intervening_clobbers", "target_entry_kind_and_live_mx", "HLE_semantics_and_reachability"},
				}})
				break
			}
		}
		field, valid := shadowMemoryField(instruction)
		if !valid {
			continue
		}
		reg := shadowPointerRegister(instruction.Mnemonic)
		kind := "unknown_value"
		switch instruction.Mnemonic {
		case "STA", "STX", "STY":
		case "STZ":
			reg, kind = "A", "zero_store"
		case "INC", "DEC", "ASL", "LSR", "ROL", "ROR", "TRB", "TSB":
			reg, kind = "A", "read_modify_write"
		default:
			continue
		}
		writer := ShadowStoredTargetWriter{
			Contexts: []analysis.EntryVariant{{PC: graph.Entry.PC, EntryMX: analysis.MXState{M: graph.Entry.M, X: graph.Entry.X}}},
			StorePC:  key.PC, StoreMX: analysis.MXState{M: key.M, X: key.X}, Width: 2, Kind: kind,
		}
		var initializer *ShadowTableInitializer
		if !shadowPointerWord(key, reg) {
			writer.Width, writer.Kind = 1, "partial_write"
		} else if kind == "zero_store" {
			value := uint16(0)
			writer.StoredValue = &value
		} else if kind == "unknown_value" {
			if source, addend := getWalk().storedValue(key, reg); source != nil {
				if read := getWalk().initializerRead(source.Key, true); read != nil {
					initializer = &ShadowTableInitializer{StorePC: key.PC, Contexts: writer.Contexts, ValueAddend: addend, Read: *read}
				}
				writer.SourcePC, writer.SourceMode, writer.SourceOperand, writer.ValueAddend = source.Key.PC, source.Instruction.Mode.String(), source.Instruction.Operand, addend
				writer.Kind = "memory_value"
				if field, valid := shadowStoredField(source.Instruction); valid {
					writer.SourceField = &field
				}
				switch {
				case source.Instruction.Mode == cpu65816.IMM:
					writer.Kind = "literal_store"
					value := uint16(int(source.Instruction.Operand) + addend)
					writer.StoredValue = &value
				case (source.Instruction.Mnemonic == "LDA" && source.Instruction.Mode == cpu65816.STK && source.Instruction.Operand == 1) || source.Instruction.Mnemonic == "PLA":
					writer.Kind = "stack_word"
					// Only an entry's first instruction is a supported caller-frame
					// read. A nearby stack load after PHP/PHA is NOT a return address.
					if source.Key == graph.Entry {
						writer.Kind = "entry_stack_word"
					}
				}
			} else {
				writer.Expression = getWalk().storedExpression(key, reg)
				writer.SourceField = writer.Expression.SourceField
			}
		}
		writes = append(writes, shadowStoredWrite{field: field, writer: writer, initializer: initializer})
	}
	return reads, writes
}

func shadowStoredJSONKey(value any) string {
	data, _ := json.Marshal(value) // all record fields are JSON-safe primitives
	return string(data)
}

func mergeStoredContexts(existing, incoming []analysis.EntryVariant) []analysis.EntryVariant {
	seen := make(map[analysis.EntryVariant]bool)
	for _, context := range existing {
		seen[context] = true
	}
	for _, context := range incoming {
		if !seen[context] {
			existing = append(existing, context)
			seen[context] = true
		}
	}
	sort.Slice(existing, func(i, j int) bool {
		a, b := existing[i], existing[j]
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		if a.EntryMX.M != b.EntryMX.M {
			return a.EntryMX.M < b.EntryMX.M
		}
		return a.EntryMX.X < b.EntryMX.X
	})
	return existing
}

func attachShadowStoredTargets(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, get func(uint32) *ShadowDispatchSite) {
	// Most games have no supported memory-fed RTS sink. Do not build a global
	// writer/call/ownership index for them, or serialize unrelated writers in
	// games that do: this query only needs expressions consumed by these sinks.
	wanted := make(map[shadowStoredFieldKey]bool)
	for _, result := range results {
		if result.issue == nil {
			for _, read := range result.storedReads {
				wanted[shadowStoredFieldKey{bank: byte(read.site >> 16), field: read.flow.Field}] = true
			}
		}
	}
	if len(wanted) == 0 {
		return
	}
	// Fixed-depth breadth expansion is independent of result/map iteration
	// order. Cycles and deeper chains remain visible but cannot grow forever.
	for range shadowStoredSourceDepth {
		next := make(map[shadowStoredFieldKey]bool)
		for _, result := range results {
			if result.issue != nil {
				continue
			}
			for _, write := range result.storedWrites {
				bank := byte(write.writer.StorePC >> 16)
				if wanted[shadowStoredFieldKey{bank, write.field}] && write.writer.SourceField != nil {
					next[shadowStoredFieldKey{bank, *write.writer.SourceField}] = true
				}
			}
		}
		for key := range next {
			wanted[key] = true
		}
	}
	writes := make(map[shadowStoredFieldKey]map[string]ShadowStoredTargetWriter)
	reads := make(map[string]shadowStoredRead)
	// Direct JSR call evidence is separate from helper stack-read evidence.
	calls := make(map[decoder.Variant][]uint32)
	authored := make(map[uint32]bool)
	boundaries, interiors := make(map[uint32]bool), make(map[uint32]bool)
	for _, bank := range banks {
		for _, entry := range bank.Config.Entries {
			authored[decoder.Address24(bank.ID, entry.Start)] = true
		}
	}
	for _, result := range results {
		if result.issue != nil {
			continue
		}
		for _, read := range result.storedReads {
			contexts := read.flow.Contexts
			read.flow.Contexts = nil
			key := fmt.Sprintf("%06X:%s", read.site, shadowStoredJSONKey(read.flow))
			read.flow.Contexts = mergeStoredContexts(reads[key].flow.Contexts, contexts)
			reads[key] = read
		}
		for _, write := range result.storedWrites {
			key := shadowStoredFieldKey{bank: byte(write.writer.StorePC >> 16), field: write.field}
			if !wanted[key] {
				continue
			}
			if writes[key] == nil {
				writes[key] = make(map[string]ShadowStoredTargetWriter)
			}
			contexts := write.writer.Contexts
			write.writer.Contexts = nil
			identity := shadowStoredJSONKey(write.writer)
			write.writer.Contexts = mergeStoredContexts(writes[key][identity].Contexts, contexts)
			writes[key][identity] = write.writer
		}
		for _, decoded := range result.instructions {
			boundaries[decoded.PC] = true
			for offset := uint8(1); offset < decoded.Instruction.Length; offset++ {
				interiors[decoded.PC&0xff0000|uint32(uint16(decoded.PC)+uint16(offset))] = true
			}
			if decoded.Instruction.Mnemonic == "JSR" && decoded.Instruction.Mode == cpu65816.ABS {
				callee := decoder.Variant{Address: decoded.PC&0xff0000 | decoded.Instruction.Operand, M: decoded.M, X: decoded.X}
				calls[callee] = appendUniqueAddresses(calls[callee], decoded.PC)
			}
		}
	}
	regions := collectShadowDataRegions(banks)
	ownership := func(pc uint32) string {
		for _, region := range regions {
			if byte(pc>>16) == region.Bank && uint16(pc) >= region.Start && uint16(pc) < region.End {
				return "authored_data"
			}
		}
		if interiors[pc] && boundaries[pc] {
			return "conflicting_decode_interpretations"
		}
		if interiors[pc] {
			return "instruction_interior_not_entry_proof"
		}
		if boundaries[pc] {
			return "decoded_boundary_not_entry_proof"
		}
		if byte(pc>>16) == 0x7e || byte(pc>>16) == 0x7f {
			return "not_rom_mapped"
		}
		if _, err := image.Slice(byte(pc>>16), uint16(pc), 1); err != nil {
			return "not_rom_mapped"
		}
		return "unclaimed_rom_not_decoded"
	}
	for _, read := range reads {
		flow := read.flow
		writerKeys := writes[shadowStoredFieldKey{bank: byte(read.site >> 16), field: flow.Field}]
		flow.SourceFields, flow.SourcesTruncated = shadowStoredSourceFields(byte(read.site>>16), flow.Field, writes)
		var candidates []ShadowStoredTargetCandidate
		add := func(value uint16, storePC, callPC uint32, kind string) {
			pc := read.site&0xff0000 | uint32(uint16(int(value)+flow.TargetAddend))
			candidates = append(candidates, ShadowStoredTargetCandidate{PC: pc, StorePC: storePC, CallPC: callPC, EntryKind: kind, Ownership: ownership(pc), AuthoredEntry: authored[pc]})
		}
		for _, writer := range writerKeys {
			flow.Writers = append(flow.Writers, writer)
			if writer.StoredValue != nil {
				add(*writer.StoredValue, writer.StorePC, 0, "unknown")
			}
			if writer.Kind == "entry_stack_word" {
				for _, context := range writer.Contexts {
					callee := decoder.Variant{Address: context.PC, M: context.EntryMX.M, X: context.EntryMX.X}
					for _, call := range calls[callee] {
						kind := "offset_from_call_return"
						if writer.ValueAddend+flow.TargetAddend == 1 {
							kind = "continuation_candidate"
						}
						// A native near call pushes the address of its final operand.
						// RTS adds one; the writer may independently add/subtract one.
						add(uint16(int(uint16(call))+2+writer.ValueAddend), writer.StorePC, call, kind)
					}
				}
			}
		}
		sort.Slice(flow.Writers, func(i, j int) bool {
			if flow.Writers[i].StorePC != flow.Writers[j].StorePC {
				return flow.Writers[i].StorePC < flow.Writers[j].StorePC
			}
			return shadowStoredJSONKey(flow.Writers[i]) < shadowStoredJSONKey(flow.Writers[j])
		})
		unique := make(map[string]ShadowStoredTargetCandidate)
		for _, candidate := range candidates {
			unique[shadowStoredJSONKey(candidate)] = candidate
		}
		for _, candidate := range unique {
			flow.Targets = append(flow.Targets, candidate)
		}
		sort.Slice(flow.Targets, func(i, j int) bool {
			if flow.Targets[i].PC != flow.Targets[j].PC {
				return flow.Targets[i].PC < flow.Targets[j].PC
			}
			return shadowStoredJSONKey(flow.Targets[i]) < shadowStoredJSONKey(flow.Targets[j])
		})
		site := get(read.site)
		if site.Routing == "unknown" {
			site.Routing = "compiler"
		}
		if site.StaticStatus == "not_analyzed" {
			site.StaticStatus = "unresolved"
			site.Reason = "indexed-memory word supplies an RTS target; matching writer expressions are conditional candidates, not a closed target set"
		}
		site.StoredTargetFlows = append(site.StoredTargetFlows, flow)
	}
}

func shadowStoredSourceFields(bank byte, root ShadowStoredField, writes map[shadowStoredFieldKey]map[string]ShadowStoredTargetWriter) ([]ShadowStoredFieldSource, bool) {
	seen := map[ShadowStoredField]bool{root: true}
	frontier := []ShadowStoredField{root}
	sources := make(map[ShadowStoredField]*ShadowStoredFieldSource)
	truncated := false
	for depth := 1; depth <= shadowStoredSourceDepth+1; depth++ {
		var next []ShadowStoredField
		for _, field := range frontier {
			for _, writer := range writes[shadowStoredFieldKey{bank, field}] {
				if writer.SourceField == nil {
					continue
				}
				source := *writer.SourceField
				if !seen[source] {
					if depth > shadowStoredSourceDepth {
						truncated = true
						continue
					}
					seen[source] = true
					sources[source] = &ShadowStoredFieldSource{Field: source, Depth: depth}
					for _, candidate := range writes[shadowStoredFieldKey{bank, source}] {
						sources[source].Writers = append(sources[source].Writers, candidate)
					}
					next = append(next, source)
				}
				if entry := sources[source]; entry != nil {
					entry.IncomingStorePCs = appendUniqueAddresses(entry.IncomingStorePCs, writer.StorePC)
				}
			}
		}
		frontier = next
	}
	var ordered []ShadowStoredFieldSource
	for _, source := range sources {
		sort.Slice(source.IncomingStorePCs, func(i, j int) bool { return source.IncomingStorePCs[i] < source.IncomingStorePCs[j] })
		sort.Slice(source.Writers, func(i, j int) bool {
			if source.Writers[i].StorePC != source.Writers[j].StorePC {
				return source.Writers[i].StorePC < source.Writers[j].StorePC
			}
			return shadowStoredJSONKey(source.Writers[i]) < shadowStoredJSONKey(source.Writers[j])
		})
		ordered = append(ordered, *source)
	}
	sort.Slice(ordered, func(i, j int) bool {
		if ordered[i].Depth != ordered[j].Depth {
			return ordered[i].Depth < ordered[j].Depth
		}
		return shadowStoredJSONKey(ordered[i].Field) < shadowStoredJSONKey(ordered[j].Field)
	})
	return ordered, truncated
}
