package tooling

import (
	"fmt"
	"io"
	"slices"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// A nearest same-spelling scalar writer on a decoded local path. Even with
// zero recorded clobbers this is not an alias/lifetime or reaching-value proof.
type ShadowLocalSlotSource struct {
	EntryPC          uint32                 `json:"entry_pc"`
	EntryMX          analysis.MXState       `json:"entry_mx"`
	StorePC          uint32                 `json:"store_pc"`
	LoadPC           uint32                 `json:"load_pc"`
	Field            ShadowStoredField      `json:"field"`
	Value            ShadowInitializerIndex `json:"stored_value_expression"`
	PossibleClobbers []ShadowSlotClobber    `json:"possible_alias_writes,omitempty"`
	Calls            []ShadowEntryCallInput `json:"direct_caller_inputs,omitempty"`
	CallScope        string                 `json:"call_scope"`
	Obligations      []string               `json:"proof_obligations"`
}

type ShadowSlotClobber struct {
	PC       uint32 `json:"pc"`
	Mnemonic string `json:"mnemonic"`
	Mode     string `json:"mode"`
	Operand  uint32 `json:"operand"`
}

type ShadowEntryCallInput struct {
	CallPC           uint32                 `json:"call_pc"`
	CallerEntryPC    uint32                 `json:"caller_entry_pc"`
	CallerEntryMX    analysis.MXState       `json:"caller_entry_mx"`
	TargetPC         uint32                 `json:"target_pc"`
	CallMX           analysis.MXState       `json:"call_mx"`
	MatchesEntryMX   bool                   `json:"entry_mx_matches"`
	InstructionBytes string                 `json:"instruction_bytes"`
	Register         string                 `json:"register"`
	Value            ShadowInitializerIndex `json:"value_expression"`
	SourceEvidence   *ShadowCallerSource    `json:"source_evidence,omitempty"`
}

type shadowDirectCallInputs struct {
	call    ShadowEntryCallInput
	values  [3]ShadowInitializerIndex
	sources [3]*ShadowCallerSource
}

const shadowLocalSlotLimit = 128

func (walk shadowPointerWalk) localSlotSource(at decoder.DecodeKey, field ShadowStoredField) *ShadowLocalSlotSource {
	// Indexed destinations need index/allocation identity in addition to the
	// scalar-slot obligations. Do not silently treat them as scratch slots.
	if field.Mode != "dp" && field.Mode != "abs" && field.Mode != "long" {
		return nil
	}
	result := &ShadowLocalSlotSource{EntryPC: walk.graph.Entry.PC, EntryMX: analysis.MXState{M: walk.graph.Entry.M, X: walk.graph.Entry.X}, LoadPC: at.PC, Field: field,
		CallScope:   "decoded_direct_callers_only_not_all_entries",
		Obligations: []string{"same_slot_D_DB_wrap_and_lifetime", "intervening_alias_writes", "decoded_path_not_reachability", "indirect_callers_and_external_entries", "HLE_and_entry_contract", "no_value_substitution_or_root_promotion"}}
	for range shadowLocalSlotLimit {
		previous := walk.previous(at)
		if previous == nil || previous.Instruction == nil {
			return nil
		}
		at = previous.Key
		ins := previous.Instruction
		written, valid := shadowMemoryField(ins)
		write := false
		switch ins.Mnemonic {
		case "STA", "STX", "STY", "STZ", "INC", "DEC", "ASL", "LSR", "ROL", "ROR", "TRB", "TSB":
			write = valid
		}
		if write {
			if written == field {
				reg := shadowPointerRegister(ins.Mnemonic)
				if reg == "" || !shadowPointerWord(at, reg) {
					return nil
				} // Nearest partial/RMW/zero write is not a prior register definition.
				result.StorePC = at.PC
				result.Value = walk.indexExpression(at, reg, true)
				slices.Reverse(result.PossibleClobbers)
				return result
			}
			result.PossibleClobbers = append(result.PossibleClobbers, ShadowSlotClobber{PC: at.PC, Mnemonic: ins.Mnemonic, Mode: ins.Mode.String(), Operand: ins.Operand})
			continue
		}
		// No crosses of bank/D changes, calls, stack writes, status restoration,
		// or unmodeled memory effects; they may invalidate a same-spelling slot.
		if shadowPointerTransparent(ins) {
			continue
		}
		if _, dst := shadowPointerTransfer(ins.Mnemonic); dst != "" {
			continue
		}
		switch ins.Mnemonic {
		case "LDA", "LDX", "LDY", "ADC", "SBC", "AND", "ORA", "EOR", "INX", "INY", "DEX", "DEY", "XBA":
			continue
		case "ASL", "LSR", "ROL", "ROR", "INC", "DEC":
			if ins.Mode == cpu65816.ACC {
				continue
			}
		}
		return nil
	}
	return nil
}

func collectShadowDirectCallInputs(graph *decoder.Graph) []shadowDirectCallInputs {
	var result []shadowDirectCallInputs
	var walk shadowPointerWalk
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded == nil || decoded.Instruction == nil {
			continue
		}
		ins := decoded.Instruction
		if ins.DispatchKind != "" || ins.DispatchEntries != nil {
			continue
		}
		var target uint32
		switch {
		case ins.Opcode == 0x20 && ins.Mnemonic == "JSR" && ins.Mode == cpu65816.ABS:
			target = key.PC&0xff0000 | ins.Operand&0xffff
		case ins.Opcode == 0x22 && ins.Mnemonic == "JSL" && ins.Mode == cpu65816.LONG:
			target = ins.Operand & 0xffffff
		default:
			continue
		}
		if walk.graph == nil {
			walk = shadowPointerWalk{graph: graph, preds: shadowPredecessors(graph)}
		}
		bytes := []string{fmt.Sprintf("%02X", ins.Opcode)}
		for i := uint8(1); i < ins.Length; i++ {
			bytes = append(bytes, fmt.Sprintf("%02X", byte(ins.Operand>>(8*(i-1)))))
		}
		input := shadowDirectCallInputs{call: ShadowEntryCallInput{CallPC: key.PC, CallerEntryPC: graph.Entry.PC, CallerEntryMX: analysis.MXState{M: graph.Entry.M, X: graph.Entry.X}, TargetPC: target, CallMX: analysis.MXState{M: key.M, X: key.X}, InstructionBytes: strings.Join(bytes, " ")}}
		for i, reg := range []string{"A", "X", "Y"} {
			input.values[i] = walk.indexExpression(key, reg, true)
			input.sources[i] = walk.callerSource(input.values[i])
		}
		result = append(result, input)
	}
	return result
}

func attachShadowEntryCallInputs(results []shadowDecodeResult, sites map[uint32]*ShadowDispatchSite) {
	var sources []*ShadowLocalSlotSource
	wanted := make(map[uint32]bool)
	var visit func(*ShadowInitializerRead)
	visit = func(read *ShadowInitializerRead) {
		if read.Index.LocalSource != nil {
			copy := *read.Index.LocalSource
			read.Index.LocalSource = &copy
			if copy.Value.Source.Kind == "entry_register" && copy.Value.Source.PC == copy.EntryPC {
				sources = append(sources, &copy)
				wanted[copy.EntryPC] = true
			}
		}
		if read.PointerSource != nil {
			visit(read.PointerSource)
		} // Already copied by attachInitializerSamples.
	}
	for _, site := range sites {
		for i := range site.PointerProducers {
			if e := site.PointerProducers[i].IndexEvidence; e != nil {
				for j := range e.Initializers {
					visit(&e.Initializers[j].Read)
				}
			}
		}
	}
	byTarget := make(map[uint32][]shadowDirectCallInputs)
	for _, result := range results {
		if result.issue != nil {
			continue
		}
		for _, input := range result.callInputs {
			if wanted[input.call.TargetPC] {
				byTarget[input.call.TargetPC] = append(byTarget[input.call.TargetPC], input)
			}
		}
	}
	for _, source := range sources {
		reg := source.Value.Source.Register
		index := strings.Index("AXY", reg)
		if len(reg) != 1 || index < 0 {
			continue
		}
		seen := make(map[string]bool)
		for _, raw := range byTarget[source.EntryPC] {
			input := raw.call
			input.Register = reg
			input.Value = raw.values[index]
			input.SourceEvidence = cloneShadowCallerSource(raw.sources[index])
			input.MatchesEntryMX = input.CallMX == source.EntryMX
			id := shadowStoredJSONKey(input)
			if seen[id] {
				continue
			}
			seen[id] = true
			source.Calls = append(source.Calls, input)
		}
		sort.Slice(source.Calls, func(i, j int) bool {
			if source.Calls[i].CallPC != source.Calls[j].CallPC {
				return source.Calls[i].CallPC < source.Calls[j].CallPC
			}
			return shadowStoredJSONKey(source.Calls[i]) < shadowStoredJSONKey(source.Calls[j])
		})
	}
}

func writeShadowLocalSlot(out io.Writer, local *ShadowLocalSlotSource, indent string) {
	if local == nil {
		return
	}
	fmt.Fprintf(out, "%slocal-slot candidate: writer=%s load=%s entry=%s source=%s register=%s at=%s possible-alias-writes=%d\n", indent, shadowAddress(local.StorePC), shadowAddress(local.LoadPC), shadowAddress(local.EntryPC), local.Value.Source.Kind, local.Value.Source.Register, shadowAddress(local.Value.Source.PC), len(local.PossibleClobbers))
	for _, write := range local.PossibleClobbers {
		fmt.Fprintf(out, "%spossible-alias=%s %s %s $%X\n", indent, shadowAddress(write.PC), write.Mnemonic, write.Mode, write.Operand)
	}
	for _, call := range local.Calls {
		fmt.Fprintf(out, "%scaller-input: %s [%s] -> %s M%dX%d entry-mx-match=%t %s source=%s %s $%X at=%s reason=%s domain-superset=%d known-zero=$%04X known-one=$%04X\n", indent, shadowAddress(call.CallPC), call.InstructionBytes, shadowAddress(call.TargetPC), call.CallMX.M, call.CallMX.X, call.MatchesEntryMX, call.Register, call.Value.Source.Kind, call.Value.Source.Mode, call.Value.Source.Operand, shadowAddress(call.Value.Source.PC), call.Value.Source.Reason, call.Value.DomainSize, call.Value.KnownZero, call.Value.KnownOne)
		writeShadowCallerSource(out, call.SourceEvidence, indent+"  ")
	}
	fmt.Fprintf(out, "%sscope=%s obligations=%v\n", indent, local.CallScope, local.Obligations)
}
