package tooling

import (
	"encoding/json"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// A native path back to this selector's fetch, not a full script grammar or
// proof that memory writes cannot affect code, ROM mapping, or the next read.
type ShadowCommandAdvance struct {
	Status      string                  `json:"status"`
	CursorDelta int                     `json:"cursor_delta_from_command_entry"`
	StopPC      uint32                  `json:"stop_pc"`
	Path        []uint32                `json:"native_path"`
	Contexts    []analysis.EntryVariant `json:"decoded_in"`
}

type shadowCommandPathStep struct {
	key decoder.DecodeKey
	ins cpu65816.Instruction
}

type shadowCommandPath struct {
	entry   decoder.DecodeKey
	context analysis.EntryVariant
	steps   []shadowCommandPathStep
	stopPC  uint32
	stopKey decoder.DecodeKey
	reason  string
}

// Snapshot only bounded single-successor paths, not whole instruction maps.
// Sibling bodies are independent paths; missing edges are not decoded anew.
func shadowCommandNativeBlockers(cfg *config.Config) map[uint16]string {
	blocked := map[uint16]string{}
	if cfg != nil {
		for pc := range cfg.HLEFunctions {
			blocked[pc] = "native_HLE_boundary"
		}
		for pc := range cfg.HLEFunctionsIf {
			blocked[pc] = "native_HLE_boundary"
		}
		for pc := range cfg.HLEDispatch {
			blocked[pc] = "native_HLE_boundary"
		}
		for _, pc := range cfg.HLESPCUpload {
			blocked[pc] = "native_HLE_boundary"
		}
		for _, e := range cfg.Entries {
			if e.End != nil || e.ExitMX != nil || e.TailCallPC != nil || e.EntrySOffset != 0 {
				blocked[e.Start] = "authored_body_override"
			}
		}
	}
	return blocked
}

func collectShadowCommandPaths(g *decoder.Graph, cfg *config.Config, streams []ShadowCommandStream) []shadowCommandPath {
	blocked := shadowCommandNativeBlockers(cfg)
	starts := map[decoder.DecodeKey]bool{}
	if g.Entry.M == 0 && g.Entry.X == 0 {
		starts[g.Entry] = true
	}
	for _, s := range streams {
		selector := g.Instructions[decoder.DecodeKey{PC: s.SelectorPC}]
		if selector == nil {
			continue // No guessed saved-status history at an internal entry.
		}
		for _, c := range s.Commands {
			starts[decoder.DecodeKey{PC: c.EntryPC}] = true
		}
	}
	var out []shadowCommandPath
	for start := range starts {
		p := shadowCommandPath{entry: start, context: analysis.EntryVariant{PC: g.Entry.PC, EntryMX: analysis.MXState{M: g.Entry.M, X: g.Entry.X}}}
		at := start
		seen := map[decoder.DecodeKey]bool{}
		for range shadowCommandPrefixLimit {
			p.stopPC, p.stopKey = at.PC, at
			if reason := blocked[uint16(at.PC)]; reason != "" {
				p.reason = reason
				break
			}
			if seen[at] {
				p.reason = "native_path_cycle"
				break
			}
			seen[at] = true
			d := g.Instructions[at]
			if d == nil || d.Instruction == nil {
				p.reason = "outside_decoded_graph"
				break
			}
			step := shadowCommandPathStep{key: at, ins: *d.Instruction}
			p.steps = append(p.steps, step)
			if len(d.Successors) != 1 {
				p.reason = "ambiguous_or_terminal_native_successor"
				break
			}
			at = d.Successors[0]
			p.stopPC, p.stopKey, p.reason = at.PC, at, "native_path_budget"
		}
		out = append(out, p)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].entry.PC < out[j].entry.PC })
	return out
}

func summarizeShadowCommandAdvance(p shadowCommandPath, fetch uint32) ShadowCommandAdvance {
	r := ShadowCommandAdvance{Contexts: []analysis.EntryVariant{p.context}}
	if p.reason == "native_HLE_boundary" || p.reason == "authored_body_override" {
		r.StopPC, r.Status = p.stopPC, p.reason
		return r
	}
	for _, step := range p.steps {
		k, i := step.key, step.ins
		r.StopPC = k.PC
		if k.M != 0 || k.X != 0 || k.PDepth != 0 {
			r.Status = "native_width_or_saved_status_boundary"
			return r
		}
		if k.PC == fetch {
			r.Status = "linear_native_refetch"
			return r
		}
		r.Path = append(r.Path, k.PC)
		switch i.Mnemonic {
		case "INY":
			r.CursorDelta++
		case "DEY":
			r.CursorDelta--
		case "LDA", "LDX", "CMP", "CPX", "CPY", "AND", "ORA", "EOR", "ADC", "SBC", "XBA", "TAX", "TXA", "INX", "DEX",
			"NOP", "CLC", "SEC", "CLD", "SED", "BRA", "BRL":
		case "ASL", "LSR", "ROL", "ROR", "INC", "DEC":
			if i.Mode != cpu65816.ACC {
				r.Status = "unmodeled_memory_effect"
				return r
			}
		case "STA", "STZ", "STY":
			// Record only register-cursor effects. These field writes remain
			// conditional on ordinary RAM/ROM ownership and no I/O aliasing.
			if i.Mode != cpu65816.DPX && i.Mode != cpu65816.ABSX {
				r.Status = "unmodeled_memory_effect"
				return r
			}
		case "JMP":
			if i.Mode != cpu65816.ABS {
				r.Status = "dynamic_native_transfer"
				return r
			}
		default:
			r.Status = "unsupported_" + i.Mnemonic
			return r
		}
	}
	r.StopPC, r.Status = p.stopPC, p.reason
	// A decoded direct edge to the independently owned fetch is sufficient;
	// do not re-enter its owner prologue or merge arbitrary instruction maps.
	if p.reason == "outside_decoded_graph" && p.stopPC == fetch && len(r.Path) != 0 && p.stopKey.M == 0 && p.stopKey.X == 0 && p.stopKey.PDepth == 0 {
		r.Status = "linear_native_refetch"
	}
	return r
}

func annotateShadowCommandAdvances(streams []ShadowCommandStream, results []shadowDecodeResult) []ShadowCommandStream {
	paths := map[uint32][]shadowCommandPath{}
	for _, r := range results {
		if r.issue != nil {
			continue
		}
		for _, p := range r.commandPaths {
			// A selector's absent sibling is lack of ownership, not a second
			// conflicting native body. An independently decoded path may supply it.
			if len(p.steps) == 0 && p.reason == "outside_decoded_graph" {
				continue
			}
			paths[p.entry.PC] = append(paths[p.entry.PC], p)
		}
	}
	for n := range streams {
		streams[n].Commands = append([]ShadowCommandPrefix(nil), streams[n].Commands...)
		for j := range streams[n].Commands {
			c := &streams[n].Commands[j]
			byShape := map[string]*ShadowCommandAdvance{}
			for _, p := range paths[c.EntryPC] {
				r := summarizeShadowCommandAdvance(p, streams[n].FetchPC)
				contexts := r.Contexts
				r.Contexts = nil
				key, _ := json.Marshal(r)
				if byShape[string(key)] == nil {
					copy := r
					byShape[string(key)] = &copy
				}
				byShape[string(key)].Contexts = append(byShape[string(key)].Contexts, contexts...)
			}
			keys := make([]string, 0, len(byShape))
			for k := range byShape {
				keys = append(keys, k)
			}
			sort.Strings(keys)
			c.Advances = nil
			for _, k := range keys {
				r := byShape[k]
				r.Contexts = normalizeShadowCommandContexts(r.Contexts)
				c.Advances = append(c.Advances, *r)
			}
		}
	}
	return streams
}
