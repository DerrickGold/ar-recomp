package tooling

import (
	"encoding/json"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// ShadowPointerProducer is a conditional def-use relationship, not a dispatch
// fact. No table extent, handler entry width, or target completeness follows
// from finding a load and a store. In particular, caller M/X is not handler M/X
// proof, and a nearby loop bound may constrain a different register/value.
type ShadowPointerProducer struct {
	CallerEntryPC     uint32                      `json:"caller_entry_pc"`
	CallPC            uint32                      `json:"call_pc"`
	TrampolinePC      uint32                      `json:"trampoline_pc"`
	CallMX            analysis.MXState            `json:"call_mx"`
	PointerAddress    uint16                      `json:"pointer_address"`
	StorePC           uint32                      `json:"store_pc"`
	StoreOperand      uint16                      `json:"store_operand"`
	DirectPage        ShadowRegisterEvidence      `json:"direct_page"`
	RequiredD         uint16                      `json:"required_d"`
	PointerAlias      string                      `json:"pointer_alias"`
	LoadPC            uint32                      `json:"load_pc"`
	LoadMX            analysis.MXState            `json:"load_mx"`
	LoadMode          string                      `json:"load_mode"`
	LoadRegister      string                      `json:"load_register"`
	TableOffset       uint16                      `json:"table_offset"`
	IndexRegister     string                      `json:"index_register"`
	IndexOrigin       *ShadowPointerIndexOrigin   `json:"index_origin,omitempty"`
	IndexEvidence     *ShadowPointerIndexEvidence `json:"index_evidence,omitempty"`
	LoadPath          []ShadowPointerPathStep     `json:"load_to_store_path,omitempty"`
	DataBank          ShadowRegisterEvidence      `json:"data_bank"`
	ROMBaseCandidates []uint32                    `json:"rom_base_candidates,omitempty"`
	ProofObligations  []string                    `json:"proof_obligations"`
}

type ShadowPointerIndexOrigin struct {
	PC       uint32 `json:"pc"`
	Mnemonic string `json:"mnemonic"`
	Mode     string `json:"mode"`
	Operand  uint32 `json:"operand"`
}

// Constants are definitions encountered on bounded predecessor paths. Unknown
// paths (including a callee or loop) prohibit treating them as an all-path value.
type ShadowRegisterEvidence struct {
	Constants    []ShadowRegisterConstant `json:"constants,omitempty"`
	UnknownPaths bool                     `json:"unknown_paths"`
}

type ShadowRegisterConstant struct {
	Value        uint16 `json:"value"`
	DefinitionPC uint32 `json:"definition_pc"`
}

type shadowPointerWalk struct {
	graph *decoder.Graph
	preds map[decoder.DecodeKey][]decoder.DecodeKey
}

const shadowPointerWalkLimit = 32

func (walk shadowPointerWalk) previous(key decoder.DecodeKey) *decoder.DecodedInstruction {
	// Entry is an external predecessor even if a backedge reaches it.
	if key == walk.graph.Entry || len(walk.preds[key]) != 1 {
		return nil
	}
	return walk.graph.Instructions[walk.preds[key][0]]
}

func shadowPointerRegister(mnemonic string) string {
	switch mnemonic {
	case "LDA", "STA":
		return "A"
	case "LDX", "STX":
		return "X"
	case "LDY", "STY":
		return "Y"
	}
	return ""
}

func shadowPointerWord(key decoder.DecodeKey, reg string) bool {
	return (reg == "A" && key.M == 0) || (reg != "A" && key.X == 0)
}

// Only instructions with understood register/memory effects may be crossed.
// Unsupported stack shuffles, arithmetic, calls, and aliasing writes end the
// query; this deliberately under-recovers rather than guessing a producer.
func shadowPointerTransparent(instruction *cpu65816.Instruction) bool {
	switch instruction.Mnemonic {
	case "NOP", "CMP", "CPX", "CPY", "BIT", "CLC", "SEC", "CLD", "SED", "CLI", "SEI", "CLV",
		"BCC", "BCS", "BEQ", "BNE", "BMI", "BPL", "BVC", "BVS", "BRA", "BRL":
		return true
	}
	return instruction.Mnemonic == "JMP" && instruction.Mode == cpu65816.ABS
}

func shadowPointerTransfer(mnemonic string) (string, string) {
	switch mnemonic {
	case "TAX":
		return "A", "X"
	case "TAY":
		return "A", "Y"
	case "TXA":
		return "X", "A"
	case "TYA":
		return "Y", "A"
	case "TXY":
		return "X", "Y"
	case "TYX":
		return "Y", "X"
	}
	return "", ""
}

func (walk shadowPointerWalk) registerLoad(key decoder.DecodeKey, reg string, word bool) *decoder.DecodedInstruction {
	for range shadowPointerWalkLimit {
		decoded := walk.previous(key)
		if decoded == nil || decoded.Instruction == nil {
			return nil
		}
		key = decoded.Key
		instruction := decoded.Instruction
		if word && !shadowPointerWord(key, reg) {
			return nil
		}
		if instruction.Mnemonic == "LD"+reg {
			return decoded
		}
		if src, dst := shadowPointerTransfer(instruction.Mnemonic); dst != "" {
			if dst == reg {
				if word && !shadowPointerWord(key, src) {
					return nil
				}
				reg = src
			}
			continue
		}
		if instruction.Mnemonic == "LDA" || instruction.Mnemonic == "LDX" || instruction.Mnemonic == "LDY" || shadowPointerTransparent(instruction) {
			continue
		}
		return nil
	}
	return nil
}

// bankOrD follows all predecessor alternatives under a shared work budget.
// Calls are barriers: even a stack-balanced callee need not preserve DB or D.
func (walk shadowPointerWalk) bankOrD(key decoder.DecodeKey, reg string) ShadowRegisterEvidence {
	var result ShadowRegisterEvidence
	seen := make(map[decoder.DecodeKey]bool)
	budget := shadowPointerWalkLimit
	var visit func(decoder.DecodeKey)
	visit = func(at decoder.DecodeKey) {
		if at == walk.graph.Entry || len(walk.preds[at]) == 0 {
			result.UnknownPaths = true
		}
		for _, prev := range walk.preds[at] {
			if seen[prev] || budget == 0 {
				result.UnknownPaths = true
				continue
			}
			seen[prev], budget = true, budget-1
			decoded := walk.graph.Instructions[prev]
			if decoded == nil || decoded.Instruction == nil {
				result.UnknownPaths = true
				continue
			}
			instruction := decoded.Instruction
			if instruction.Mnemonic == "JSR" || instruction.Mnemonic == "JSL" || instruction.Mnemonic == "RTI" || instruction.Mnemonic == "BRK" || instruction.Mnemonic == "COP" || instruction.Mnemonic == "WAI" {
				result.UnknownPaths = true
				continue
			}
			defines := (reg == "DB" && (instruction.Mnemonic == "PLB" || instruction.Mnemonic == "MVN" || instruction.Mnemonic == "MVP")) ||
				(reg == "D" && (instruction.Mnemonic == "PLD" || instruction.Mnemonic == "TCD"))
			if !defines {
				visit(prev)
				continue
			}
			var value uint16
			known := false
			producer := walk.previous(prev)
			if producer != nil && producer.Instruction != nil {
				push := producer.Instruction
				switch {
				case instruction.Mnemonic == "TCD" && push.Mnemonic == "LDA" && push.Mode == cpu65816.IMM && producer.Key.M == 0:
					value, known = uint16(push.Operand), true
				case (instruction.Mnemonic == "PLB" || instruction.Mnemonic == "PLD") && push.Mnemonic == "PEA":
					value, known = uint16(push.Operand), true
					if reg == "DB" {
						value &= 0xff // PEA pushes high then low; PLB pulls low.
					}
				case instruction.Mnemonic == "PLB" && push.Mnemonic == "PHK":
					value, known = uint16(producer.Key.PC>>16), true
				}
			}
			if known {
				result.Constants = append(result.Constants, ShadowRegisterConstant{Value: value, DefinitionPC: prev.PC})
			} else {
				result.UnknownPaths = true
			}
		}
	}
	visit(key)
	sort.Slice(result.Constants, func(i, j int) bool {
		a, b := result.Constants[i], result.Constants[j]
		if a.Value != b.Value {
			return a.Value < b.Value
		}
		return a.DefinitionPC < b.DefinitionPC
	})
	return result
}

func collectShadowPointerProducers(image romimage.Image, graph *decoder.Graph, regions []decoder.DataRegion) []ShadowPointerProducer {
	var producers []ShadowPointerProducer
	var walk shadowPointerWalk
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil || decoded.Instruction.Mnemonic != "JSR" || decoded.Instruction.Mode != cpu65816.ABS {
			continue
		}
		// One-hop near-call trampolines only. Never scan arbitrary ROM bytes
		// for a caller, infer a root, or silently fold program-bank mirrors.
		pc := key.PC&0xff0000 | decoded.Instruction.Operand&0xffff
		body, err := image.Slice(byte(pc>>16), uint16(pc), 3)
		if err != nil || body[0] != 0x6c || uint16(pc) > 0xfffd {
			continue
		}
		data := false
		for _, region := range regions {
			if region.Bank == byte(pc>>16) && uint32(region.Start) < (pc&0xffff)+3 && uint32(region.End) > pc&0xffff {
				data = true
			}
		}
		if data {
			continue
		}
		if walk.graph == nil {
			walk = shadowPointerWalk{graph: graph, preds: shadowPredecessors(graph)}
		}
		slot := uint16(body[1]) | uint16(body[2])<<8
		cursor := key
		for range shadowPointerWalkLimit {
			store := walk.previous(cursor)
			if store == nil || store.Instruction == nil {
				break
			}
			cursor = store.Key
			instruction := store.Instruction
			reg := shadowPointerRegister(instruction.Mnemonic)
			if instruction.Mode != cpu65816.DP || reg == "" || instruction.Mnemonic[0] != 'S' {
				if shadowPointerTransparent(instruction) {
					continue
				}
				break
			}
			if !shadowPointerWord(store.Key, reg) {
				break
			}
			d := walk.bankOrD(store.Key, "D")
			requiredD := slot - uint16(instruction.Operand)
			possible, proven := false, !d.UnknownPaths
			for _, constant := range d.Constants {
				possible = possible || constant.Value == requiredD
				proven = proven && constant.Value == requiredD
			}
			// An unknown D is a useful alias candidate only for equal operands.
			// Otherwise virtually every dp store would match every trampoline.
			if !possible && !(d.UnknownPaths && requiredD == 0) {
				break
			}
			load := walk.registerLoad(store.Key, reg, true)
			if load == nil {
				break
			}
			index := ""
			switch load.Instruction.Mode {
			case cpu65816.ABSX, cpu65816.LONGX:
				index = "X"
			case cpu65816.ABSY:
				index = "Y"
			}
			if index == "" {
				break
			}
			producer := ShadowPointerProducer{
				CallerEntryPC: graph.Entry.PC, CallPC: key.PC, TrampolinePC: pc,
				CallMX: analysis.MXState{M: key.M, X: key.X}, PointerAddress: slot,
				StorePC: store.Key.PC, StoreOperand: uint16(instruction.Operand), DirectPage: d, RequiredD: requiredD,
				PointerAlias: "conditional", LoadPC: load.Key.PC, LoadMX: analysis.MXState{M: load.Key.M, X: load.Key.X},
				LoadMode: load.Instruction.Mode.String(), LoadRegister: shadowPointerRegister(load.Instruction.Mnemonic), TableOffset: uint16(load.Instruction.Operand), IndexRegister: index,
				ProofObligations: []string{"index_domain_and_stride", "table_extent_and_ownership", "handler_entry_kind_and_live_mx", "pointer_survives_call_stack_and_interrupts"},
			}
			if proven && len(d.Constants) > 0 {
				producer.PointerAlias = "local_constant"
			} else {
				producer.ProofObligations = append(producer.ProofObligations, "direct_page_alias")
			}
			if origin := walk.registerLoad(load.Key, index, false); origin != nil {
				producer.IndexOrigin = &ShadowPointerIndexOrigin{PC: origin.Key.PC, Mnemonic: origin.Instruction.Mnemonic, Mode: origin.Instruction.Mode.String(), Operand: origin.Instruction.Operand}
				if wordOrigin := walk.registerLoad(load.Key, index, true); wordOrigin != nil && wordOrigin.Key == origin.Key {
					if path, valid := walk.pointerPath(origin.Key, load.Key); valid {
						field, isMemory := shadowMemoryField(origin.Instruction)
						producer.IndexEvidence = &ShadowPointerIndexEvidence{OriginRegister: shadowPointerRegister(origin.Instruction.Mnemonic), Path: path,
							WriterMatch: "same_address_expression_across_program_banks", ValueSetStatus: "literal_writer_candidates_not_a_domain"}
						if isMemory {
							producer.IndexEvidence.Field = &field
							producer.ProofObligations = append(producer.ProofObligations, "index_writer_alias_and_reaching_definition")
						} else if origin.Instruction.Mode == cpu65816.IMM {
							producer.IndexEvidence.WriterMatch = "literal_index_load"
							producer.IndexEvidence.ValueSetStatus = "local_literal_not_a_dispatch_domain"
							producer.IndexEvidence.Values = []ShadowPointerIndexValue{{Value: uint16(origin.Instruction.Operand)}}
						}
					}
				}
			}
			if path, valid := walk.pointerPath(load.Key, store.Key); valid {
				producer.LoadPath = path
			} else {
				// An absent path must not be mistaken for a path with no guards.
				producer.IndexEvidence = nil
			}
			if load.Instruction.Mode == cpu65816.LONGX {
				producer.DataBank.Constants = []ShadowRegisterConstant{{Value: uint16(load.Instruction.Operand >> 16), DefinitionPC: load.Key.PC}}
			} else {
				producer.DataBank = walk.bankOrD(load.Key, "DB")
			}
			if producer.DataBank.UnknownPaths || len(producer.DataBank.Constants) == 0 {
				producer.ProofObligations = append(producer.ProofObligations, "data_bank_at_load")
			}
			for _, constant := range producer.DataBank.Constants {
				bank := byte(constant.Value)
				if bank == 0x7e || bank == 0x7f {
					continue
				}
				if _, err := image.Slice(bank, producer.TableOffset, 2); err == nil {
					producer.ROMBaseCandidates = appendUniqueAddresses(producer.ROMBaseCandidates, uint32(bank)<<16|uint32(producer.TableOffset))
				}
			}
			producers = append(producers, producer)
			break
		}
	}
	return producers
}

func normalizeShadowPointerProducers(producers []ShadowPointerProducer) []ShadowPointerProducer {
	// Include widths and evidence in identity, but discard duplicated abstract
	// PHP-stack paths. JSON supplies a deterministic, complete value key.
	unique := make(map[string]ShadowPointerProducer)
	for _, producer := range producers {
		key, _ := json.Marshal(producer)
		unique[string(key)] = producer
	}
	keys := make([]string, 0, len(unique))
	for key := range unique {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	result := make([]ShadowPointerProducer, 0, len(keys))
	for _, key := range keys {
		result = append(result, unique[key])
	}
	return result
}
