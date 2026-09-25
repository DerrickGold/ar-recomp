package tooling

import (
	"encoding/json"
	"fmt"
	"io"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const shadowCommandWalkLimit = 32
const shadowCommandWalkPathLimit = 16

type ShadowCommandWalkOrigin struct {
	Root      int `json:"root_index"`
	Reference int `json:"reference_index"`
}

type ShadowCommandWalk struct {
	SelectorPC       uint32                    `json:"selector_pc"`
	FirstFetchPC     uint32                    `json:"first_fetch_pc"`
	Origins          []ShadowCommandWalkOrigin `json:"root_references"`
	Steps            []ShadowCommandWalkStep   `json:"steps,omitempty"`
	StopPC           uint32                    `json:"stop_pc"`
	StopReason       string                    `json:"stop_reason"`
	ProofObligations []string                  `json:"proof_obligations"`
}

type ShadowCommandWalkStep struct {
	Kind          string                      `json:"kind,omitempty"`
	FetchPC       uint32                      `json:"fetch_pc"`
	Word          uint16                      `json:"command_word"`
	Index         int                         `json:"command_index"`
	Handler       uint32                      `json:"native_handler"`
	Operands      []ShadowCommandROMOperand   `json:"conditional_operands,omitempty"`
	NextPC        *uint32                     `json:"next_fetch_pc,omitempty"`
	DataPath      *ShadowCommandDataPath      `json:"conditional_native_data_path,omitempty"`
	CallbackPath  *ShadowCommandCallbackPath  `json:"conditional_native_callback_path,omitempty"`
	NativePath    *ShadowCommandCallbackPath  `json:"conditional_native_command_path,omitempty"`
	ConsumerPaths []ShadowCommandConsumerPath `json:"conditional_deferred_consumer_paths,omitempty"`
}

type ShadowCommandROMOperand struct {
	Kind       string                `json:"kind"`
	LoadPC     uint32                `json:"load_pc"`
	SourcePC   uint32                `json:"source_pc"`
	Value      uint16                `json:"value"`
	Candidates []ShadowCommandTarget `json:"open_target_candidates,omitempty"`
}

type ShadowCommandTarget struct {
	PC         uint32           `json:"pc"`
	DispatchPC uint32           `json:"dispatch_pc"`
	MX         analysis.MXState `json:"decoded_mx"`
}

// Consume only the initial rooted position or a complete, single-path native
// refetch effect. Never search forward/backward for a convenient tag byte.
// Existing input paths, native memory/return contracts and field correlations
// remain conditional; these reads are not exported as proven entry facts.
func walkShadowCommandStreams(image romimage.Image, streams []ShadowCommandStream, roots []ShadowCommandRoot) []ShadowCommandWalk {
	return walkShadowCommandStreamsWithCallbacks(image, streams, roots, nil)
}

func walkShadowCommandStreamsWithCallbacks(image romimage.Image, streams []ShadowCommandStream, roots []ShadowCommandRoot, callbacks *shadowCallbackAnalyzer) []ShadowCommandWalk {
	var out []ShadowCommandWalk
	byShape := map[string]int{}
	for ri, root := range roots {
		for fi, ref := range root.References {
			if ref.FirstFetchPC == nil || ref.StreamPC == nil {
				continue
			}
			for _, s := range streams {
				if s.SelectorPC != root.SelectorPC || s.FetchPC != root.FetchPC || s.TagByteOffset != int(root.FetchOperand)+1 {
					continue
				}
				for _, w := range walkShadowCommandStreamPathsWithCallbacks(image, s, *ref.FirstFetchPC, root.FetchOperand, callbacks) {
					key, _ := json.Marshal(w)
					index, exists := byShape[string(key)]
					if !exists {
						index = len(out)
						byShape[string(key)] = index
						out = append(out, w)
					}
					out[index].Origins = append(out[index].Origins, ShadowCommandWalkOrigin{ri, fi})
				}
			}
		}
	}
	return out
}

func walkShadowCommandStream(image romimage.Image, s ShadowCommandStream, first uint32, fetchOperand uint16) ShadowCommandWalk {
	return walkShadowCommandSegment(image, s, first, fetchOperand, shadowCommandWalkLimit, map[uint32]bool{})
}

// Stop a linear tagged-command segment at data; the bounded path driver below
// preserves every data-arm alternative and carries cycle/budget state across
// segment boundaries. No callback return or later cursor reload is invented.
func walkShadowCommandSegment(image romimage.Image, s ShadowCommandStream, first uint32, fetchOperand uint16, limit int, seen map[uint32]bool) ShadowCommandWalk {
	w := ShadowCommandWalk{SelectorPC: s.SelectorPC, FirstFetchPC: first,
		ProofObligations: []string{"conditional_root_paths_not_reachability", "native_HLE_and_ROM_mapping_noninterference", "field_D_DB_index_alias_and_lifetime", "selector_table_entries_not_closed_extent", "callback_live_PB_MX_and_return_contract", "native_branch_feasibility_and_published_cursor_lifetime_unproven", "no_proven_entry_or_closed_dispatch_promotion"}}
	pc := first
	bank := byte(first >> 16)
	for range limit {
		w.StopPC = pc
		if seen[pc] {
			w.StopReason = "stream_cursor_cycle"
			return w
		}
		seen[pc] = true
		word, ok := shadowStreamROMWord(image, bank, uint32(uint16(pc)))
		if !ok {
			w.StopReason = "fetch_not_ROM_or_bank_boundary"
			return w
		}
		if word&0x8000 == 0 {
			w.StopReason = "untagged_data_path_unmodeled"
			return w
		}
		index := int(word>>8) & int(s.SelectorMask)
		var commands []ShadowCommandPrefix
		for _, c := range s.Commands {
			if c.Index == index {
				commands = append(commands, c)
			}
		}
		if len(commands) != 1 {
			w.StopReason = "missing_or_ambiguous_native_command"
			return w
		}
		c := commands[0]
		step := ShadowCommandWalkStep{FetchPC: pc, Word: word, Index: index, Handler: c.EntryPC}
		// A graph-level host/body override cannot authorize native operand
		// recovery even if the ROM prefix by itself looks well formed.
		for _, a := range c.Advances {
			if a.Status == "native_HLE_boundary" || a.Status == "authored_body_override" {
				w.StopReason = a.Status
				return w
			}
		}
		entryY := int(uint16(pc)) - int(fetchOperand) + s.EntryCursorDelta
		if entryY < 0 || entryY > 0xffff {
			w.StopReason = "command_entry_cursor_bank_boundary"
			return w
		}
		operand := func(kind string, load uint32, offset int, source string, constant *uint8) *ShadowCommandROMOperand {
			b := bank
			if source == "constant" && constant != nil {
				b = *constant
			} else if source != "command_entry_db" {
				return nil
			}
			addr := entryY + offset
			if addr < 0 || addr > 0xfffe {
				return nil
			}
			value, ok := shadowStreamROMWord(image, b, uint32(addr))
			if !ok {
				return nil
			}
			return &ShadowCommandROMOperand{Kind: kind, LoadPC: load, SourcePC: uint32(b)<<16 | uint32(addr), Value: value}
		}
		addTarget := func(p *ShadowCommandROMOperand, b byte, site uint32, mx analysis.MXState) {
			if image.IsROM(b, p.Value) {
				p.Candidates = append(p.Candidates, ShadowCommandTarget{uint32(b)<<16 | uint32(p.Value), site, mx})
			}
		}
		if d := c.Deferred; d != nil {
			p := operand("deferred_field_word", d.LoadPC, d.OperandOffset, d.StreamBankSource, d.StreamBank)
			if p == nil {
				w.StopReason = "operand_bank_or_ROM_unknown"
				w.Steps = append(w.Steps, step)
				return w
			}
			for _, consumer := range d.Consumers {
				n := consumer.Native
				addTarget(p, n.ProgramBank, n.DispatchPC, analysis.MXState{M: n.M, X: n.X})
			}
			step.Operands = append(step.Operands, *p)
		}
		if c.Callback != nil {
			d := c.Callback
			p := operand("immediate_callback_word", d.LoadPC, d.OperandOffset, d.StreamBankSource, d.StreamBank)
			if p == nil {
				w.StopReason = "operand_bank_or_ROM_unknown"
				w.Steps = append(w.Steps, step)
				return w
			}
			addTarget(p, d.DecodedProgramBank, d.DispatchPC, analysis.MXState{})
			step.Operands = append(step.Operands, *p)
		}
		w.Steps = append(w.Steps, step)
		// No optimistic choice among conflicting or incomplete owner paths.
		if len(c.Advances) == 0 {
			w.StopReason = "native_refetch_unproven"
			return w
		}
		delta := c.Advances[0].CursorDelta
		for _, a := range c.Advances {
			if a.Status != "linear_native_refetch" || a.CursorDelta != delta {
				w.StopReason = "native_refetch_unproven"
				return w
			}
		}
		nextY := entryY + delta
		if nextY < 0 || nextY > 0xffff || nextY+int(fetchOperand) > 0xfffe {
			w.StopReason = "next_cursor_bank_boundary"
			return w
		}
		next := uint32(bank)<<16 | uint32(nextY+int(fetchOperand))
		w.Steps[len(w.Steps)-1].NextPC = &next
		pc = next
	}
	w.StopPC, w.StopReason = pc, "stream_command_budget"
	return w
}

func walkShadowCommandStreamPaths(image romimage.Image, s ShadowCommandStream, first uint32, fetchOperand uint16) []ShadowCommandWalk {
	return walkShadowCommandStreamPathsWithCallbacks(image, s, first, fetchOperand, nil)
}

func walkShadowCommandStreamPathsWithCallbacks(image romimage.Image, s ShadowCommandStream, first uint32, fetchOperand uint16, callbacks *shadowCallbackAnalyzer) []ShadowCommandWalk {
	type state struct {
		w    ShadowCommandWalk
		pc   uint32
		seen map[uint32]bool
	}
	queue := []state{{pc: first, seen: map[uint32]bool{}}}
	var out []ShadowCommandWalk
	for len(queue) != 0 {
		current := queue[0]
		queue = queue[1:]
		part := walkShadowCommandSegment(image, s, current.pc, fetchOperand, shadowCommandWalkLimit-len(current.w.Steps), current.seen)
		part.FirstFetchPC = first
		if callbacks != nil && callbacks.nativeCommands {
			for n := range part.Steps {
				part.Steps[n].ConsumerPaths = callbacks.consumerPaths(s, part.Steps[n])
			}
		}
		part.Steps = append(append([]ShadowCommandWalkStep(nil), current.w.Steps...), part.Steps...)
		if callbacks != nil && part.StopReason == "native_refetch_unproven" && len(part.Steps) != 0 {
			last := len(part.Steps) - 1
			step := part.Steps[last]
			var paths []ShadowCommandCallbackPath
			native := false
			for _, c := range s.Commands {
				if c.Index != step.Index {
					continue
				}
				if c.Callback == nil {
					if callbacks.nativeCommands {
						paths = callbacks.nativePaths(s, c)
						native = true
					}
					continue
				}
				for _, operand := range step.Operands {
					if operand.Kind == "immediate_callback_word" && len(operand.Candidates) == 1 {
						paths = callbacks.paths(s, c, operand.Candidates[0].PC)
					}
				}
			}
			if len(paths) != 0 {
				paths = distinctShadowCommandPathOutcomes(paths)
				if len(out)+len(queue)+len(paths) > shadowCommandWalkPathLimit {
					part.StopReason = "stream_path_budget"
					out = append(out, part)
					continue
				}
				for _, p := range paths {
					child := part
					child.Steps = append([]ShadowCommandWalkStep(nil), part.Steps...)
					if native {
						child.Steps[last].NativePath = &p
					} else {
						child.Steps[last].CallbackPath = &p
					}
					child.StopReason = p.Status
					if p.Status != "owned_native_refetch" && p.Status != "owned_native_stream_refetch" {
						out = append(out, child)
						continue
					}
					y := int(uint16(step.FetchPC)) - int(fetchOperand) + s.EntryCursorDelta + p.CursorDelta
					if p.Status == "owned_native_stream_refetch" {
						read := p.StreamCursorRead
						if read == nil {
							child.StopReason = "stream_cursor_operand_unknown"
							out = append(out, child)
							continue
						}
						bank := byte(step.FetchPC >> 16)
						if read.BankSource == "constant" {
							bank = read.Bank
						}
						addr := int(uint16(step.FetchPC)) - int(fetchOperand) + s.EntryCursorDelta + read.Offset
						word, ok := shadowStreamROMWord(image, bank, uint32(addr))
						if addr < 0 || addr > 0xfffe || !ok || (read.BankSource != "entry_db" && read.BankSource != "constant") {
							child.StopReason = "stream_cursor_operand_not_ROM"
							out = append(out, child)
							continue
						}
						y = int(word) + read.Delta
						child.Steps[last].Operands = append(append([]ShadowCommandROMOperand(nil), child.Steps[last].Operands...), ShadowCommandROMOperand{Kind: "stream_cursor_word", LoadPC: read.LoadPC, SourcePC: uint32(bank)<<16 | uint32(addr), Value: word})
					}
					if y < 0 || y > 0xffff || y+int(fetchOperand) > 0xfffe {
						child.StopReason = "next_cursor_bank_boundary"
						out = append(out, child)
						continue
					}
					next := step.FetchPC&0xff0000 | uint32(y+int(fetchOperand))
					child.Steps[last].NextPC = &next
					seen := map[uint32]bool{}
					for pc, v := range current.seen {
						seen[pc] = v
					}
					queue = append(queue, state{w: child, pc: next, seen: seen})
				}
				continue
			}
		}
		if part.StopReason != "untagged_data_path_unmodeled" || len(s.DataPaths) == 0 {
			out = append(out, part)
			continue
		}
		if len(part.Steps) >= shadowCommandWalkLimit {
			part.StopReason = "stream_command_budget"
			out = append(out, part)
			continue
		}
		// Fail this expansion as a whole rather than selecting a prefix of the
		// native alternatives. A budget stop explicitly leaves work unknown.
		if len(out)+len(queue)+len(s.DataPaths) > shadowCommandWalkPathLimit {
			part.StopReason = "stream_path_budget"
			out = append(out, part)
			continue
		}
		word, ok := shadowStreamROMWord(image, byte(part.StopPC>>16), uint32(uint16(part.StopPC)))
		if !ok {
			part.StopReason = "fetch_not_ROM_or_bank_boundary"
			out = append(out, part)
			continue
		}
		for _, p := range s.DataPaths {
			child := part
			child.Steps = append([]ShadowCommandWalkStep(nil), part.Steps...)
			path := p
			step := ShadowCommandWalkStep{Kind: "untagged_data", FetchPC: part.StopPC, Word: word, Index: -1, Handler: p.EntryPC, DataPath: &path}
			if p.Status != "linear_native_refetch" {
				child.StopReason = "native_data_" + p.Status
				child.Steps = append(child.Steps, step)
				out = append(out, child)
				continue
			}
			y := int(uint16(part.StopPC)) - int(fetchOperand) + p.CursorDelta
			if y < 0 || y > 0xffff || y+int(fetchOperand) > 0xfffe {
				child.StopReason = "next_cursor_bank_boundary"
				child.Steps = append(child.Steps, step)
				out = append(out, child)
				continue
			}
			next := part.StopPC&0xff0000 | uint32(y+int(fetchOperand))
			step.NextPC = &next
			child.Steps = append(child.Steps, step)
			seen := map[uint32]bool{}
			for pc, v := range current.seen {
				seen[pc] = v
			}
			queue = append(queue, state{w: child, pc: next, seen: seen})
		}
	}
	return out
}

func writeShadowCommandWalks(out io.Writer, walks []ShadowCommandWalk) {
	for _, w := range walks {
		fmt.Fprintf(out, "[COMMAND-WALK] selector=%s first=%s roots=%d steps=%d stop=%s reason=%s (conditional ROM references, not proven code roots)\n", shadowAddress(w.SelectorPC), shadowAddress(w.FirstFetchPC), len(w.Origins), len(w.Steps), shadowAddress(w.StopPC), w.StopReason)
		for _, step := range w.Steps {
			if p := step.CallbackPath; p != nil {
				fmt.Fprintf(out, "  callback=%s (conditional native frame evidence)\n", shadowCallbackSummary(*p))
			}
			for _, p := range step.ConsumerPaths {
				fmt.Fprintf(out, "  deferred-consumer=%s callback=%s cursor-publications=%d (conditional native frame evidence)\n", shadowAddress(p.DispatchPC), shadowCallbackSummary(p.Path), len(p.Path.CursorPublications))
			}
			if p := step.DataPath; p != nil {
				fmt.Fprintf(out, "  data=%s native=%s fetch-Y%+d required-branches=%v cursor-publications=%d\n", shadowAddress(step.FetchPC), p.Status, p.CursorDelta, p.Branches, len(p.Publications))
			}
			for _, p := range step.Operands {
				var targets []string
				for _, target := range p.Candidates {
					targets = append(targets, fmt.Sprintf("%s M%dX%d via %s", shadowAddress(target.PC), target.MX.M, target.MX.X, shadowAddress(target.DispatchPC)))
				}
				sort.Strings(targets)
				fmt.Fprintf(out, "  command=$%02X operand=%s word=$%04X kind=%s candidates=%v\n", step.Index, shadowAddress(p.SourcePC), p.Value, p.Kind, targets)
			}
		}
	}
}

func shadowCommandWalkCounts(walks []ShadowCommandWalk) (starts, rawOperands, uniqueOperands, targets int) {
	positions := map[[2]uint32]bool{}
	operands := map[[2]uint32]bool{}
	addresses := map[uint32]bool{}
	for _, w := range walks {
		positions[[2]uint32{w.SelectorPC, w.FirstFetchPC}] = true
		for _, s := range w.Steps {
			for _, p := range s.Operands {
				rawOperands++
				operands[[2]uint32{p.LoadPC, p.SourcePC}] = true
				for _, t := range p.Candidates {
					addresses[t.PC] = true
				}
			}
		}
	}
	return len(positions), rawOperands, len(operands), len(addresses)
}

// Native or callback alternatives that differ only in proof detail (native
// path, required branches, obligations, stop PC) continue a walk identically.
// Keep the first representative of each walk-relevant outcome so equivalent
// branch arms cannot exhaust the path budget on their own. Distinct status,
// entry decimal condition, cursor, stack, DB, stream cursor read, publication
// or owned-return outcomes all remain separate alternatives.
func distinctShadowCommandPathOutcomes(paths []ShadowCommandCallbackPath) []ShadowCommandCallbackPath {
	type outcome struct {
		Status                string
		EntryDecimalCondition string
		CursorDelta           int
		CursorKnown           bool
		StackBytes            int
		DBSource              string
		StreamCursorRead      *ShadowCommandCursorRead
		CursorPublications    []ShadowCommandCursorStore
		Returns               []ShadowCommandCallbackReturn
	}
	seen := map[string]bool{}
	var out []ShadowCommandCallbackPath
	for _, p := range paths {
		key, err := json.Marshal(outcome{p.Status, p.EntryDecimalCondition, p.CursorDelta, p.CursorKnown, p.StackBytes, p.DBSource, p.StreamCursorRead, p.CursorPublications, p.Returns})
		if err != nil {
			out = append(out, p)
			continue
		}
		if seen[string(key)] {
			continue
		}
		seen[string(key)] = true
		out = append(out, p)
	}
	return out
}
