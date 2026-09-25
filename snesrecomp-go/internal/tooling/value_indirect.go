package tooling

import (
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
)

// Arithmetic remains cold inventory. Unknown decimal mode is an explicit
// binary-arithmetic condition, not an inferred CPU flag or a dispatch proof.
// Carry, unlike decimal mode, is never guessed: it must have a finite origin.
// Operands bound to one index definition are combined only when they agree.
func (e *coldValueEngine) add(q coldValueQuery, carryOnly bool) []coldWordValue {
	w := e.walks[q.site.graph]
	if q.site.key.M != 0 {
		return nil
	}
	d := shadowCommandFlag(w, q.site.key, 8)
	if d.Value != nil && *d.Value != 0 {
		e.boundaries["decimal_value_arithmetic"] = true
		return nil
	}
	if d.Value == nil {
		e.conditions["binary_value_arithmetic"] = true
	}
	left := e.values(coldValueQuery{q.site, "A", q.context})
	right := e.values(coldValueQuery{q.site, "read", q.context})
	carry := e.values(coldValueQuery{q.site, "carry", q.context})
	if operations, ok := e.sameAddOperand(q); ok {
		// LDA slot; ASL; CLC; ADC slot is three times ONE value, not
		// a Cartesian product of independently selected slot lifetimes.
		var out []coldWordValue
		for _, b := range right {
			a := b.word
			for _, op := range operations {
				z, o := initializerBits(^a, a, op)
				if z|o != 0xffff {
					return nil
				}
				a = o
			}
			for _, c := range carry {
				binding, ok := coldBind(b.binding, c.binding)
				if !ok {
					continue
				}
				sum := uint32(a) + uint32(b.word) + uint32(c.word)
				word := uint16(sum)
				if carryOnly {
					word = uint16(sum >> 16)
				}
				out = append(out, coldWordValue{word: word, binding: binding, speculative: b.speculative || c.speculative})
			}
		}
		return out
	}
	if len(left)*len(right)*len(carry) > coldValueWorkLimit {
		e.boundaries["arithmetic_product_budget"] = true
		return nil
	}
	var out []coldWordValue
	for _, a := range left {
		for _, b := range right {
			ab, ok := coldBind(a.binding, b.binding)
			if !ok {
				continue
			}
			for _, c := range carry {
				binding, ok := coldBind(ab, c.binding)
				if !ok {
					continue
				}
				sum := uint32(a.word) + uint32(b.word) + uint32(c.word)
				word := uint16(sum)
				if carryOnly {
					word = uint16(sum >> 16)
				}
				out = append(out, coldWordValue{word: word, binding: binding, speculative: a.speculative || b.speculative || c.speculative})
			}
		}
	}
	return out
}

func (e *coldValueEngine) sameAddOperand(q coldValueQuery) ([]ShadowStoredOperation, bool) {
	w := e.walks[q.site.graph]
	i := w.graph.Instructions[q.site.key].Instruction
	x := w.indexExpression(q.site.key, "A", true)
	if x.Source.Kind != "load" || i.Mode != cpu65816.DP && i.Mode != cpu65816.ABS {
		return nil, false
	}
	load := w.graph.Instructions[x.sourceKey].Instruction
	if load.Mode != i.Mode || load.Operand != i.Operand {
		return nil, false
	}
	at := q.site.key
	for range shadowPointerWalkLimit {
		if at == w.graph.Entry {
			break
		}
		prev := w.previous(at)
		if prev == nil {
			break
		}
		if prev.Key == x.sourceKey {
			e.conditions["repeated_operand_no_external_alias"] = true
			return x.Operations, true
		}
		at = prev.Key
		p := prev.Instruction
		if p.Mode == cpu65816.ACC || p.Mode == cpu65816.IMM && (p.Mnemonic == "AND" || p.Mnemonic == "ORA" || p.Mnemonic == "EOR") || shadowPointerTransparent(p) {
			continue
		}
		break // any memory write, bank/D change, call or unknown effect
	}
	return nil, false
}

func (e *coldValueEngine) carry(q coldValueQuery) []coldWordValue {
	w := e.walks[q.site.graph]
	at := q.site.key
	for range shadowPointerWalkLimit {
		if at == w.graph.Entry {
			break
		}
		prev := w.previous(at)
		if prev == nil {
			break
		}
		at = prev.Key
		i := prev.Instruction
		if i.Mnemonic == "ADC" {
			return e.add(coldValueQuery{coldValueSite{q.site.graph, at}, "A", q.context}, true)
		}
		if at.M == 0 && i.Mode == cpu65816.ACC && (i.Mnemonic == "ASL" || i.Mnemonic == "LSR") {
			var out []coldWordValue
			for _, v := range e.values(coldValueQuery{coldValueSite{q.site.graph, at}, "A", q.context}) {
				bit := v.word & 1
				if i.Mnemonic == "ASL" {
					bit = v.word >> 15
				}
				out = append(out, coldWordValue{word: bit, binding: v.binding, speculative: v.speculative})
			}
			return out
		}
		if f, stop := shadowStoredFlagEffect(prev, 1); stop {
			if f.Value != nil {
				return []coldWordValue{{word: uint16(*f.Value)}}
			}
			break
		}
	}
	e.boundaries["unknown_value_carry"] = true
	return nil
}

func (e *coldValueEngine) zeroPointerD(q coldValueQuery) bool {
	if d, ok := shadowStreamConstantWord(e.walks[q.site.graph].bankOrD(q.site.key, "D")); ok {
		return d == 0
	}
	e.conditions["direct_pointer_D_zero"] = true
	return true
}

// Only a local full-word reaching definition can supply a scratch pointer.
// In particular, the load before a store back to that same slot must read the
// EARLIER definition, not globally union its own result into the pointer.
func (e *coldValueEngine) localPointer(q coldValueQuery) []coldWordValue {
	w := e.walks[q.site.graph]
	i := w.graph.Instructions[q.site.key].Instruction
	slot := i.Operand
	if slot >= 0xff || !e.zeroPointerD(q) {
		return nil
	}
	at := q.site.key
	for range shadowStreamLocalStoreLimit {
		if at == w.graph.Entry {
			break
		}
		prev := w.previous(at)
		if prev == nil {
			break
		}
		at = prev.Key
		p := prev.Instruction
		r, width := coldValueStore(prev)
		write := r != "" || p.Mnemonic == "STZ" || p.Mnemonic == "TRB" || p.Mnemonic == "TSB"
		switch p.Mnemonic {
		case "ASL", "LSR", "ROL", "ROR", "INC", "DEC":
			write = p.Mode != cpu65816.ACC
		}
		if write {
			if width == 0 {
				width = 2 - int(at.M)
			}
			if p.Mode != cpu65816.DP && p.Mode != cpu65816.ABS {
				break // indexed/indirect writes can alias either pointer byte
			}
			if p.Mode == cpu65816.ABS {
				bank, ok := e.contextBank(coldValueSite{q.site.graph, at}, q.context, 0)
				if !ok || bank&0x7f >= 0x40 {
					break // includes hardware and bank identity uncertainty
				}
			}
			if p.Operand < slot+2 && slot < p.Operand+uint32(width) {
				if width != 2 || p.Operand != slot || r == "" && p.Mnemonic != "STZ" {
					break
				}
				if p.Mnemonic == "STZ" {
					return []coldWordValue{{}}
				}
				e.conditions["local_pointer_no_external_alias"] = true
				return e.values(coldValueQuery{coldValueSite{q.site.graph, at}, r, q.context})
			}
			if p.Operand >= 0x2000 {
				break // hardware writes can trigger DMA
			}
			continue
		}
		switch p.Mnemonic {
		case "LDA", "LDX", "LDY", "ADC", "SBC", "AND", "ORA", "EOR", "ASL", "LSR", "ROL", "ROR", "INC", "DEC", "INX", "DEX", "INY", "DEY", "XBA", "TAX", "TAY", "TXA", "TYA", "TXY", "TYX":
			continue
		case "PEA", "PER", "PHA", "PHX", "PHY", "PHK", "PHB", "PHP", "PHD":
			e.conditions["saved_stack_non_aliasing"] = true
			continue
		}
		if shadowPointerTransparent(p) {
			continue
		}
		break // calls, pulls, D/S changes, joins and unknown effects
	}
	e.boundaries["local_pointer_definition_unproven"] = true
	return nil
}

func (e *coldValueEngine) indirectRead(q coldValueQuery) []coldWordValue {
	i := e.graphs[q.site.graph].Instructions[q.site.key].Instruction
	pointers := e.values(coldValueQuery{q.site, "pointer", q.context})
	indices := []coldWordValue{{}}
	if i.Mode == cpu65816.INDIRY {
		if q.site.key.X != 0 {
			return nil
		}
		indices = e.values(coldValueQuery{q.site, "Y", q.context})
	}
	bank, ok := e.contextBank(q.site, q.context, 0)
	if !ok {
		e.boundaries["unknown_indirect_read_bank"] = true
		return nil
	}
	var out []coldWordValue
	for _, p := range pointers {
		var agreeing []coldWordValue
		for _, y := range indices {
			binding, ok := coldBind(p.binding, y.binding)
			if !ok {
				continue
			}
			y.binding = binding
			agreeing = append(agreeing, y)
		}
		out = append(out, e.readWords(bank, p.word, agreeing, coldReadFamily{})...)
	}
	return out
}

func (e *coldValueEngine) indirectConsumers() []coldIndexedConsumer {
	var out []coldIndexedConsumer
	for gi, g := range e.graphs {
		for _, key := range g.Order {
			i := g.Instructions[key].Instruction
			if i.Opcode != 0x6c || i.Operand >= 0xff || len(i.DispatchEntries) != 0 {
				continue
			}
			for _, ctx := range e.invocationContexts(gi, coldValueContextDepth, map[int]bool{}) {
				node := e.node(coldValueQuery{coldValueSite{gi, key}, "pointer", ctx})
				if node >= 0 {
					out = append(out, coldIndexedConsumer{node, byte(key.PC >> 16)})
				}
			}
		}
	}
	return out
}
