package tooling

import (
	"maps"
	"math/bits"
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// A decoded predecessor choice, not a proven branch outcome or reachable path.
type ShadowCommandInputEdge struct {
	FromPC  uint32           `json:"from_pc"`
	ToPC    uint32           `json:"to_pc"`
	FromMX  analysis.MXState `json:"from_mx"`
	ToMX    analysis.MXState `json:"to_mx"`
	Opcode  byte             `json:"opcode"`
	Operand uint32           `json:"operand"`
}

type ShadowCommandInputPath struct {
	Value ShadowInitializerIndex   `json:"value_expression"`
	Edges []ShadowCommandInputEdge `json:"required_predecessor_edges,omitempty"`
	read  *shadowStreamInputRead
	flags *ShadowCommandInputFlags
}

const shadowCommandInputPathLimit = 8
const shadowCommandInputPathBudget = 64

// Specialize stream-root and caller input queries. Other inventories and
// generation keep their expression rules. Paths never splice instruction owners;
// every predecessor choice remains in the evidence. No unknown branch is picked
// as canonical, and a partial result cannot claim the complete input domain.
func collectShadowCommandInputPaths(walk shadowPointerWalk, end decoder.DecodeKey, reg string) ([]ShadowCommandInputPath, bool) {
	budget := shadowCommandInputPathBudget
	truncated := false
	var visit func(shadowPointerWalk, decoder.DecodeKey, string, []ShadowCommandInputEdge, int) []ShadowCommandInputPath
	visit = func(w shadowPointerWalk, at decoder.DecodeKey, reg string, edges []ShadowCommandInputEdge, depth int) []ShadowCommandInputPath {
		if budget == 0 || depth == 16 {
			truncated = true
			return nil
		}
		budget--
		expr := w.indexExpression(at, reg, true)
		if at == w.graph.Entry && shadowPointerWord(at, reg) {
			// Query BEFORE the entry instruction, even when a backedge also
			// reaches it. Peeling an arithmetic instruction at entry is valid;
			// importing the previous loop iteration's definition is not.
			expr = ShadowInitializerIndex{Source: ShadowStoredOrigin{Kind: "entry_register", PC: at.PC, Register: reg, Reason: "entry_value_unproven"}, DomainSize: 65536, sourceKey: at}
		}
		if expr.Source.Kind == "load" && !shadowStreamLocalPrecedes(w, at, expr.Source.PC) {
			expr.Source.Kind, expr.Source.Reason = "unknown", "input_definition_crosses_entry_or_join"
		}
		origin := expr.sourceKey
		finish := func() []ShadowCommandInputPath {
			return []ShadowCommandInputPath{{Value: expr, Edges: slices.Clone(edges), read: collectShadowStreamInputRead(w, expr, 0), flags: shadowCommandFlags(w, end)}}
		}
		if expr.Source.Kind != "unknown" || at == w.graph.Entry {
			return finish()
		}
		if origin == w.graph.Entry && expr.Source.Reason == "entry_or_ambiguous_predecessor" {
			return finish()
		}
		if expr.Source.Reason == "entry_or_ambiguous_predecessor" && len(w.preds[origin]) > 1 {
			preds := slices.Clone(w.preds[origin])
			sort.Slice(preds, func(i, j int) bool { return shadowStoredJSONKey(preds[i]) < shadowStoredJSONKey(preds[j]) })
			preds = slices.Compact(preds)
			var out []ShadowCommandInputPath
			for _, p := range preds {
				if len(out) == shadowCommandInputPathLimit {
					truncated = true
					break
				}
				d := w.graph.Instructions[p]
				if d == nil || d.Instruction == nil {
					out = append(out, finish()...)
					continue
				}
				// Never override a choice already used earlier in the path.
				if slices.ContainsFunc(edges, func(e ShadowCommandInputEdge) bool {
					return e.ToPC == origin.PC && e.ToMX == (analysis.MXState{M: origin.M, X: origin.X})
				}) {
					return finish()
				}
				next := w
				next.preds = maps.Clone(w.preds)
				next.preds[origin] = []decoder.DecodeKey{p}
				e := ShadowCommandInputEdge{FromPC: p.PC, ToPC: origin.PC, FromMX: analysis.MXState{M: p.M, X: p.X}, ToMX: analysis.MXState{M: origin.M, X: origin.X}, Opcode: d.Instruction.Opcode, Operand: d.Instruction.Operand}
				for _, path := range visit(next, at, reg, append(slices.Clone(edges), e), depth+1) {
					if len(out) == shadowCommandInputPathLimit {
						truncated = true
						break
					}
					out = append(out, path)
				}
			}
			return out
		}
		d := w.graph.Instructions[origin]
		if d == nil || d.Instruction == nil {
			return finish()
		}
		i := d.Instruction
		op := ShadowStoredOperation{PC: origin.PC, Mnemonic: i.Mnemonic, Operand: uint16(i.Operand)}
		valid := false
		if expr.Source.Register == "A" && i.Mode == cpu65816.IMM && (i.Mnemonic == "ADC" || i.Mnemonic == "SBC") {
			carry, decimal := shadowCommandFlag(w, origin, 1), shadowCommandFlag(w, origin, 8)
			op.Carry, op.Decimal = &carry, &decimal
			valid = true
		}
		if expr.Source.Register == "A" && i.Mode == cpu65816.ACC && (i.Mnemonic == "INC" || i.Mnemonic == "DEC") {
			valid = true
		}
		if expr.Source.Register == "X" && (i.Mnemonic == "INX" || i.Mnemonic == "DEX") {
			valid = true
		}
		if expr.Source.Register == "Y" && (i.Mnemonic == "INY" || i.Mnemonic == "DEY") {
			valid = true
		}
		if !valid {
			return finish()
		}
		paths := visit(w, origin, expr.Source.Register, edges, depth+1)
		for n := range paths {
			p := &paths[n]
			p.Value.Operations = append(slices.Clone(p.Value.Operations), op)
			p.Value.Operations = append(p.Value.Operations, expr.Operations...)
			shadowCommandPathBits(&p.Value)
		}
		return paths
	}
	paths := visit(walk, end, reg, nil, 0)
	// No extra public record for the ordinary single unique predecessor query.
	base := walk.indexExpression(end, reg, true)
	if len(paths) == 1 && len(paths[0].Edges) == 0 && shadowStoredJSONKey(paths[0].Value) == shadowStoredJSONKey(base) {
		return nil, truncated
	}
	return paths, truncated
}

func shadowCommandInputWord(word uint16, op ShadowStoredOperation) (uint16, bool) {
	switch op.Mnemonic {
	case "INC", "INX", "INY":
		return word + 1, true
	case "DEC", "DEX", "DEY":
		return word - 1, true
	case "ADC", "SBC":
		if op.Carry == nil || op.Carry.Value == nil || *op.Carry.Value > 1 || op.Decimal == nil || op.Decimal.Value == nil || *op.Decimal.Value != 0 {
			return 0, false
		}
		if op.Mnemonic == "ADC" {
			return word + op.Operand + uint16(*op.Carry.Value), true
		}
		return word - op.Operand - 1 + uint16(*op.Carry.Value), true
	default:
		zero, one := initializerBits(^word, word, op)
		return one, zero|one == 0xffff
	}
}

func shadowCommandPathBits(expr *ShadowInitializerIndex) {
	expr.KnownZero, expr.KnownOne = 0, 0
	if expr.Source.Kind == "load" && expr.Source.Mode == "imm" {
		expr.KnownOne = uint16(expr.Source.Operand)
		expr.KnownZero = ^expr.KnownOne
	}
	for _, op := range expr.Operations {
		if expr.KnownZero|expr.KnownOne == 0xffff {
			if word, ok := shadowCommandInputWord(expr.KnownOne, op); ok {
				expr.KnownOne, expr.KnownZero = word, ^word
				continue
			}
		}
		expr.KnownZero, expr.KnownOne = initializerBits(expr.KnownZero, expr.KnownOne, op)
	}
	expr.DomainSize = uint32(1) << bits.OnesCount16(^(expr.KnownZero | expr.KnownOne))
	expr.DomainValues = nil
	if expr.DomainSize == 1 {
		expr.DomainValues = []uint16{expr.KnownOne}
	}
}
