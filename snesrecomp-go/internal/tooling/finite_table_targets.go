package tooling

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// FiniteStoredTableTargets reuses initializer bit/value and bank provenance to
// inventory ROM words stored into slots used by decoded JMP (abs) instructions.
// Unlike a structural table-prefix probe, it reads only the finite local index
// superset (at most 256 values), including sparse masks and interleaved records.
// DP/DB aliasing, reaching definitions and slot lifetime are NOT proven. These
// are conditional cold entries, never closed edge sets or width/entry facts.
// The caller must retain native writes, live target/MX dispatch, and hard misses.
// eligible must exclude authored body/width replacements, as for the other
// cold queries; all configs also impose mapper-aware HLE/data barriers here.
func FiniteStoredTableTargets(image rom.Image, configs map[byte]*config.Config, graphs, eligible []*decoder.Graph) []uint32 {
	native, a := coldNativeGraphs(image, configs, graphs, eligible)
	consumers := map[uint16]map[byte]bool{}
	for _, g := range native {
		for _, d := range g.Instructions {
			i := d.Instruction
			if i.Opcode == 0x6c && i.Operand < 0x1fff {
				slot := uint16(i.Operand)
				if consumers[slot] == nil {
					consumers[slot] = map[byte]bool{}
				}
				consumers[slot][byte(d.Key.PC>>16)] = true
			}
		}
	}
	seen := map[uint32]bool{}
	for _, g := range native {
		w := shadowPointerWalk{graph: g}
		for key, d := range g.Instructions {
			i := d.Instruction
			if key.M != 0 || key.X != 0 || i.Mnemonic != "STA" ||
				(i.Mode != cpu65816.DP && i.Mode != cpu65816.ABS) || len(consumers[uint16(i.Operand)]) == 0 {
				continue
			}
			if w.preds == nil {
				w.preds = shadowPredecessors(g)
			}
			if i.Mode == cpu65816.ABS {
				// An absolute store in a full-ROM bank is not this WRAM slot.
				bank, ok := shadowStreamConstantBank(w.initializerBank(key))
				if !ok || bank&0x7f >= 0x40 {
					continue
				}
			}
			load := w.previous(key)
			if load == nil || load.Instruction.Mnemonic != "LDA" {
				continue
			}
			read := w.initializerRead(load.Key, false)
			if read == nil || load.Instruction.Mode == cpu65816.INDIRY {
				continue
			}
			// Bank/stack operations may intervene after TAX/TAY, and reads
			// of other record fields may overwrite A without changing X/Y.
			index := w.indexExpression(load.Key, read.IndexRegister, true)
			bank, ok := shadowStreamConstantBank(read.DataBank)
			if !ok || len(index.DomainValues) == 0 {
				continue
			}
			base := uint32(uint16(read.Operand))
			start, ok := a.offset(uint32(bank)<<16 | base)
			if !ok {
				continue
			}
			for targetBank := range consumers[uint16(i.Operand)] {
				end := start + 0x10000 - int(base)
				for _, index := range index.DomainValues {
					address := base + uint32(index)
					// A mask is not a record count. Stop where pointed code
					// starts, rather than parsing its bytes as further records.
					// This boundary is physical: a different target bank must
					// not accidentally truncate the source ROM table.
					if address >= 0xffff || start+int(index)+2 > end {
						break
					}
					word, ok := shadowStreamROMWord(image, bank, address)
					if !ok || word == 0 || word == 0xffff {
						continue
					}
					// The word is interpreted in the consumer's PB, not DB.
					target := uint32(targetBank)<<16 | uint32(word)
					off, ok := a.offset(target)
					if !ok {
						continue
					}
					ins, err := decodeShadowInstruction(image, targetBank, word, 0, 0)
					if err != nil || ins.Opcode == 0 || ins.Opcode == 0xff {
						continue
					}
					if off >= start && off < end {
						end = off
					}
					if start+int(index)+2 > end {
						break
					}
					valid := true
					for n := 0; n < int(ins.Length); n++ {
						p, ok := a.offset(target + uint32(n))
						if !ok || p != off+n || a.blocked[p] != "" {
							valid = false
						}
					}
					if valid {
						seen[target] = true
					}
				}
			}
		}
	}
	var targets []uint32
	for target := range seen {
		targets = append(targets, target)
	}
	sort.Slice(targets, func(i, j int) bool { return targets[i] < targets[j] })
	return targets
}
