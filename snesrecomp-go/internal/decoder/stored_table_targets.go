package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// StoredTableDispatchTargets joins a ROM record's word field, stored into an
// absolute low-WRAM slot, with decoded JMP (abs) consumers of that slot. The
// writer must establish DB=PB and a power-of-two record index on one unique
// native M0X0 path. This inventories an open structural prefix, not the index
// domain, writer order, slot lifetime or a closed control-flow edge set.
// Callers must preserve authored/HLE/data admission barriers and live dispatch.
func StoredTableDispatchTargets(image rom.Image, graphs []*Graph, regions []DataRegion) []uint32 {
	consumers := map[uint16]map[byte]bool{}
	for _, g := range graphs {
		for _, d := range g.Instructions {
			i := d.Instruction
			if i.Opcode == 0x6c && i.Operand < 0x1fff {
				slot := uint16(i.Operand)
				if consumers[slot] == nil {
					consumers[slot] = map[byte]bool{}
				}
				consumers[slot][byte(i.Address>>16)] = true
			}
		}
	}
	tables := map[openRecordTable]bool{}
	for _, g := range graphs {
		pred := forwardedFieldPredecessors(g)
		previous := func(key DecodeKey) *DecodedInstruction {
			d := forwardedFieldPrevious(g, pred, key)
			if d == nil || d.Key.M != 0 || d.Key.X != 0 {
				return nil
			}
			return d
		}
		for _, d := range g.Instructions {
			i := d.Instruction
			if d.Key.M != 0 || d.Key.X != 0 || i.Mnemonic != "STA" || i.Mode != cpu65816.ABS || len(consumers[uint16(i.Operand)]) == 0 {
				continue
			}
			// PHK/PLB below establishes the writer's low-WRAM mirror only in
			// system banks, not in a full-ROM bank such as HiROM $C0.
			writerBank := byte(i.Address >> 16)
			if writerBank&0x7f >= 0x40 {
				continue
			}
			p := previous(d.Key)
			for n := 0; n < 4 && p != nil && p.Instruction.Mnemonic == "STA" && p.Instruction.Mode == cpu65816.ABS; n++ {
				p = previous(p.Key)
			}
			if p == nil || p.Instruction.Mnemonic != "LDA" || (p.Instruction.Mode != cpu65816.ABSX && p.Instruction.Mode != cpu65816.ABSY) {
				continue
			}
			base, mode := uint16(p.Instruction.Operand), p.Instruction.Mode
			transfer, metadataLoad := "TAX", "LDY"
			if mode == cpu65816.ABSY {
				transfer, metadataLoad = "TAY", "LDX"
			}
			p = previous(p.Key)
			// A neighboring field may initialize the other index register.
			// It cannot alter the effective index or accumulator's provenance.
			for n := 0; n < 4 && p != nil && p.Instruction.Mnemonic == metadataLoad && p.Instruction.Mode == mode; n++ {
				p = previous(p.Key)
			}
			if p == nil || p.Instruction.Mnemonic != transfer {
				continue
			}
			stride := uint32(1)
			for p = previous(p.Key); p != nil && p.Instruction.Mnemonic == "ASL" && p.Instruction.Mode == cpu65816.ACC; p = previous(p.Key) {
				stride *= 2
				if stride > 16 {
					break
				}
			}
			if stride < 4 || stride > 16 || p == nil || p.Instruction.Mnemonic != "PLB" {
				continue
			}
			p = previous(p.Key)
			if p == nil || p.Instruction.Mnemonic != "PHK" {
				continue
			}
			source, err := image.Offset(writerBank, base)
			if err != nil {
				continue
			}
			for bank := range consumers[uint16(i.Operand)] {
				// A short pointer belongs to the consumer's live program bank.
				// Limit this query to mapper-equivalent table/code banks; do not
				// substitute the writer's bank for an unrelated consumer bank.
				if offset, err := image.Offset(bank, base); err == nil && offset == source {
					tables[openRecordTable{bank, base, stride}] = true
				}
			}
		}
	}
	targets := map[uint32]bool{}
	for table := range tables {
		for _, target := range openRecordTargets(image, table, regions) {
			targets[target] = true
		}
	}
	var result []uint32
	for target := range targets {
		result = append(result, target)
	}
	sort.Slice(result, func(i, j int) bool { return result[i] < result[j] })
	return result
}
