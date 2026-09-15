package tooling

import (
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type queuedField struct {
	mode cpu65816.AddressingMode
	base uint16
}
type queuedLane struct {
	field        queuedField
	index, space int
	known        bool
}

func queuedIndex(mode cpu65816.AddressingMode) int {
	switch mode {
	case cpu65816.ABSX, cpu65816.DPX:
		return 0
	case cpu65816.ABSY:
		return 1
	}
	return -1
}
func queuedIndexWrite(mnemonic string) int {
	switch mnemonic {
	case "LDX", "PLX", "INX", "DEX", "TAX", "TYX", "TSX":
		return 0
	case "LDY", "PLY", "INY", "DEY", "TAY", "TXY":
		return 1
	}
	return -1
}

// Follow three correlated record-byte origins through DP scratch writes into
// JML [abs]. Index/DB changes between record reads cannot combine two objects.
// D=0 at scratch stores, stack non-aliasing and record lifetime remain conditions.
func queuedLongFields(g *decoder.Graph) map[queuedField]bool {
	out := map[queuedField]bool{}
	var w shadowPointerWalk
	for key, end := range g.Instructions {
		if end.Instruction.Opcode != 0xdc || end.Instruction.Operand >= 0x1ffe || key.M != 0 || key.X != 0 {
			continue
		}
		if w.graph == nil {
			w = shadowPointerWalk{graph: g, preds: shadowPredecessors(g)}
		}
		path := []*decoder.DecodedInstruction{end}
		at := key
		for len(path) < 32 {
			p := w.previous(at)
			if p == nil || p.Key.M != 0 || p.Key.X != 0 {
				break
			}
			path = append(path, p)
			at = p.Key
		}
		slices.Reverse(path)
		var a queuedLane
		var lanes [3]queuedLane
		var epoch [2]int
		var db, dp int
		for _, d := range path {
			i := d.Instruction
			if n := queuedIndexWrite(i.Mnemonic); n >= 0 {
				epoch[n]++
			}
			switch i.Mnemonic {
			case "LDA":
				a = queuedLane{}
				if n := queuedIndex(i.Mode); n >= 0 && i.Operand < 0x1ffe {
					space := db
					if i.Mode == cpu65816.DPX {
						space = dp
					}
					a = queuedLane{queuedField{i.Mode, uint16(i.Operand)}, epoch[n], space, true}
				}
			case "STA", "STX", "STY", "STZ":
				if i.Mode != cpu65816.DP {
					lanes = [3]queuedLane{}
					continue
				}
				for n := 0; n < 2; n++ {
					lane := int(i.Operand) + n - int(end.Instruction.Operand)
					if lane < 0 || lane >= 3 {
						continue
					}
					lanes[lane] = queuedLane{}
					if i.Mnemonic == "STA" && a.known {
						lanes[lane] = a
						lanes[lane].field.base += uint16(n)
					}
				}
			case "PLB":
				db++
			case "TCD", "PLD":
				dp++
				lanes = [3]queuedLane{}
			case "LDX", "LDY", "INX", "DEX", "INY", "DEY", "TAX", "TYX", "TSX", "TAY", "TXY":
			case "NOP", "CLC", "SEC", "CLD", "SED", "PHB", "PHK", "PHA", "PHX", "PHY", "PEA", "PER", "PHP",
				"BEQ", "BNE", "BCC", "BCS", "BMI", "BPL", "BVC", "BVS", "BRA", "BRL", "CMP", "CPX", "CPY", "BIT":
			case "JMP":
				if d != end && i.Mode != cpu65816.ABS {
					a = queuedLane{}
					lanes = [3]queuedLane{}
				}
			case "JML":
				if d != end {
					a = queuedLane{}
					lanes = [3]queuedLane{}
				}
			default:
				a = queuedLane{}
				lanes = [3]queuedLane{}
			}
		}
		f := lanes[0]
		if !f.known {
			continue
		}
		valid := true
		for n := 1; n < 3; n++ {
			p := lanes[n]
			if !p.known || p.field.mode != f.field.mode || p.field.base != f.field.base+uint16(n) || p.index != f.index || p.space != f.space {
				valid = false
			}
		}
		if valid {
			out[f.field] = true
		}
	}
	return out
}

// Entry A supplies a record word; the low byte of LDA $03,S supplies the
// caller's native JSL bank, before any stack adjustment. A word bank store may
// overlap later metadata; only its low byte is part of the callback pointer.
func queuedSetter(g *decoder.Graph, fields map[queuedField]bool) bool {
	if g.Entry.M != 0 || g.Entry.X != 0 || g.Entry.PDepth != 0 {
		return false
	}
	at := g.Entry
	origin := 1 // 1: entry A; 2: native JSL bank byte; 0: unknown
	var epoch [2]int
	words := map[queuedField]int{}
	seen := map[decoder.DecodeKey]bool{}
	for range 32 {
		d := g.Instructions[at]
		if d == nil || d.Instruction == nil || at.M != 0 || at.X != 0 || seen[at] {
			return false
		}
		seen[at] = true
		i := d.Instruction
		if n := queuedIndexWrite(i.Mnemonic); n >= 0 {
			epoch[n]++
		}
		switch i.Mnemonic {
		case "LDA":
			origin = 0
			if i.Mode == cpu65816.STK && i.Operand == 3 {
				origin = 2
			}
		case "STA":
			n := queuedIndex(i.Mode)
			if n < 0 {
				return false
			}
			f := queuedField{i.Mode, uint16(i.Operand)}
			if origin == 1 && fields[f] {
				words[f] = epoch[n]
			}
			if origin == 2 && f.base >= 2 {
				f.base -= 2
				if e, ok := words[f]; ok && e == epoch[n] {
					return true
				}
			}
		case "LDX", "LDY", "INX", "DEX", "INY", "DEY", "NOP", "CLC", "SEC", "CLD", "SED", "BRA", "BRL":
		case "JMP":
			if i.Mode != cpu65816.ABS {
				return false
			}
		default:
			return false // calls, pushes/pulls and S/status changes are barriers
		}
		if len(d.Successors) != 1 {
			return false
		}
		at = d.Successors[0]
	}
	return false
}

// QueuedCallbackTargets joins decoded long-pointer consumers, native record
// setters and finite direct-JSL arguments. It imports no observations. These
// are cold conditional roots, not a proven queue, reaching definition or edge:
// D/DB/index aliases, no record write clobbering the JSL frame, lifetime and
// pointer publication remain obligations. Native queue operations and live
// M/X dispatch must remain intact. eligible must exclude authored replacements.
func QueuedCallbackTargets(image rom.Image, configs map[byte]*config.Config, graphs, eligible []*decoder.Graph) []uint32 {
	native, a := coldNativeGraphs(image, configs, graphs, eligible)
	fields := map[queuedField]bool{}
	for _, g := range native {
		for f := range queuedLongFields(g) {
			fields[f] = true
		}
	}
	if len(fields) == 0 {
		return nil
	}
	setters := map[int]bool{}
	for _, g := range native {
		if queuedSetter(g, fields) {
			if off, ok := a.offset(g.Entry.PC); ok {
				setters[off] = true
			}
		}
	}
	targets := map[uint32]bool{}
	for _, g := range native {
		var w shadowPointerWalk
		for key, d := range g.Instructions {
			if d.Instruction.Opcode != 0x22 || key.M != 0 || key.X != 0 {
				continue
			}
			off, ok := a.offset(d.Instruction.Operand)
			if !ok || !setters[off] {
				continue
			}
			if w.graph == nil {
				w = shadowPointerWalk{graph: g, preds: shadowPredecessors(g)}
			}
			input := w.indexExpression(key, "A", true)
			for _, word := range input.DomainValues {
				pc := key.PC&0xff0000 | uint32(word)
				if coldMappedTarget(a, pc) {
					targets[pc] = true
				}
			}
		}
	}
	var out []uint32
	for pc := range targets {
		out = append(out, pc)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}
