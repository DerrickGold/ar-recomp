package decoder

import (
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// StoredAddressReferences is an experimental, open inventory of literal
// values stored into slots also used by decoded JMP/JML instructions. It is
// not a must-alias or closed-target proof (DP/DB and writer order may vary).
// Consumers may emit cold AOT roots but must retain actual-pointer/live-MX
// runtime dispatch, authored data/HLE boundaries and hard misses.
func StoredAddressReferences(image rom.Image, graphs []*Graph) []uint32 {
	type store struct {
		value uint16
		width int
	}
	type pointer struct {
		slot uint16
		bank byte
		long bool
	}
	writes := map[uint16]map[store]bool{}
	structuredTargets := map[uint32]bool{}
	reads := map[pointer]bool{}
	peiSlots := map[uint16]map[byte]bool{}
	type parameter struct {
		reg   string
		slot  uint16
		width int
		m, x  uint8
	}
	parameters := map[int]map[parameter]bool{}
	for _, g := range graphs {
		bases := map[uint16]map[uint16]bool{}
		var localReads []pointer
		unknownWrites := map[uint16]bool{}
		pred := map[DecodeKey][]DecodeKey{}
		for _, d := range g.Instructions {
			for _, s := range d.Successors {
				pred[s] = append(pred[s], d.Key)
			}
		}
		for _, d := range g.Instructions {
			i := d.Instruction
			if i == nil {
				continue
			}
			if i.Opcode == 0x6c || i.Opcode == 0xdc {
				localReads = append(localReads, pointer{uint16(i.Operand), byte(i.Address >> 16), i.Opcode == 0xdc})
			}
			if i.Mnemonic == "PEI" && len(d.Successors) == 1 {
				next := g.Instructions[d.Successors[0]]
				if next != nil && next.Instruction.Mnemonic == "RTS" {
					slot := uint16(i.Operand)
					if peiSlots[slot] == nil {
						peiSlots[slot] = map[byte]bool{}
					}
					peiSlots[slot][byte(i.Address>>16)] = true
				}
			}
			if i.Mnemonic != "STA" && i.Mnemonic != "STX" && i.Mnemonic != "STY" {
				continue
			}
			if i.Mode != cpu65816.DP && i.Mode != cpu65816.ABS {
				continue
			}
			if i.Operand >= 0x1fff {
				continue
			}
			reg := i.Mnemonic[2:]
			width := 2 - int(d.Key.X)
			if reg == "A" {
				width = 2 - int(d.Key.M)
			}
			key := d.Key
			literal := false
			for step := 0; step < 32; step++ {
				p := pred[key]
				if len(p) != 1 {
					if len(p) > 1 {
						// A shared setter may merge several literal-producing
						// paths. Keep the finite union only when every incoming
						// path is understood; never select a canonical arm.
						if values, complete := storedJoinedLiterals(g, pred, key, reg, width); complete {
							slot := uint16(i.Operand)
							if writes[slot] == nil {
								writes[slot] = map[store]bool{}
							}
							for _, value := range values {
								writes[slot][store{value, width}] = true
							}
							literal = true
						}
					}
					if key == g.Entry {
						if offset, err := image.Offset(byte(key.PC>>16), uint16(key.PC)); err == nil {
							if parameters[offset] == nil {
								parameters[offset] = map[parameter]bool{}
							}
							parameters[offset][parameter{reg, uint16(i.Operand), width, key.M, key.X}] = true
						}
					}
					break
				}
				key = p[0]
				previous := g.Instructions[key]
				if previous == nil || previous.Instruction == nil {
					break
				}
				ins := previous.Instruction
				if reg == "A" && width == 2 && ins.Mnemonic == "ADC" && ins.Mode == cpu65816.IMM {
					slot := uint16(i.Operand)
					if bases[slot] == nil {
						bases[slot] = map[uint16]bool{}
					}
					bases[slot][uint16(ins.Operand)] = true
				}
				if ins.Mnemonic == "LD"+reg {
					if ins.Mode == cpu65816.IMM {
						v := uint16(ins.Operand)
						if width == 1 {
							v &= 255
						}
						slot := uint16(i.Operand)
						if writes[slot] == nil {
							writes[slot] = map[store]bool{}
						}
						writes[slot][store{v, width}] = true
						literal = true
					}
					break
				}
				// Only transparent instructions may separate a literal load and
				// store. Reject transfers/arithmetic/pulls and unknown callees.
				if ins.Mnemonic == "JSR" || ins.Mnemonic == "JSL" || ins.Mnemonic == "PL"+reg ||
					(strings.HasPrefix(ins.Mnemonic, "T") && strings.HasSuffix(ins.Mnemonic, reg)) ||
					ins.Mnemonic == "IN"+reg || ins.Mnemonic == "DE"+reg {
					break
				}
				if reg == "A" {
					switch ins.Mnemonic {
					case "ADC", "SBC", "AND", "ORA", "EOR", "XBA", "TDC", "TSC", "INC", "DEC", "ASL", "LSR", "ROL", "ROR":
						goto nextStore
					}
				}
				if ins.Mnemonic == "REP" || ins.Mnemonic == "SEP" || ins.Mnemonic == "PLP" || ins.Mnemonic == "XCE" {
					break
				}
			}
		nextStore:
			if !literal {
				unknownWrites[uint16(i.Operand)] = true
			}
		}
		// A locally assigned dynamic scratch/return pointer is not a global
		// handler field. Do not mix unrelated literal writers into its census.
		for _, r := range localReads {
			// An explicit base added to a pointer can address individual
			// instructions in an unrolled shift suffix. Keep this structural
			// query separate from global handler-field literal matching.
			if !r.long {
				for base := range bases[r.slot] {
					b, err := image.Slice(r.bank, base, 1)
					if err != nil || (b[0] != 0x4a && b[0] != 0x0a && b[0] != 0x2a && b[0] != 0x6a) {
						continue
					}
					count := uint32(0)
					for uint32(base)+count < 0x10000 && count < 64 {
						q, e := image.Slice(r.bank, uint16(uint32(base)+count), 1)
						if e != nil || q[0] != b[0] {
							break
						}
						count++
					}
					if count >= 3 && count < 64 {
						for n := uint32(0); n <= count; n++ {
							structuredTargets[Address24(r.bank, uint16(uint32(base)+n))] = true
						}
					}
				}
			}
			if !unknownWrites[r.slot] && (!r.long || !unknownWrites[r.slot+2]) {
				reads[r] = true
			}
		}
	}
	// Small setter helpers frequently receive an address in A and its bank
	// in X/Y. Match only direct calls/jumps, mapper-identical entry bytes,
	// unchanged incoming registers and literal caller arguments. These still
	// supply an open inventory: they are not a closed field-value summary.
	for _, g := range graphs {
		pred := map[DecodeKey][]DecodeKey{}
		for _, d := range g.Instructions {
			for _, s := range d.Successors {
				pred[s] = append(pred[s], d.Key)
			}
		}
		for _, d := range g.Instructions {
			i := d.Instruction
			var target uint32
			switch i.Opcode {
			case 0x20, 0x4c:
				target = (i.Address & 0xff0000) | (i.Operand & 0xffff)
			case 0x22, 0x5c:
				target = i.Operand & 0xffffff
			default:
				continue
			}
			offset, err := image.Offset(byte(target>>16), uint16(target))
			if err != nil {
				continue
			}
			arguments := map[parameter]uint16{}
			for p := range parameters[offset] {
				if p.m != d.Key.M || p.x != d.Key.X {
					continue
				}
				key := d.Key
				for n := 0; n < 32; n++ {
					prior := pred[key]
					if len(prior) != 1 {
						break
					}
					key = prior[0]
					ins := g.Instructions[key].Instruction
					if ins.Mnemonic == "LD"+p.reg {
						if ins.Mode == cpu65816.IMM {
							v := uint16(ins.Operand)
							if p.width == 1 {
								v &= 255
							}
							arguments[p] = v
						}
						break
					}
					// Whitelist transparent instructions. Any instruction not
					// listed must not silently carry a literal across a clobber.
					switch ins.Mnemonic {
					case "LDA", "LDX", "LDY", "STA", "STX", "STY", "STZ", "PHK", "PHB", "PHA", "PHX", "PHY", "PLB", "NOP", "CLC", "SEC", "CLD", "SED", "BRA", "BRL":
					default:
						n = 32
					}
				}
			}
			// Keep low/bank arguments correlated to this one call. Mixing
			// unrelated setters into a global Cartesian product decodes data.
			for low, word := range arguments {
				if low.width != 2 {
					continue
				}
				for r := range reads {
					if r.slot != low.slot {
						continue
					}
					if !r.long {
						structuredTargets[Address24(r.bank, word)] = true
						continue
					}
					for high, bank := range arguments {
						if high.slot == low.slot+2 && bank <= 255 {
							structuredTargets[Address24(byte(bank), word)] = true
						}
					}
				}
			}
		}
	}
	targets := structuredTargets
	// Some PEI dispatchers initialize a page/base once, then replace only its
	// low byte. A literal which lands on a dense JMP island supplies cold
	// candidates even when handler-to-handler edges split the initializer
	// from the later PEI. This is structural evidence, never a closed bound.
	for slot, banks := range peiSlots {
		if len(writes[slot]) > 64 {
			continue
		}
		for literal := range writes[slot] {
			if literal.width != 2 {
				continue
			}
			for bank := range banks {
				for _, delta := range []uint32{0, 1} {
					for _, stride := range []uint32{3, 4} {
						var island []uint32
						for pc := uint32(literal.value) + delta; pc+3 <= 0x10000 && len(island) < 64; pc += stride {
							b, err := image.Slice(bank, uint16(pc), 3)
							if err != nil || b[0] != 0x4c {
								break
							}
							target := uint16(b[1]) | uint16(b[2])<<8
							if !image.IsROM(bank, target) {
								break
							}
							island = append(island, Address24(bank, uint16(pc)))
						}
						if len(island) >= 3 {
							for _, pc := range island {
								targets[pc] = true
							}
						}
					}
				}
			}
		}
	}
	for r := range reads {
		if len(writes[r.slot]) > 64 {
			continue
		}
		for low := range writes[r.slot] {
			if low.width != 2 {
				continue
			}
			if !r.long {
				targets[Address24(r.bank, low.value)] = true
				continue
			}
			if len(writes[r.slot+2]) > 64 {
				continue
			}
			for high := range writes[r.slot+2] {
				if high.value > 0xff {
					continue
				}
				targets[Address24(byte(high.value), low.value)] = true
			}
		}
	}
	result := make([]uint32, 0, len(targets))
	for target := range targets {
		result = append(result, target)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
}

// Resolve a value immediately before a merged store through a bounded native
// predecessor DAG. This is an open address inventory, not a reachability,
// must-alias or closed dispatch proof. One unknown/cyclic/over-budget arm
// rejects the query as a whole, including any literals found before it.
func storedJoinedLiterals(g *Graph, pred map[DecodeKey][]DecodeKey, at DecodeKey, reg string, width int) ([]uint16, bool) {
	const workLimit, depthLimit, valueLimit = 128, 32, 64
	work := workLimit
	active := map[DecodeKey]bool{}
	values := map[uint16]bool{}
	var visit func(DecodeKey, int) bool
	visit = func(key DecodeKey, depth int) bool {
		if depth == depthLimit || work == 0 || active[key] || key == g.Entry {
			return false
		}
		active[key] = true
		defer delete(active, key)
		previous := pred[key]
		if len(previous) == 0 {
			return false
		}
		for _, p := range previous {
			if work == 0 {
				return false
			}
			work--
			d := g.Instructions[p]
			if d == nil || d.Instruction == nil {
				return false
			}
			i := d.Instruction
			w := 2 - int(p.X)
			if reg == "A" {
				w = 2 - int(p.M)
			}
			if w != width || i.DispatchKind != "" || len(i.DispatchEntries) != 0 {
				return false
			}
			if i.Mnemonic == "LD"+reg {
				if i.Mode != cpu65816.IMM {
					return false
				}
				v := uint16(i.Operand)
				if width == 1 {
					v &= 255
				}
				values[v] = true
				if len(values) > valueLimit {
					return false
				}
				continue
			}
			// Explicitly transparent operations only. Pulls, arithmetic,
			// status/width changes and unknown callees do not preserve a value.
			switch i.Mnemonic {
			case "LDA", "LDX", "LDY", "STA", "STX", "STY", "STZ", "PHK", "PHB", "PHA", "PHX", "PHY", "PLB", "NOP", "CLC", "SEC", "CLD", "SED", "BRA", "BRL", "BEQ", "BNE", "BCC", "BCS", "BMI", "BPL", "BVC", "BVS", "CMP", "CPX", "CPY", "BIT":
			case "JMP":
				if i.Mode != cpu65816.ABS {
					return false
				}
			default:
				return false
			}
			if !visit(p, depth+1) {
				return false
			}
		}
		return true
	}
	if !visit(at, 0) || len(values) == 0 {
		return nil, false
	}
	result := make([]uint16, 0, len(values))
	for value := range values {
		result = append(result, value)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result, true
}
