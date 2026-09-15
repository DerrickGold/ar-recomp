package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// ForwardedIndirectField is a conditional native value-flow shape, not a
// must-alias or closed target proof. An indexed word is loaded into A, staged
// in a direct-page slot, and consumed by JMP (abs). The path may depend on a
// branch; pushes may alias the scratch slot. D/DB, index equality, mutable-field
// lifetime and native return ownership remain explicit caller obligations.
type ForwardedIndirectField struct {
	LoadPC         uint32   `json:"load_pc"`
	Mode           string   `json:"field_mode"`
	Operand        uint16   `json:"field_operand"`
	StorePC        uint32   `json:"scratch_store_pc"`
	ScratchOperand uint16   `json:"scratch_store_operand"`
	DispatchPC     uint32   `json:"dispatch_pc"`
	PointerAddress uint16   `json:"pointer_address"`
	RequiredD      uint16   `json:"required_d_for_scratch"`
	ProgramBank    byte     `json:"decoded_program_bank"`
	M              uint8    `json:"m"`
	X              uint8    `json:"x"`
	Path           []uint32 `json:"required_native_path"` // load through dispatch, inclusive
}

const forwardedFieldWalkLimit = 32
const forwardedFieldValueLimit = 64

func ForwardedIndirectFields(g *Graph) []ForwardedIndirectField {
	pred := forwardedFieldPredecessors(g)
	var out []ForwardedIndirectField
	for _, key := range g.Order {
		d := g.Instructions[key]
		if d == nil || d.Instruction == nil || d.Instruction.Opcode != 0x6c || key.M != 0 {
			continue
		}
		r := ForwardedIndirectField{DispatchPC: key.PC, PointerAddress: uint16(d.Instruction.Operand), ProgramBank: byte(key.PC >> 16), M: key.M, X: key.X, Path: []uint32{key.PC}}
		at := key
		stored := false
		for range forwardedFieldWalkLimit {
			p := forwardedFieldPrevious(g, pred, at)
			if p == nil || p.Key.M != key.M || p.Key.X != key.X {
				break
			}
			at = p.Key
			i := p.Instruction
			r.Path = append(r.Path, at.PC)
			if !stored && i.Mnemonic == "STA" && i.Mode == cpu65816.DP {
				r.StorePC, r.ScratchOperand = at.PC, uint16(i.Operand)
				r.RequiredD = r.PointerAddress - r.ScratchOperand
				stored = true
				continue
			}
			if stored && i.Mnemonic == "LDA" && (i.Mode == cpu65816.DPX || i.Mode == cpu65816.ABSX) {
				r.LoadPC, r.Mode, r.Operand = at.PC, i.Mode.String(), uint16(i.Operand)
				for i, j := 0, len(r.Path)-1; i < j; i, j = i+1, j-1 {
					r.Path[i], r.Path[j] = r.Path[j], r.Path[i]
				}
				out = append(out, r)
				break
			}
			// Deliberately narrow: unknown writes, register changes, calls,
			// status changes and pulls other than PLB are barriers. Conditional
			// branches are retained as path dependencies, never proven feasible.
			switch i.Mnemonic {
			case "NOP", "CLC", "SEC", "CLD", "SED", "PHB", "PHK", "PLB", "PHA", "PHX", "PHY", "PEA",
				"BEQ", "BNE", "BCC", "BCS", "BMI", "BPL", "BVC", "BVS", "BRA", "BRL":
			case "JMP":
				if i.Mode != cpu65816.ABS {
					goto nextDispatch
				}
			default:
				goto nextDispatch
			}
		}
	nextDispatch:
	}
	sort.Slice(out, func(i, j int) bool {
		a, b := out[i], out[j]
		if a.DispatchPC != b.DispatchPC {
			return a.DispatchPC < b.DispatchPC
		}
		if a.M != b.M {
			return a.M < b.M
		}
		return a.X < b.X
	})
	return out
}

func forwardedFieldPredecessors(g *Graph) map[DecodeKey][]DecodeKey {
	p := make(map[DecodeKey][]DecodeKey)
	for _, d := range g.Instructions {
		if d != nil {
			for _, s := range d.Successors {
				p[s] = append(p[s], d.Key)
			}
		}
	}
	return p
}

func forwardedFieldPrevious(g *Graph, pred map[DecodeKey][]DecodeKey, key DecodeKey) *DecodedInstruction {
	if key == g.Entry || len(pred[key]) != 1 {
		return nil
	}
	p := g.Instructions[pred[key][0]]
	if p == nil || p.Instruction == nil {
		return nil
	}
	return p
}

// ForwardedFieldLiteralTargets inventories literal word writes to the same
// indexed field spelling as a decoded forwarding consumer. This is OPTIONAL
// cold AOT coverage, never a claim that an instance/alias/path reaches a target.
// Dynamic stores remain unknown; nothing scans ROM stream data for addresses.
// Callers must exclude HLE/body-overridden graphs, retain live native dispatch,
// preserve authored boundaries, and not serialize these as proven facts.
func ForwardedFieldLiteralTargets(image rom.Image, graphs []*Graph) []uint32 {
	type field struct {
		mode    string
		operand uint16
	}
	consumers := make(map[field]map[byte]bool)
	for _, g := range graphs {
		for _, r := range ForwardedIndirectFields(g) {
			f := field{r.Mode, r.Operand}
			if consumers[f] == nil {
				consumers[f] = make(map[byte]bool)
			}
			consumers[f][r.ProgramBank] = true
		}
	}
	writes := make(map[field]map[uint16]bool)
	for _, g := range graphs {
		pred := forwardedFieldPredecessors(g)
		for _, key := range g.Order {
			d := g.Instructions[key]
			if d == nil || d.Instruction == nil || key.M != 0 {
				continue
			}
			i := d.Instruction
			f := field{i.Mode.String(), uint16(i.Operand)}
			if i.Mnemonic != "STA" || len(consumers[f]) == 0 {
				continue
			}
			at := key
			for range forwardedFieldWalkLimit {
				p := forwardedFieldPrevious(g, pred, at)
				if p == nil || p.Key.M != key.M || p.Key.X != key.X {
					break
				}
				at = p.Key
				i = p.Instruction
				if i.Mnemonic == "LDA" {
					if i.Mode == cpu65816.IMM {
						if writes[f] == nil {
							writes[f] = make(map[uint16]bool)
						}
						// Retain an overflow sentinel, not an arbitrary prefix.
						if len(writes[f]) <= forwardedFieldValueLimit {
							writes[f][uint16(i.Operand)] = true
						}
					}
					break
				}
				switch i.Mnemonic {
				case "NOP", "CLC", "SEC", "CLD", "SED", "PHB", "PHK", "PLB", "PHA", "PHX", "PHY", "PEA",
					"LDX", "LDY", "STA", "STX", "STY", "STZ", "BRA", "BRL":
				case "JMP":
					if i.Mode != cpu65816.ABS {
						goto nextStore
					}
				default:
					goto nextStore
				}
			}
		nextStore:
		}
	}
	targets := make(map[uint32]bool)
	for f, values := range writes {
		if len(values) > forwardedFieldValueLimit {
			continue
		}
		for bank := range consumers[f] {
			for value := range values {
				if image.IsROM(bank, value) {
					targets[Address24(bank, value)] = true
				}
			}
		}
	}
	var out []uint32
	for target := range targets {
		out = append(out, target)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}
