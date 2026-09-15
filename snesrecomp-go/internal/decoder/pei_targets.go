package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
)

// PEIReturnTargets inventories a bounded DP word immediately pushed for RTS.
// This is open address evidence, not an alias/closed-dispatch proof. In
// particular it never changes native PEI, stack pops, or live-width dispatch.
// Unique predecessor walks stop at calls, pointer changes and possible aliases.
func PEIReturnTargets(g *Graph) []uint32 {
	pred := map[DecodeKey][]DecodeKey{}
	for _, d := range g.Instructions {
		for _, next := range d.Successors {
			pred[next] = append(pred[next], d.Key)
		}
	}
	previous := func(k DecodeKey) (DecodeKey, bool) {
		p := pred[k]
		if len(p) != 1 {
			return DecodeKey{}, false
		}
		return p[0], true
	}
	// Recover only immediate A or an eight-bit load constrained by immediate
	// logic/shifts. No guessed ROM contents, incoming flags or register values.
	accumulator := func(k DecodeKey) (uint16, uint16, bool) {
		var path []*DecodedInstruction
		for n := 0; n < 64; n++ {
			var ok bool
			k, ok = previous(k)
			if !ok {
				return 0, 0, false
			}
			d := g.Instructions[k]
			i := d.Instruction
			if i.Mnemonic == "LDA" {
				mask, value := uint16(0), uint16(0)
				if i.Mode == cpu65816.IMM {
					mask = 0xffff
					if d.Key.M != 0 {
						mask = 0xff // M8 LDA preserves the unknown accumulator high byte.
					}
					value = uint16(i.Operand)
				}
				for p := len(path) - 1; p >= 0; p-- {
					q := path[p]
					ins := q.Instruction
					w := uint16(0xffff)
					if q.Key.M != 0 {
						w = 0xff
					}
					m, v := mask&w, value&w
					switch ins.Mnemonic {
					case "AND":
						m |= ^uint16(ins.Operand) & w
						v &= uint16(ins.Operand)
					case "ORA":
						m |= uint16(ins.Operand) & w
						v |= uint16(ins.Operand) & w
					case "EOR":
						v ^= uint16(ins.Operand) & w
					case "LSR":
						m = (m >> 1) | (w ^ (w >> 1))
						v >>= 1
					case "ASL":
						m = (m << 1) | 1
						v <<= 1
					}
					mask = (mask &^ w) | (m & w)
					value = ((value &^ w) | (v & w)) & mask
				}
				return mask, value, true
			}
			switch i.Mnemonic {
			case "AND", "ORA", "EOR":
				if i.Mode != cpu65816.IMM {
					return 0, 0, false
				}
				path = append(path, d)
			case "ASL", "LSR":
				if i.Mode != cpu65816.ACC {
					return 0, 0, false
				}
				path = append(path, d)
			case "STA", "STX", "STY", "STZ", "LDX", "LDY", "INX", "INY", "DEX", "DEY", "REP", "SEP", "NOP", "CLC", "SEC", "CLD", "SED", "BRA", "BRL":
			default:
				return 0, 0, false
			}
		}
		return 0, 0, false
	}
	lane := func(k DecodeKey, slot uint16) (byte, byte, bool) {
		for n := 0; n < 128; n++ {
			var ok bool
			k, ok = previous(k)
			if !ok {
				return 0, 0, false
			}
			d := g.Instructions[k]
			i := d.Instruction
			switch i.Mnemonic {
			case "STA", "STX", "STY", "STZ":
				if i.Mode != cpu65816.DP {
					return 0, 0, false
				}
				width := 2 - int(d.Key.M)
				if i.Mnemonic == "STX" || i.Mnemonic == "STY" {
					width = 2 - int(d.Key.X)
				}
				if slot < uint16(i.Operand) || int(slot-uint16(i.Operand)) >= width {
					continue
				}
				if i.Mnemonic == "STZ" {
					return 255, 0, true
				}
				if i.Mnemonic != "STA" {
					return 0, 0, false
				}
				m, v, found := accumulator(k)
				shift := 8 * (slot - uint16(i.Operand))
				return byte(m >> shift), byte(v >> shift), found
			case "JSR", "JSL", "TCD", "PLD", "XCE", "MVN", "MVP", "TSB", "TRB":
				return 0, 0, false
			case "INC", "DEC", "ASL", "LSR", "ROL", "ROR":
				if i.Mode != cpu65816.ACC {
					return 0, 0, false
				}
			}
		}
		return 0, 0, false
	}
	targets := map[uint32]bool{}
	for _, d := range g.Instructions {
		i := d.Instruction
		if i.Mnemonic != "PEI" || len(d.Successors) != 1 {
			continue
		}
		next := g.Instructions[d.Successors[0]]
		if next == nil || next.Instruction.Mnemonic != "RTS" {
			continue
		}
		lm, lv, lok := lane(d.Key, uint16(i.Operand))
		hm, hv, hok := lane(d.Key, uint16(i.Operand)+1)
		if !lok || !hok {
			continue
		}
		mask, value := uint16(hm)<<8|uint16(lm), uint16(hv)<<8|uint16(lv)
		unknown := ^mask
		var bits []uint
		for b := uint(0); b < 16; b++ {
			if unknown&(1<<b) != 0 {
				bits = append(bits, b)
			}
		}
		if len(bits) > 6 {
			continue
		}
		for n := 0; n < (1 << len(bits)); n++ {
			word := value
			for j, b := range bits {
				if n&(1<<j) != 0 {
					word |= 1 << b
				}
			}
			targets[(i.Address&0xff0000)|uint32(word+1)] = true
		}
	}
	var result []uint32
	for target := range targets {
		result = append(result, target)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
}
