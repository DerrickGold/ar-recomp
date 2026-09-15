package tooling

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// Policy-only ownership for cold queries. Do not copy/index every decoded
// instruction when only an authored entry's internal bytes need propagation.
func coldNativeGraphs(image rom.Image, configs map[byte]*config.Config, graphs, eligible []*decoder.Graph) ([]*decoder.Graph, *shadowCallbackAnalyzer) {
	var banks []shadowBank
	for id, cfg := range configs {
		banks = append(banks, shadowBank{ID: id, Config: cfg})
	}
	sort.Slice(banks, func(i, j int) bool { return banks[i].ID < banks[j].ID })
	a := newShadowCallbackAnalyzer(image, banks, nil, nil)
	var ownership []shadowDecodeResult
	for _, g := range graphs {
		off, ok := a.offset(g.Entry.PC)
		if !ok || a.blocked[off] == "" {
			continue
		}
		ownership = append(ownership, shadowDecodeResult{entry: decoder.Variant{Address: g.Entry.PC, M: g.Entry.M, X: g.Entry.X}, instructions: shadowDecodedInstructions(byte(g.Entry.PC>>16), uint16(g.Entry.PC), g)})
	}
	if len(ownership) != 0 {
		a = newShadowCallbackAnalyzer(image, banks, ownership, nil)
	}
	var native []*decoder.Graph
	for _, g := range eligible {
		valid := true
		if len(a.blocked) != 0 {
			for _, d := range g.Instructions {
				for n := 0; n < int(d.Instruction.Length); n++ {
					if off, ok := a.offset(d.Key.PC + uint32(n)); ok && a.blocked[off] != "" {
						valid = false
					}
				}
			}
		}
		if valid {
			native = append(native, g)
		}
	}
	return native, a
}

func coldMappedTarget(a *shadowCallbackAnalyzer, pc uint32) bool {
	off, ok := a.offset(pc)
	if !ok || a.blocked[off] != "" {
		return false
	}
	i, err := decodeShadowInstruction(a.image, byte(pc>>16), uint16(pc), 0, 0)
	if err != nil || i.Opcode == 0 || i.Opcode == 0xff {
		return false
	}
	for n := 0; n < int(i.Length); n++ {
		p, ok := a.offset(pc + uint32(n))
		if !ok || p != off+n || a.blocked[p] != "" {
			return false
		}
	}
	return true
}
