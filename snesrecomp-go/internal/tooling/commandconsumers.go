package tooling

import (
	"encoding/json"
	"slices"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// A deferred command operand becomes a callback only through a separately
// decoded consumer that loads the same field spelling and dispatches through a
// scratch pointer. The consumer, not the command, owns that native frame: its
// Y is not the stream cursor and its DB is not the stream bank. A path is kept
// only when it publishes a literal cursor. That publication stays conditional
// on the field value reaching this consumer invocation, on the consumer's
// required branches, and on the ordinary publication/reload alias, lifetime
// and stream-bank obligations. It is never a return or a closed target set.
type ShadowCommandConsumerPath struct {
	DispatchPC uint32                    `json:"consumer_dispatch_pc"`
	Path       ShadowCommandCallbackPath `json:"conditional_native_callback_path"`
}

func (a *shadowCallbackAnalyzer) consumerPaths(stream ShadowCommandStream, step ShadowCommandWalkStep) []ShadowCommandConsumerPath {
	var out []ShadowCommandConsumerPath
	seen := map[string]bool{}
	for _, c := range stream.Commands {
		d := c.Deferred
		if d == nil || c.Index != step.Index || c.EntryPC != step.Handler {
			continue
		}
		for _, operand := range step.Operands {
			if operand.Kind != "deferred_field_word" || operand.LoadPC != d.LoadPC {
				continue
			}
			for _, consumer := range d.Consumers {
				n := consumer.Native
				if n.M != 0 || n.X != 0 || !a.image.IsROM(n.ProgramBank, operand.Value) {
					continue
				}
				for _, p := range a.consumerCallbackPaths(stream, n, uint32(n.ProgramBank)<<16|uint32(operand.Value)) {
					item := ShadowCommandConsumerPath{n.DispatchPC, p}
					if key, err := json.Marshal(item); err == nil && !seen[string(key)] {
						seen[string(key)] = true
						out = append(out, item)
					}
				}
			}
		}
	}
	return out
}

func (a *shadowCallbackAnalyzer) consumerCallbackPaths(stream ShadowCommandStream, n decoder.ForwardedIndirectField, target uint32) []ShadowCommandCallbackPath {
	key, _ := json.Marshal([]any{"deferred_consumer", stream.SelectorPC, stream.FetchPC, n, target})
	if paths, ok := a.cache[string(key)]; ok {
		return paths
	}
	var paths []ShadowCommandCallbackPath
	if f := a.consumerFrame(n); f != nil {
		y := shadowCallbackUnknown()
		for _, p := range a.queryFrameY(stream, target, f, n.Path, &y) {
			if !slices.ContainsFunc(p.CursorPublications, func(s ShadowCommandCursorStore) bool { return s.Literal }) {
				continue
			}
			for i := range p.Returns {
				if p.Returns[i].Source == "command_pea" && slices.Contains(n.Path, p.Returns[i].PushPC) {
					p.Returns[i].Source = "consumer_pea"
				}
			}
			p.Obligations = append(slices.Clip(p.Obligations), "deferred_field_must_reach_consumer_invocation", "consumer_required_branches_not_proven_feasible")
			paths = append(paths, p)
		}
		paths = distinctShadowCommandPathOutcomes(paths)
	}
	a.cache[string(key)] = paths
	return paths
}

// Replay the consumer's unique predecessor chain from its field load to the
// dispatch. The forwarding shape admits only stack, DB, flag and branch
// instructions besides the load and scratch store. Anything else, or a pull
// of a caller-owned byte, leaves the consumer frame unknown.
func (a *shadowCallbackAnalyzer) consumerFrame(n decoder.ForwardedIndirectField) *ShadowCommandFrame {
	if n.M != 0 || n.X != 0 || len(n.Path) < 2 || n.Path[0] != n.LoadPC || n.Path[len(n.Path)-1] != n.DispatchPC {
		return nil
	}
	unknown := ShadowCommandStackByte{Source: "unknown"}
	f := &ShadowCommandFrame{Complete: true, DB: unknown, Carry: "unknown", Decimal: "unknown"}
	for _, pc := range n.Path[:len(n.Path)-1] {
		i, reason := a.instruction(pc)
		if reason != "" {
			return nil
		}
		switch i.Mnemonic {
		case "PHB":
			f.Stack = append(f.Stack, f.DB)
		case "PHK":
			f.Stack = append(f.Stack, ShadowCommandStackByte{Source: "constant", Value: byte(pc >> 16)})
		case "PLB":
			if len(f.Stack) == 0 {
				return nil
			}
			f.DB = f.Stack[len(f.Stack)-1]
			f.DB.PushPC = 0
			f.Stack = f.Stack[:len(f.Stack)-1]
		case "PHA", "PHX", "PHY":
			f.Stack = append(f.Stack, ShadowCommandStackByte{Source: "unknown", Part: 1}, unknown)
		case "PEA":
			f.Stack = append(f.Stack,
				ShadowCommandStackByte{Source: "constant", Value: byte(i.Operand >> 8), Part: 1, PushPC: pc},
				ShadowCommandStackByte{Source: "constant", Value: byte(i.Operand), PushPC: pc})
		case "CLC":
			f.Carry = "clear"
		case "SEC":
			f.Carry = "set"
		case "CLD":
			f.Decimal = "clear"
		case "SED":
			f.Decimal = "set"
		case "LDA", "STA", "NOP", "JMP", "BRA", "BRL", "BEQ", "BNE", "BCC", "BCS", "BMI", "BPL", "BVC", "BVS":
		default:
			return nil
		}
	}
	return f
}
