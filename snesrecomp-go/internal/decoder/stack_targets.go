package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
)

// PushedReturnTargets inventories constant address words still on the stack
// at an RTS. These are open address-taken roots, NOT a closed edge proof: an
// older word may be a later continuation, or ordinary data. Runtime RTS keeps
// consuming the real stack and dispatching on live M/X. No call, return, or
// memory write is replaced by this inventory.
//
// Calls, arbitrary stack movement and possible stack aliases kill evidence.
// Bounded states converge through small initialization loops without guessing
// a canonical register width. Overflow abandons the loop-dependent query, not
// its checks or the independent immediate PEA/PER + indirect-JMP inventory.
func PushedReturnTargets(g *Graph) []uint32 {
	// A large unrelated loop must not erase a trivially address-taken software
	// call continuation elsewhere in the region. Enumerate every immediate
	// envelope first, without using a traversal budget or retaining a sampled
	// prefix. Short indirect jumps preserve PB; a long jump needs a separate
	// return-bank/RTL model and is deliberately not part of this fast query.
	immediate := make(map[uint32]bool)
	for _, key := range g.Order {
		d := g.Instructions[key]
		if d == nil || d.Instruction == nil {
			continue
		}
		i := d.Instruction
		if i.Mnemonic != "PEA" && i.Mnemonic != "PER" {
			continue
		}
		fallthroughPC := i.Address&0xff0000 | uint32(uint16(i.Address+uint32(i.Length)))
		for _, key := range d.Successors {
			next := g.Instructions[key]
			if next != nil && next.Instruction != nil && key.PC == fallthroughPC &&
				(next.Instruction.Opcode == 0x6c || next.Instruction.Opcode == 0x7c) {
				immediate[i.Address&0xff0000|uint32(uint16(i.Operand)+1)] = true
			}
		}
	}
	type lane struct {
		value byte
		known bool
	}
	type state struct {
		key               DecodeKey
		stack             [12]lane // newest byte first, as read by a native pull
		depth             uint8
		x, y              uint16
		kx, ky            bool
		flags, knownFlags byte // N=4,Z=2,C=1; unknown flags never prune a branch
	}
	var queue []state
	// Start at decoded literal pushes: no predecessor register knowledge is
	// required, and unrelated pre-push loops cannot exhaust the query budget.
	for _, key := range g.Order {
		i := g.Instructions[key].Instruction
		if i != nil && (i.Mnemonic == "PEA" || i.Mnemonic == "PER") {
			queue = append(queue, state{key: key})
		}
	}
	seen := make(map[state]bool)
	targets := make(map[uint32]bool)
	for pc := range immediate {
		targets[pc] = true
	}
	for len(queue) > 0 {
		s := queue[len(queue)-1]
		queue = queue[:len(queue)-1]
		if seen[s] {
			continue
		}
		seen[s] = true
		if len(seen) > 8192 {
			return sortedPushedTargets(immediate)
		}
		d := g.Instructions[s.key]
		if d == nil || d.Instruction == nil {
			continue
		}
		i := d.Instruction
		// Bound branches only from exact tracked index arithmetic. This is
		// particularly useful for register-initialization loops before RTS.
		flags, knownFlags := s.flags, s.knownFlags
		setNZ := func(v uint16, known bool) {
			knownFlags &^= 6
			if !known {
				return
			}
			knownFlags |= 6
			flags &^= 6
			mask := uint16(0xffff)
			sign := uint16(0x8000)
			if s.key.X != 0 {
				mask, sign = 0xff, 0x80
			}
			if v&mask == 0 {
				flags |= 2
			}
			if v&sign != 0 {
				flags |= 4
			}
		}
		switch i.Mnemonic {
		case "CPX", "CPY":
			v, k := s.x, s.kx
			if i.Mnemonic == "CPY" {
				v, k = s.y, s.ky
			}
			k = k && i.Mode == cpu65816.IMM
			setNZ(v-uint16(i.Operand), k)
			knownFlags &^= 1
			if k {
				knownFlags |= 1
				flags &^= 1
				if v >= uint16(i.Operand) {
					flags |= 1
				}
			}
		case "LDX", "LDY":
			setNZ(uint16(i.Operand), i.Mode == cpu65816.IMM)
		case "INX":
			setNZ(s.x+1, s.kx)
		case "DEX":
			setNZ(s.x-1, s.kx)
		case "INY":
			setNZ(s.y+1, s.ky)
		case "DEY":
			setNZ(s.y-1, s.ky)
		case "STA", "STX", "STY", "STZ", "PEA", "PER", "PEI", "PHA", "PHX", "PHY", "PHB", "PHK", "PHD", "PHP", "REP", "SEP", "NOP", "BRA", "BRL", "JMP", "BNE", "BEQ", "BCC", "BCS", "BMI", "BPL", "BVC", "BVS", "TXS":
		default:
			knownFlags = 0
		}
		push := func(v uint16, width int, known bool) {
			if int(s.depth)+width > len(s.stack) {
				s.stack = [12]lane{}
				s.depth = 0
			}
			copy(s.stack[width:], s.stack[:len(s.stack)-width])
			for n := 0; n < width; n++ {
				s.stack[n] = lane{byte(v >> uint(n*8)), known}
			}
			s.depth += uint8(width)
		}
		pull := func(width int) {
			if int(s.depth) < width {
				s.depth = 0
				s.stack = [12]lane{}
				return
			}
			copy(s.stack[:], s.stack[width:])
			clear(s.stack[len(s.stack)-width:])
			s.depth -= uint8(width)
		}
		kill := func() { s.depth = 0; s.stack = [12]lane{} }
		switch i.Mnemonic {
		case "PEA":
			push(uint16(i.Operand), 2, true)
		case "PER":
			push(uint16(i.Operand), 2, true)
		case "PEI":
			push(0, 2, false)
		case "PHA":
			push(0, 2-int(s.key.M), false)
		case "PHX":
			push(s.x, 2-int(s.key.X), s.kx)
		case "PHY":
			push(s.y, 2-int(s.key.X), s.ky)
		case "PHP", "PHB":
			push(0, 1, false)
		case "PHK":
			push(uint16(i.Address>>16), 1, true)
		case "PHD":
			push(0, 2, false)
		case "PLA":
			pull(2 - int(s.key.M))
		case "PLX":
			pull(2 - int(s.key.X))
			s.kx = false
		case "PLY":
			pull(2 - int(s.key.X))
			s.ky = false
		case "PLP", "PLB":
			pull(1)
		case "PLD":
			pull(2)
		case "TXS", "TCS", "XCE", "JSR", "JSL", "BRK", "COP":
			kill()
			s.kx, s.ky = false, false
		case "RTS":
			// Only recognize complete constant-word chains with a known top.
			for n := 0; n+1 < int(s.depth); n += 2 {
				if !s.stack[n].known || !s.stack[n+1].known {
					break
				}
				target := uint16(s.stack[n].value) | uint16(s.stack[n+1].value)<<8
				targets[(i.Address&0xff0000)|uint32(target+1)] = true
			}
			continue
		case "JMP", "JML":
			// PEA continuation-1 / JMP (handler) is a native call envelope.
			// Its address-taken continuation may not be a direct CFG successor.
			if i.Opcode == 0x6c || i.Opcode == 0x7c || i.Opcode == 0xdc {
				for n := 0; n+1 < int(s.depth); n += 2 {
					if !s.stack[n].known || !s.stack[n+1].known {
						break
					}
					target := uint16(s.stack[n].value) | uint16(s.stack[n+1].value)<<8
					targets[(i.Address&0xff0000)|uint32(target+1)] = true
				}
			}
		case "RTL", "RTI", "STP", "WAI":
			continue
		case "LDX":
			s.kx = i.Mode == cpu65816.IMM
			s.x = uint16(i.Operand)
		case "LDY":
			s.ky = i.Mode == cpu65816.IMM
			s.y = uint16(i.Operand)
		case "INX":
			s.x++
		case "DEX":
			s.x--
		case "INY":
			s.y++
		case "DEY":
			s.y--
		case "TAX", "TSX", "TYX":
			s.kx = false
		case "TAY", "TXY":
			s.ky = false
		case "MVN", "MVP":
			kill()
			s.kx, s.ky = false, false
		case "STA", "STX", "STY", "STZ", "INC", "DEC", "ASL", "LSR", "ROL", "ROR", "TSB", "TRB":
			// Absolute hardware addresses cannot alias a WRAM stack. Indexed
			// accesses require a tracked finite index and no 16-bit wrapping.
			if i.Mode == cpu65816.ACC {
				break
			}
			address, known := uint32(i.Operand), i.Mode == cpu65816.ABS
			if i.Mode == cpu65816.ABSX && s.kx {
				address += uint32(s.x)
				known = true
			}
			if i.Mode == cpu65816.ABSY && s.ky {
				address += uint32(s.y)
				known = true
			}
			if !known || address < 0x2000 || address >= 0x6000 {
				kill()
			}
		}
		post := PostState(i, s.key)
		s.flags, s.knownFlags = flags, knownFlags
		if post.X != 0 {
			s.x &= 255
			s.y &= 255
		}
		if !s.kx {
			s.x = 0
		}
		if !s.ky {
			s.y = 0
		}
		for _, next := range d.Successors {
			bit, want := byte(0), false
			switch i.Mnemonic {
			case "BNE":
				bit, want = 2, false
			case "BEQ":
				bit, want = 2, true
			case "BCC":
				bit, want = 1, false
			case "BCS":
				bit, want = 1, true
			case "BPL":
				bit, want = 4, false
			case "BMI":
				bit, want = 4, true
			}
			if bit != 0 && knownFlags&bit != 0 {
				taken := (flags&bit != 0) == want
				if (uint16(next.PC) == uint16(i.Operand)) != taken {
					continue
				}
			}
			q := s
			q.key = next
			queue = append(queue, q)
		}
	}
	return sortedPushedTargets(targets)
}

func sortedPushedTargets(targets map[uint32]bool) []uint32 {
	result := make([]uint32, 0, len(targets))
	for target := range targets {
		result = append(result, target)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
}
