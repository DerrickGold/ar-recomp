package tooling

import (
	"encoding/json"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

const shadowCommandDataPathLimit = 8

// Each record is one conditional native path from the non-negative data arm
// of the selector. Unknown branches remain distinct requirements, not a choice
// of canonical behavior. A cursor publication is evidence at that store only;
// it does not prove the field survives until a later engine invocation.
type ShadowCommandDataPath struct {
	SelectorPC   uint32                     `json:"selector_pc"`
	EntryPC      uint32                     `json:"entry_pc"`
	Status       string                     `json:"status"`
	CursorDelta  int                        `json:"cursor_delta_from_fetch_y"`
	StopPC       uint32                     `json:"stop_pc"`
	Path         []uint32                   `json:"native_path"`
	Branches     []ShadowCommandBranch      `json:"required_branches,omitempty"`
	Publications []ShadowCommandCursorStore `json:"conditional_cursor_publications,omitempty"`
	Contexts     []analysis.EntryVariant    `json:"decoded_in"`
}

type ShadowCommandBranch struct {
	PC       uint32 `json:"pc"`
	Mnemonic string `json:"mnemonic"`
	TargetPC uint32 `json:"successor_pc"`
	Taken    bool   `json:"taken"`
}

type shadowCommandDataState struct {
	at      decoder.DecodeKey
	path    ShadowCommandDataPath
	seen    map[decoder.DecodeKey]bool
	aCursor bool
	aDelta  int
}

func cloneShadowCommandDataState(s shadowCommandDataState) shadowCommandDataState {
	s.path.Path = append([]uint32(nil), s.path.Path...)
	s.path.Branches = append([]ShadowCommandBranch(nil), s.path.Branches...)
	s.path.Publications = append([]ShadowCommandCursorStore(nil), s.path.Publications...)
	seen := map[decoder.DecodeKey]bool{}
	for k, v := range s.seen {
		seen[k] = v
	}
	s.seen = seen
	return s
}

func collectShadowCommandDataPaths(g *decoder.Graph, cfg *config.Config, streams []ShadowCommandStream) []ShadowCommandDataPath {
	blocked := shadowCommandNativeBlockers(cfg)
	var out []ShadowCommandDataPath
	for _, stream := range streams {
		fetch := g.Instructions[decoder.DecodeKey{PC: stream.FetchPC}]
		test := g.Instructions[decoder.DecodeKey{PC: stream.SignTestPC}]
		if fetch == nil || test == nil || fetch.Instruction == nil || test.Instruction == nil ||
			fetch.Instruction.Mnemonic != "LDA" || test.Instruction.Mnemonic != "BMI" || len(test.Successors) != 2 {
			continue
		}
		fallPC := test.Key.PC&0xff0000 | uint32(uint16(test.Key.PC)+uint16(test.Instruction.Length))
		var entry decoder.DecodeKey
		found := false
		for _, next := range test.Successors {
			if next.PC == fallPC && next.M == 0 && next.X == 0 && next.PDepth == 0 {
				entry = next
				found = true
			}
		}
		if !found {
			continue
		}
		if test.Key.PC&0xff0000|test.Instruction.Operand == fallPC {
			continue // A zero-displacement branch does not separate a data arm.
		}
		base := ShadowCommandDataPath{SelectorPC: stream.SelectorPC, EntryPC: entry.PC,
			Branches: []ShadowCommandBranch{{test.Key.PC, "BMI", entry.PC, false}},
			Contexts: []analysis.EntryVariant{{PC: g.Entry.PC, EntryMX: analysis.MXState{M: g.Entry.M, X: g.Entry.X}}}}
		if reason := blocked[uint16(fetch.Key.PC)]; reason != "" {
			base.Status, base.StopPC = reason, fetch.Key.PC
			out = append(out, base)
			continue
		}
		if reason := blocked[uint16(test.Key.PC)]; reason != "" {
			base.Status, base.StopPC = reason, test.Key.PC
			out = append(out, base)
			continue
		}
		queue := []shadowCommandDataState{{at: entry, path: base, seen: map[decoder.DecodeKey]bool{}}}
		var paths []ShadowCommandDataPath
		for len(queue) != 0 {
			s := queue[0]
			queue = queue[1:]
			for {
				s.path.StopPC = s.at.PC
				if reason := blocked[uint16(s.at.PC)]; reason != "" {
					s.path.Status = reason
					break
				}
				if s.at.M != 0 || s.at.X != 0 || s.at.PDepth != 0 {
					s.path.Status = "native_width_or_saved_status_boundary"
					break
				}
				if s.at.PC == stream.FetchPC {
					s.path.Status = "linear_native_refetch"
					break
				}
				if len(s.path.Path) >= shadowCommandPrefixLimit {
					s.path.Status = "native_data_instruction_budget"
					break
				}
				if s.seen[s.at] {
					s.path.Status = "native_data_cycle"
					break
				}
				s.seen[s.at] = true
				d := g.Instructions[s.at]
				if d == nil || d.Instruction == nil {
					s.path.Status = "outside_decoded_graph"
					break
				}
				i := d.Instruction
				s.path.Path = append(s.path.Path, s.at.PC)
				switch i.Mnemonic {
				case "INY":
					s.path.CursorDelta++
				case "DEY":
					s.path.CursorDelta--
				case "TYA":
					s.aCursor = true
					s.aDelta = s.path.CursorDelta
				case "LDA", "ADC", "SBC", "AND", "ORA", "EOR", "XBA", "TXA":
					s.aCursor = false
				case "LDX", "CMP", "CPX", "CPY", "TAX", "INX", "DEX", "NOP", "CLC", "SEC", "CLD", "SED", "BRA", "BRL":
				case "ASL", "LSR", "ROL", "ROR", "INC", "DEC":
					if i.Mode != cpu65816.ACC {
						s.path.Status = "unmodeled_memory_effect"
					} else {
						s.aCursor = false
					}
				case "STA", "STY", "STZ":
					if i.Mode != cpu65816.DPX && i.Mode != cpu65816.ABSX {
						s.path.Status = "unmodeled_memory_effect"
						break
					}
					if i.Mnemonic == "STY" || i.Mnemonic == "STA" && s.aCursor {
						delta := s.path.CursorDelta
						if i.Mnemonic == "STA" {
							delta = s.aDelta
						}
						s.path.Publications = append(s.path.Publications, ShadowCommandCursorStore{PC: s.at.PC, Mode: i.Mode.String(), Operand: uint16(i.Operand), CursorDelta: delta})
					}
				case "JMP":
					if i.Mode != cpu65816.ABS {
						s.path.Status = "dynamic_native_transfer"
					}
				case "BEQ", "BNE", "BCC", "BCS", "BMI", "BPL", "BVC", "BVS":
					if len(d.Successors) != 2 {
						s.path.Status = "ambiguous_native_branch"
						break
					}
					target := s.at.PC&0xff0000 | i.Operand
					fall := s.at.PC&0xff0000 | uint32(uint16(s.at.PC)+uint16(i.Length))
					if target == fall && d.Successors[0] == d.Successors[1] && d.Successors[0].PC == target {
						s.at = d.Successors[0]
						continue // Both outcomes have the same native edge.
					}
					if !((d.Successors[0].PC == target && d.Successors[1].PC == fall) || (d.Successors[1].PC == target && d.Successors[0].PC == fall)) {
						s.path.Status = "ambiguous_native_branch"
						break
					}
					if len(paths)+len(queue)+2 > shadowCommandDataPathLimit {
						s.path.Status = "native_data_path_budget"
						break
					}
					// Enumerate both decoded arms, keeping the required branch.
					// No feasibility claim is made from unknown object/flag values.
					nexts := append([]decoder.DecodeKey(nil), d.Successors...)
					sort.Slice(nexts, func(a, b int) bool { return nexts[a].PC < nexts[b].PC })
					for _, next := range nexts {
						child := cloneShadowCommandDataState(s)
						child.at = next
						child.path.Branches = append(child.path.Branches, ShadowCommandBranch{s.at.PC, i.Mnemonic, next.PC, next.PC == target})
						queue = append(queue, child)
					}
					s.path.Status = "forked"
				default:
					s.path.Status = "unsupported_" + i.Mnemonic
				}
				if s.path.Status != "" {
					break
				}
				if len(d.Successors) != 1 {
					s.path.Status = "ambiguous_or_terminal_native_successor"
					break
				}
				s.at = d.Successors[0]
			}
			if s.path.Status != "forked" {
				paths = append(paths, s.path)
			}
		}
		out = append(out, paths...)
	}
	return out
}

func annotateShadowCommandDataPaths(streams []ShadowCommandStream, results []shadowDecodeResult) []ShadowCommandStream {
	for n := range streams {
		streams[n].DataPaths = nil
		byShape := map[string]*ShadowCommandDataPath{}
		for _, r := range results {
			if r.issue != nil {
				continue
			}
			for _, p := range r.commandDataPaths {
				if p.SelectorPC != streams[n].SelectorPC || len(p.Path) == 0 && p.Status == "outside_decoded_graph" {
					continue
				}
				contexts := p.Contexts
				p.Contexts = nil
				key, _ := json.Marshal(p)
				if byShape[string(key)] == nil {
					copy := p
					byShape[string(key)] = &copy
				}
				byShape[string(key)].Contexts = append(byShape[string(key)].Contexts, contexts...)
			}
		}
		keys := make([]string, 0, len(byShape))
		for k := range byShape {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			p := byShape[k]
			p.Contexts = normalizeShadowCommandContexts(p.Contexts)
			streams[n].DataPaths = append(streams[n].DataPaths, *p)
		}
	}
	return streams
}
