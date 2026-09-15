package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// LiteralLongPointerTargets reconstructs locally correlated byte lanes of
// pointers consumed by JML [abs]. Word writes may overlap in either order;
// reverse traversal implements last-write-wins, without mixing unrelated
// writers or selecting one arm of a value join. This remains an open inventory:
// DP/DB aliasing, slot lifetime and execution order relative to the consumer
// are not proved. Only caller-approved graphs and low-WRAM slot spellings
// participate; actual native stores, target reads and live M/X remain intact.
func LiteralLongPointerTargets(image rom.Image, graphs []*Graph, regions []DataRegion) []uint32 {
	byWrite := map[uint16]map[uint16]bool{}
	for _, g := range graphs {
		for _, d := range g.Instructions {
			if i := d.Instruction; i.Opcode == 0xdc && i.Operand < 0x1ffe {
				base := uint16(i.Operand)
				for delta := -1; delta <= 2; delta++ {
					at := uint16(int(base) + delta)
					if byWrite[at] == nil {
						byWrite[at] = map[uint16]bool{}
					}
					byWrite[at][base] = true
				}
			}
		}
	}
	storeWidth := func(d *DecodedInstruction) (string, int) {
		switch d.Instruction.Mnemonic {
		case "STA":
			return "A", 2 - int(d.Key.M)
		case "STX":
			return "X", 2 - int(d.Key.X)
		case "STY":
			return "Y", 2 - int(d.Key.X)
		case "STZ":
			return "", 2 - int(d.Key.M)
		}
		return "", 0
	}
	targets := map[uint32]bool{}
	for _, g := range graphs {
		pred := forwardedFieldPredecessors(g)
		for _, end := range g.Instructions {
			i := end.Instruction
			_, width := storeWidth(end)
			if width == 0 || (i.Mode != cpu65816.DP && i.Mode != cpu65816.ABS) || i.Operand >= 0x1fff {
				continue
			}
			for base := range byWrite[uint16(i.Operand)] {
				var bytes [3]byte
				mask := uint8(0)
				at := end
				for steps := 0; steps < 32 && at != nil; steps++ {
					ins := at.Instruction
					reg, width := storeWidth(at)
					if width != 0 {
						// Do not walk across indirect/indexed writes or changes of
						// DP/absolute spelling: they may alias unfinished lanes.
						if ins.Mode != i.Mode || ins.Operand >= 0x1fff {
							break
						}
						var needs uint8
						for n := 0; n < width; n++ {
							lane := int(uint16(ins.Operand)) + n - int(base)
							if lane >= 0 && lane < 3 && mask&(1<<lane) == 0 {
								needs |= 1 << lane
							}
						}
						if needs != 0 {
							value := uint16(0)
							if reg != "" {
								values, ok := storedJoinedLiterals(g, pred, at.Key, reg, width)
								if !ok || len(values) != 1 {
									break
								}
								value = values[0]
							}
							for n := 0; n < width; n++ {
								lane := int(uint16(ins.Operand)) + n - int(base)
								if lane >= 0 && lane < 3 && needs&(1<<lane) != 0 {
									bytes[lane] = byte(value >> (8 * n))
								}
							}
							mask |= needs
						}
					} else {
						switch ins.Mnemonic {
						case "LDA", "LDX", "LDY", "NOP", "REP", "SEP", "CLC", "SEC", "CLD", "SED":
						default:
							goto nextPointer
						}
					}
					if mask == 7 {
						pc := uint16(bytes[0]) | uint16(bytes[1])<<8
						if image.IsROM(bytes[2], pc) && !targetIsPadding(image, bytes[2], pc) && !inDataRegion(regions, bytes[2], pc) {
							targets[Address24(bytes[2], pc)] = true
						}
						break
					}
					at = forwardedFieldPrevious(g, pred, at.Key)
				}
			nextPointer:
			}
		}
	}
	var result []uint32
	for target := range targets {
		result = append(result, target)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
}
