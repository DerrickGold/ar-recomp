package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// StackedFieldTableTargets joins a decoded RTL's stacked object fields with
// paired ROM-table stores. Literal indices can be forwarded through bounded
// direct-call wrappers. Two distinct literals may anchor a small, uniformly
// strided prefix: every record must map to ROM and the inferred record width
// must hold the three consumed bytes. This bounded structural inventory is
// not a proof that intervening indices execute, nor a closed table bound.
// Fields are syntactic, not must-alias identities: all results remain open
// cold-entry candidates, never closed targets or M/X facts.
func StackedFieldTableTargets(image rom.Image, graphs []*Graph) []uint32 {
	return stackedFieldTableTargets(image, graphs, nil, nil)
}

// StackedFieldTableTargetsWithOpenPrefixes additionally inventories structural
// records beyond literal anchors in caller-approved, non-overridden graphs.
// The literal anchors establish a plausible record stride, not selector bounds.
// These extra targets remain speculative cold roots with live runtime dispatch.
func StackedFieldTableTargetsWithOpenPrefixes(image rom.Image, graphs, eligible []*Graph, regions []DataRegion) []uint32 {
	allowed := make(map[*Graph]bool, len(eligible))
	for _, graph := range eligible {
		allowed[graph] = true
	}
	return stackedFieldTableTargets(image, graphs, allowed, regions)
}

func stackedFieldTableTargets(image rom.Image, graphs []*Graph, openPrefixes map[*Graph]bool, regions []DataRegion) []uint32 {
	type walk struct {
		g    *Graph
		pred map[DecodeKey][]DecodeKey
	}
	var walks []*walk
	for _, g := range graphs {
		w := &walk{g: g, pred: map[DecodeKey][]DecodeKey{}}
		for _, d := range g.Instructions {
			for _, s := range d.Successors {
				w.pred[s] = append(w.pred[s], d.Key)
			}
		}
		walks = append(walks, w)
	}
	previous := func(w *walk, k DecodeKey) *DecodedInstruction {
		p := w.pred[k]
		if len(p) != 1 {
			return nil
		}
		return w.g.Instructions[p[0]]
	}
	// Offsets of decoded routine entries, built only when a table outgrows the
	// open window and its end must be proven.
	var entries map[int]bool
	decodedEntry := func(offset int) bool {
		if entries == nil {
			entries = map[int]bool{}
			for _, w := range walks {
				if off, err := image.Offset(byte(w.g.Entry.PC>>16), uint16(w.g.Entry.PC)); err == nil {
					entries[off] = true
				}
			}
		}
		return entries[offset]
	}
	// The unique backwards path may carry a register only across explicitly
	// transparent operations. Width truncation, arithmetic and calls stop it.
	type origin struct {
		load     *DecodedInstruction
		incoming string
	}
	registerOrigin := func(w *walk, k DecodeKey, reg string) origin {
		for n := 0; n < 64; n++ {
			if k == w.g.Entry {
				return origin{incoming: reg}
			}
			d := previous(w, k)
			if d == nil {
				return origin{}
			}
			k = d.Key
			i := d.Instruction
			if i.Mnemonic == "LD"+reg {
				return origin{load: d}
			}
			src, dst := "", ""
			switch i.Mnemonic {
			case "TAX":
				src, dst = "A", "X"
			case "TAY":
				src, dst = "A", "Y"
			case "TXA":
				src, dst = "X", "A"
			case "TYA":
				src, dst = "Y", "A"
			case "TXY":
				src, dst = "X", "Y"
			case "TYX":
				src, dst = "Y", "X"
			}
			if dst != "" {
				if dst == reg {
					if d.Key.X != 0 || ((src == "A" || dst == "A") && d.Key.M != 0) {
						return origin{}
					}
					reg = src
				}
				continue
			}
			switch i.Mnemonic {
			case "LDA", "LDX", "LDY", "STA", "STX", "STY", "STZ", "PHB", "PHK", "PHA", "PHX", "PHY", "PLB", "NOP", "CLC", "SEC", "CLD", "SED", "BRA", "BRL":
			case "SEP", "REP":
				if reg != "A" && i.Operand&0x10 != 0 {
					return origin{}
				}
			default:
				return origin{}
			}
		}
		return origin{}
	}
	type field struct {
		mode   cpu65816.AddressingMode
		offset uint16
	}
	type fields struct{ low, bank field }
	consumers := map[fields]bool{}
	openConsumers := map[fields]bool{}
	for _, w := range walks {
		for _, end := range w.g.Instructions {
			if end.Instruction.Mnemonic != "RTL" {
				continue
			}
			var lanes []field
			k, skip := end.Key, 0
			for n := 0; n < 48 && len(lanes) < 3; n++ {
				d := previous(w, k)
				if d == nil {
					break
				}
				k = d.Key
				i := d.Instruction
				if i.Mnemonic == "PLB" {
					skip++
					continue
				}
				if i.Mnemonic == "PHA" {
					o := registerOrigin(w, k, "A")
					if o.load == nil || o.load.Key.M != d.Key.M {
						break
					}
					load := o.load.Instruction
					if load.Mode != cpu65816.ABSY && load.Mode != cpu65816.ABSX {
						break
					}
					for b := 0; b < 2-int(d.Key.M) && len(lanes) < 3; b++ {
						if skip != 0 {
							skip--
							continue
						}
						lanes = append(lanes, field{load.Mode, uint16(load.Operand) + uint16(b)})
					}
					continue
				}
				switch i.Mnemonic {
				case "LDA", "LDX", "LDY", "STA", "STX", "STY", "STZ", "TAX", "TAY", "TXA", "TYA", "AND", "ORA", "EOR", "ASL", "LSR", "CMP", "CPX", "CPY", "NOP", "SEP", "REP":
				default:
					n = 48
				}
			}
			if len(lanes) == 3 && lanes[0].mode == lanes[1].mode && lanes[1].offset == lanes[0].offset+1 {
				pair := fields{lanes[0], lanes[2]}
				consumers[pair] = true
				if openPrefixes[w.g] {
					openConsumers[pair] = true
				}
			}
		}
	}
	if len(consumers) == 0 {
		return nil
	}
	type call struct {
		w   *walk
		key DecodeKey
	}
	type entry struct {
		offset int
		m, x   uint8
	}
	callers := map[entry][]call{}
	for _, w := range walks {
		for _, d := range w.g.Instructions {
			i := d.Instruction
			var target uint32
			switch i.Opcode {
			case 0x20, 0x4c:
				target = i.Address&0xff0000 | i.Operand&0xffff
			case 0x22, 0x5c:
				target = i.Operand & 0xffffff
			default:
				continue
			}
			if off, err := image.Offset(byte(target>>16), uint16(target)); err == nil {
				e := entry{off, d.Key.M, d.Key.X}
				callers[e] = append(callers[e], call{w, d.Key})
			}
		}
	}
	var values func(*walk, DecodeKey, string, int, map[uint16]bool)
	values = func(w *walk, k DecodeKey, reg string, depth int, out map[uint16]bool) {
		if depth > 4 || len(out) > 64 {
			return
		}
		o := registerOrigin(w, k, reg)
		if o.load != nil {
			if o.load.Instruction.Mode == cpu65816.IMM {
				v := uint16(o.load.Instruction.Operand)
				if (o.load.Instruction.Mnemonic == "LDA" && o.load.Key.M != 0) || (o.load.Instruction.Mnemonic != "LDA" && o.load.Key.X != 0) {
					return
				}
				out[v] = true
			}
			return
		}
		if o.incoming == "" {
			return
		}
		g := w.g.Entry
		off, err := image.Offset(byte(g.PC>>16), uint16(g.PC))
		if err != nil {
			return
		}
		for _, c := range callers[entry{off, g.M, g.X}] {
			values(c.w, c.key, o.incoming, depth+1, out)
		}
	}
	targets := map[uint32]bool{}
	for _, w := range walks {
		type write struct {
			field field
			load  *DecodedInstruction
			index string
			width int
		}
		var writes []write
		for _, d := range w.g.Instructions {
			i := d.Instruction
			if i.Mnemonic != "STA" || (i.Mode != cpu65816.ABSY && i.Mode != cpu65816.ABSX) {
				continue
			}
			o := registerOrigin(w, d.Key, "A")
			if o.load == nil || o.load.Key.M != d.Key.M || o.load.Key.X != 0 {
				continue
			}
			if o.load.Instruction.Mode != cpu65816.LONGX {
				continue
			}
			writes = append(writes, write{field{i.Mode, uint16(i.Operand)}, o.load, "X", 2 - int(d.Key.M)})
		}
		for _, low := range writes {
			if low.width != 2 {
				continue
			}
			for _, bank := range writes {
				if !consumers[fields{low.field, bank.field}] {
					continue
				}
				loAddr, bankAddr := low.load.Instruction.Operand, bank.load.Instruction.Operand
				if bankAddr != loAddr+2 {
					continue
				}
				// Both reads must use the same unmodified index producer.
				loOrigin := registerOrigin(w, low.load.Key, low.index)
				hiOrigin := registerOrigin(w, bank.load.Key, bank.index)
				if loOrigin != hiOrigin || (loOrigin.load == nil && loOrigin.incoming == "") {
					continue
				}
				indices := map[uint16]bool{}
				values(w, low.load.Key, low.index, 0, indices)
				if len(indices) > 64 {
					continue
				}
				readTarget := func(index uint16) (uint32, bool) {
					read := (loAddr + uint32(index)) & 0xffffff
					b, err := image.Slice(byte(read>>16), uint16(read), 3)
					if err != nil {
						return 0, false
					}
					target := Address24(b[2], uint16(b[0])|uint16(b[1])<<8)
					target = target&0xff0000 | uint32(uint16(target)+1) // RTL increments only PC.
					return target, image.IsROM(byte(target>>16), uint16(target))
				}
				for index := range indices {
					if target, ok := readTarget(index); ok {
						targets[target] = true
					}
				}
				// Interpolate only a coherent prefix anchored by independent
				// literal indices. Never extend beyond the largest anchor, choose
				// a stride from ROM contents, or retain a partially valid scan.
				stride, maximum := uint16(0), uint16(0)
				for index := range indices {
					a, b := stride, index
					for b != 0 {
						a, b = b, a%b
					}
					stride = a
					if index > maximum {
						maximum = index
					}
				}
				if len(indices) < 2 || stride < 3 || stride > 16 || maximum/stride < 2 || maximum/stride >= 256 {
					continue
				}
				var prefix []uint32
				for index := uint32(0); index <= uint32(maximum); index += uint32(stride) {
					target, ok := readTarget(uint16(index))
					if !ok {
						prefix = nil
						break
					}
					prefix = append(prefix, target)
				}
				for _, target := range prefix {
					targets[target] = true
				}
				if len(prefix) == 0 || !openPrefixes[w.g] || !openConsumers[fields{low.field, bank.field}] {
					continue
				}
				// Require the entire literal-anchored prefix to satisfy the
				// stronger cold-scan checks before extending it. A hole in that
				// evidence cannot be bridged by a guessed table length.
				source, err := image.Offset(byte(loAddr>>16), uint16(loAddr))
				if err != nil {
					continue
				}
				limit := uint32(0x10000) - uint32(uint16(loAddr))
				var extension, beyond []uint32
				for index := uint32(0); index+3 <= limit; index += uint32(stride) {
					target, ok := readTarget(uint16(index))
					bank, pc := byte(target>>16), uint16(target)
					if !ok || targetIsPadding(image, bank, pc) || inDataRegion(regions, bank, pc) {
						if index <= uint32(maximum) {
							extension = nil
						}
						// Records past the 256-record window need a proven end:
						// this first invalid record must be exactly where a
						// decoded routine begins, never a guessed table length.
						if len(beyond) != 0 && decodedEntry(source+int(index)) {
							extension = append(extension, beyond...)
						}
						break
					}
					if offset, err := image.Offset(bank, pc); err == nil && offset >= source && uint32(offset-source) < limit {
						limit = uint32(offset - source)
						if index+3 > limit {
							if index <= uint32(maximum) {
								extension = nil
							}
							break
						}
					}
					if index > uint32(maximum) && index/uint32(stride) < 256 {
						extension = append(extension, target)
					} else if index > uint32(maximum) {
						beyond = append(beyond, target)
					}
				}
				for _, target := range extension {
					targets[target] = true
				}
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
