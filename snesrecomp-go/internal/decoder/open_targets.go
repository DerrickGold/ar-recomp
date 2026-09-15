package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type openRecordTable struct {
	bank   byte
	base   uint16
	stride uint32
}

// OpenDispatchColdTargets extends address inventories, not control-flow proofs.
// A shifted index can select a word field in records wider than a pointer;
// address-taken unrolled shifts can expose individual one-byte suffix entries.
// Runtime dispatch must retain the actual pointer, live widths and hard misses.
func OpenDispatchColdTargets(image rom.Image, graphs []*Graph, regions []DataRegion) []uint32 {
	tables := map[openRecordTable]bool{}
	shifts := map[uint32]bool{}
	for _, graph := range graphs {
		pred := map[DecodeKey][]DecodeKey{}
		for _, d := range graph.Instructions {
			for _, s := range d.Successors {
				pred[s] = append(pred[s], d.Key)
			}
		}
		previous := func(key DecodeKey) *DecodedInstruction {
			if len(pred[key]) != 1 {
				return nil
			}
			return graph.Instructions[pred[key][0]]
		}
		for _, d := range graph.Instructions {
			i := d.Instruction
			if i.DispatchOpen {
				for _, address := range i.DispatchEntries {
					if address != 0 {
						shifts[address] = true
					}
				}
			}
			// An unresolved word table can have a null hole before enough
			// contiguous targets exist for ordinary dispatch recovery. Do not
			// extend an authored/resolved closed dispatch contract here.
			if !i.DispatchOpen && (len(i.DispatchEntries) != 0 || i.DispatchKind != "" || i.DispatchReturn != nil) {
				continue
			}
			if i.Opcode != 0x7c || d.Key.M != 0 || d.Key.X != 0 {
				continue
			}
			p := previous(d.Key)
			if p == nil || p.Instruction.Mnemonic != "TAX" || p.Key.M != 0 || p.Key.X != 0 {
				continue
			}
			stride := uint32(1)
			for p = previous(p.Key); p != nil && p.Key.M == 0 && p.Instruction.Mnemonic == "ASL" && p.Instruction.Mode == cpu65816.ACC; p = previous(p.Key) {
				stride *= 2
				if stride > 16 {
					break
				}
			}
			// Do not infer multiply-by-three through ADC: its carry input and
			// decimal flag need independent evidence. Consecutive ASLs suffice.
			if stride == 2 && p != nil {
				switch p.Instruction.Mnemonic {
				case "LDA", "LDX", "LDY", "AND", "ORA", "EOR":
				default:
					continue
				}
			}
			if stride >= 2 && stride <= 16 {
				tables[openRecordTable{byte(i.Address >> 16), uint16(i.Operand), stride}] = true
			}
		}
	}
	targets := map[uint32]bool{}
	for table := range tables {
		for _, address := range openRecordTargets(image, table, regions) {
			targets[address] = true
		}
	}
	for address := range shifts {
		bank, base := byte(address>>16), uint16(address)
		first, err := image.Slice(bank, base, 1)
		if err != nil || (first[0] != 0x0a && first[0] != 0x4a && first[0] != 0x2a && first[0] != 0x6a) {
			continue
		}
		count := uint32(0)
		closed := false
		for uint32(base)+count < 0x10000 && count < 64 {
			pc := uint16(uint32(base) + count)
			bytes, err := image.Slice(bank, pc, 1)
			if err != nil || inDataRegion(regions, bank, pc) {
				break
			}
			if bytes[0] != first[0] {
				closed = bytes[0] == 0x60
				break
			}
			count++
		}
		if !closed || count < 3 {
			continue
		}
		for offset := uint32(0); offset <= count; offset++ {
			targets[Address24(bank, uint16(uint32(base)+offset))] = true
		}
	}
	result := make([]uint32, 0, len(targets))
	for address := range targets {
		result = append(result, address)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
}

// Structural prefix only: the index domain and record count remain unknown.
func openRecordTargets(image rom.Image, table openRecordTable, regions []DataRegion) []uint32 {
	var targets []uint32
	end := uint32(0x10000)
	for index := uint32(0); index < 256; index++ {
		address := uint32(table.base) + index*table.stride
		if address+2 > end {
			break
		}
		bytes, err := image.Slice(table.bank, uint16(address), 2)
		if err != nil {
			break
		}
		target := uint16(bytes[0]) | uint16(bytes[1])<<8
		// Two independently mapped words and a forward pointed-code boundary
		// delimit this cold nullable prefix. A zero is neither an executable
		// edge nor proof that the table ends. Never probe past the boundary
		// or use this to close the dynamic index range.
		if table.stride == 2 && target == 0 && len(targets) >= 2 && end < 0x10000 {
			continue
		}
		if !image.IsROM(table.bank, target) || targetIsPadding(image, table.bank, target) || inDataRegion(regions, table.bank, target) ||
			(uint32(target) >= uint32(table.base) && uint32(target) < address+2) {
			break
		}
		targets = append(targets, Address24(table.bank, target))
		if uint32(target) >= uint32(table.base) && uint32(target) < end {
			end = uint32(target)
		}
	}
	return targets
}
