package tooling

import "github.com/DerrickGold/snesrecomp-go/internal/cpu65816"

// An affine index describes [D+slot]+offset, not an absolute object address.
// Slot identity/lifetime and D remain conditions, never global alias proofs.
type coldFieldIndex struct {
	slot  uint16
	node  int // -1 is a zero displacement
	valid bool
}

func (e *coldValueEngine) fieldIndex(q coldValueQuery) coldFieldIndex {
	w := e.walks[q.site.graph]
	x := w.indexExpression(q.site.key, "X", true)
	if len(x.Operations) != 0 || !shadowPointerWord(x.sourceKey, "A") || !shadowPointerWord(x.sourceKey, "X") {
		return coldFieldIndex{}
	}
	d := w.graph.Instructions[x.sourceKey]
	if d == nil || d.Instruction.Mode != cpu65816.DP || d.Instruction.Operand >= 0xff {
		return coldFieldIndex{}
	}
	if x.Source.Kind == "load" {
		return coldFieldIndex{uint16(d.Instruction.Operand), -1, true}
	}
	carry, decimal := shadowCommandFlag(w, x.sourceKey, 1), shadowCommandFlag(w, x.sourceKey, 8)
	if x.Source.Register != "A" || x.Source.Reason != "unsupported_value_effect_ADC" ||
		d.Instruction.Mnemonic != "ADC" || carry.Value == nil || *carry.Value != 0 ||
		decimal.Value != nil && *decimal.Value != 0 {
		return coldFieldIndex{}
	}
	// Decimal unknown is explicitly conditional on binary arithmetic. A
	// caller's carry is never guessed; CLC must establish it on this path.
	e.conditions["binary_affine_field_index"] = true
	node := e.node(coldValueQuery{coldValueSite{q.site.graph, x.sourceKey}, "A", q.context})
	return coldFieldIndex{uint16(d.Instruction.Operand), node, node >= 0}
}

func (e *coldValueEngine) fieldOffset(f coldFieldIndex) (uint16, bool) {
	if !f.valid {
		return 0, false
	}
	if f.node < 0 {
		return 0, true
	}
	v := e.nodes[f.node].values
	// A broad mask domain alone is not evidence for a particular field. The
	// rooted invocation must select one offset; never cross-product scripts.
	if len(v) != 1 {
		return 0, false
	}
	return v[0].word, true
}
